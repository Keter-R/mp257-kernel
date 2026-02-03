#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/thermal.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/pwm.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/freezer.h>
#include "fan_controller_ioctl.h"

#define DRIVER_NAME "fan_controller"
#define CLASS_NAME  "fan_controller_class"
#define STABILIZATION_CYCLES_REQUIRED 3

enum temp_trend { TEMP_TREND_STABLE, TEMP_TREND_RISING, TEMP_TREND_DROPPING };

struct fan_thermal_data {
    struct platform_device *pdev;
    struct thermal_zone_device *tz;
    
    /* PWM 动态管理相关 */
    struct pwm_device *pwm;
    struct mutex pwm_lock; /* 保护 pwm 指针的申请与释放 */
    
    struct task_struct *monitor_thread;
    struct fan_config config;
    struct mutex config_lock;
    int active_level_idx;
    int stabilization_count;
    int last_temp;
    enum temp_trend trend;
    u32 polling_ms;
    struct cdev cdev;
    dev_t devt;
    struct class *class;
    bool is_suspended;
};

/* 
 * 核心函数：动态 PWM 控制
 * 逻辑：
 * 1. speed > 0: 检查是否持有 PWM，若无则申请(pwm_get)，然后设置占空比。
 * 2. speed <= 0: 检查是否持有 PWM，若有则 Disable 并释放(pwm_put)，指针置空。
 */
static void fan_actuator_apply_speed(struct fan_thermal_data *data, int speed_percent)
{
    struct pwm_state state;
    u64 inverted_duty_cycle;
    int ret;

    /* 加锁防止多线程或休眠回调竞争 PWM 资源 */
    mutex_lock(&data->pwm_lock);

    if (speed_percent <= 0) {
        /* 停转逻辑：释放资源 */
        if (data->pwm) {
            /* 先获取当前状态，优雅关闭 */
            pwm_get_state(data->pwm, &state);
            if (state.enabled) {
                state.enabled = false;
                state.duty_cycle = 0;
                pwm_apply_might_sleep(data->pwm, &state);
            }
            
            /* 关键：释放 PWM 引用，允许 STM32 PWM 驱动休眠 */
            pwm_put(data->pwm);
            data->pwm = NULL;
        }
    } else {
        /* 运转逻辑：按需申请 */
        if (!data->pwm) {
            data->pwm = pwm_get(&data->pdev->dev, NULL);
            if (IS_ERR(data->pwm)) {
                dev_err(&data->pdev->dev, "Failed to acquire PWM dynamically: %ld\n", PTR_ERR(data->pwm));
                data->pwm = NULL;
                mutex_unlock(&data->pwm_lock);
                return;
            }
        }

        if (speed_percent > 100) speed_percent = 100;

        /* 获取默认周期，如果未设置则给个默认值 (例如 25kHz -> 40000ns) */
        pwm_get_state(data->pwm, &state);
        if (state.period == 0) state.period = 40000; 

        /* 计算占空比 (假设是反向逻辑：Duty 越小转速越快？根据实际情况调整) */
        /* 如果是正向逻辑： duty = period * speed / 100 */
        /* 这里保留你原代码的反向逻辑： */
        inverted_duty_cycle = state.period - ((state.period * speed_percent) / 100);
        state.duty_cycle = inverted_duty_cycle;
        state.enabled = true;

        ret = pwm_apply_might_sleep(data->pwm, &state);
        if (ret) {
            dev_err(&data->pdev->dev, "Failed to apply PWM state: %d\n", ret);
            /* 如果应用失败，尝试释放资源以防状态不一致 */
            pwm_put(data->pwm);
            data->pwm = NULL;
        }
    }

    mutex_unlock(&data->pwm_lock);
}

static void fan_actuator_set_speed(struct fan_thermal_data *data, int speed_percent)
{
    /* 如果系统已挂起，禁止操作硬件 */
    if (data->is_suspended) return;
    fan_actuator_apply_speed(data, speed_percent);
}

static void fan_policy_engine_update(struct fan_thermal_data *data, int current_temp)
{
    struct fan_config local_config;
    int target_level_idx = -1, i;
    
    mutex_lock(&data->config_lock);
    memcpy(&local_config, &data->config, sizeof(local_config));
    mutex_unlock(&data->config_lock);
    
    if (local_config.num_levels == 0) { fan_actuator_set_speed(data, 0); return; }
    
    for (i = local_config.num_levels - 1; i >= 0; i--) {
        if (current_temp >= local_config.levels[i].temp) { target_level_idx = i; break; }
    }
    
    if (target_level_idx > data->active_level_idx) {
        data->active_level_idx = target_level_idx;
        data->stabilization_count = 0;
        fan_actuator_set_speed(data, local_config.levels[target_level_idx].fan_speed);
    } else if (target_level_idx < data->active_level_idx) {
        const struct fan_level_config *active_level = &local_config.levels[data->active_level_idx];
        if (current_temp < (active_level->temp - active_level->hyst)) {
            if (data->trend == TEMP_TREND_RISING) data->stabilization_count = 0;
            else data->stabilization_count++;
            
            if (data->stabilization_count >= STABILIZATION_CYCLES_REQUIRED) {
                data->active_level_idx = target_level_idx;
                data->stabilization_count = 0;
                int new_speed = (target_level_idx == -1) ? 0 : local_config.levels[target_level_idx].fan_speed;
                fan_actuator_set_speed(data, new_speed);
            }
        } else { data->stabilization_count = 0; }
    } else { data->stabilization_count = 0; }
}

static int fan_monitor_thread(void *priv)
{
    struct fan_thermal_data *data = priv;
    int current_temp, ret;
    u32 local_polling_ms;

    set_freezable();
    while (!kthread_should_stop()) {
        /* 响应系统休眠冻结 */
        if (try_to_freeze())
            continue;
        
        if (data->is_suspended) {
            msleep_interruptible(100);
            continue;
        }
        
        ret = thermal_zone_get_temp(data->tz, &current_temp);
        if (ret) {
            msleep_interruptible(data->polling_ms);
            continue;
        }
        
        if (current_temp > data->last_temp) data->trend = TEMP_TREND_RISING;
        else if (current_temp < data->last_temp) data->trend = TEMP_TREND_DROPPING;
        else data->trend = TEMP_TREND_STABLE;
        data->last_temp = current_temp;

        fan_policy_engine_update(data, current_temp);

        mutex_lock(&data->config_lock);
        local_polling_ms = data->polling_ms;
        mutex_unlock(&data->config_lock);

        msleep_interruptible(local_polling_ms);
    }
    return 0;
}

static long fan_thermal_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct fan_thermal_data *data = f->private_data;
    int ret = 0;
    switch (cmd) {
    case FAN_CTRL_IOC_SET_CONFIG: {
        struct fan_control_package new_pkg;
        if (copy_from_user(&new_pkg, (void __user *)arg, sizeof(new_pkg))) return -EFAULT;
        if (new_pkg.config.num_levels > MAX_FAN_LEVELS) return -EINVAL;
        mutex_lock(&data->config_lock);
        memcpy(&data->config, &new_pkg.config, sizeof(data->config));
        data->polling_ms = new_pkg.polling_ms;
        data->active_level_idx = -1;
        mutex_unlock(&data->config_lock);
        if (data->monitor_thread) wake_up_process(data->monitor_thread);
        break;
    }
    case FAN_CTRL_IOC_GET_CONFIG: {
        mutex_lock(&data->config_lock);
        if (copy_to_user((void __user *)arg, &data->config, sizeof(data->config))) ret = -EFAULT;
        mutex_unlock(&data->config_lock);
        break;
    }
    default: ret = -ENOTTY;
    }
    return ret;
}

static int fan_thermal_open(struct inode *inode, struct file *f) { 
    f->private_data = container_of(inode->i_cdev, struct fan_thermal_data, cdev); 
    return 0; 
}

static const struct file_operations fan_fops = { 
    .owner = THIS_MODULE, 
    .open = fan_thermal_open, 
    .unlocked_ioctl = fan_thermal_ioctl 
};

static int fan_thermal_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct fan_thermal_data *data;
    const char *zone_name;
    int ret;
    struct pwm_device *temp_pwm;

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data) return -ENOMEM;

    data->pdev = pdev;
    platform_set_drvdata(pdev, data);
    mutex_init(&data->config_lock);
    mutex_init(&data->pwm_lock); /* 初始化 PWM 锁 */

    data->polling_ms = 2000;
    data->config.num_levels = 1;
    data->config.levels[0].temp = 120000; 
    data->config.levels[0].hyst = 0; 
    data->config.levels[0].fan_speed = 0;

    ret = of_property_read_string(dev->of_node, "monitored-zone", &zone_name);
    if (ret) { dev_err(dev, "Failed to get 'monitored-zone' property\n"); return ret; }

    data->tz = thermal_zone_get_zone_by_name(zone_name);
    if (IS_ERR(data->tz)) { dev_err(dev, "Could not get thermal zone '%s'\n", zone_name); return PTR_ERR(data->tz); }

    /* 
     * 探测阶段：尝试获取一次 PWM 以验证设备树配置是否正确。
     * 获取后立即释放，不长期持有。
     */
    temp_pwm = pwm_get(dev, NULL);
    if (IS_ERR(temp_pwm)) {
        dev_err(dev, "Probe check: Could not get PWM device. Check DT.\n");
        return PTR_ERR(temp_pwm);
    }
    pwm_put(temp_pwm);
    data->pwm = NULL; /* 确保初始状态为空 */

    data->active_level_idx = -1;

    ret = alloc_chrdev_region(&data->devt, 0, 1, DRIVER_NAME);
    if (ret < 0) { dev_err(dev, "Failed to allocate chrdev region\n"); return ret; }

    cdev_init(&data->cdev, &fan_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->devt, 1);
    if (ret < 0) { dev_err(dev, "Failed to add cdev\n"); goto unreg_chrdev; }

    data->class = class_create(CLASS_NAME);
    if (IS_ERR(data->class)) { ret = PTR_ERR(data->class); dev_err(dev, "Failed to create device class\n"); goto del_cdev; }

    device_create(data->class, dev, data->devt, NULL, DRIVER_NAME);

    data->monitor_thread = kthread_run(fan_monitor_thread, data, "fan_monitor");
    if (IS_ERR(data->monitor_thread)) {
        ret = PTR_ERR(data->monitor_thread);
        dev_err(dev, "Failed to create kthread\n");
        goto destroy_device;
    }

    dev_info(dev, "Fan controller initialized (Dynamic PWM mode)\n");
    return 0;

destroy_device: device_destroy(data->class, data->devt); class_destroy(data->class);
del_cdev: cdev_del(&data->cdev);
unreg_chrdev: unregister_chrdev_region(data->devt, 1);
    return ret;
}

static int fan_thermal_suspend(struct device *dev)
{
    struct fan_thermal_data *data = dev_get_drvdata(dev);

    if (!data) return 0;

    data->is_suspended = true;

    /* 
     * 关键步骤：
     * 设置速度为 0，这将触发 fan_actuator_apply_speed 中的逻辑：
     * 1. Disable PWM
     * 2. pwm_put() 释放引用
     * 
     * 这样当内核继续执行 Suspend 流程到达 STM32 PWM 驱动时，
     * 发现没有 Consumer 占用，即可成功休眠。
     */
    fan_actuator_apply_speed(data, 0);

    dev_info(dev, "Fan controller suspended: PWM released\n");
    return 0;
}

static int fan_thermal_resume(struct device *dev)
{
    struct fan_thermal_data *data = dev_get_drvdata(dev);

    if (!data) return 0;

    /* 
     * 恢复时不需要立即申请 PWM。
     * 只需要清除标志位，唤醒监控线程。
     * 线程下一次循环检测到温度需要风扇转动时，
     * 会自动调用 fan_actuator_apply_speed -> pwm_get。
     */
    data->is_suspended = false;

    if (data->monitor_thread)
        wake_up_process(data->monitor_thread);

    dev_info(dev, "Fan controller resumed\n");
    return 0;
}

static SIMPLE_DEV_PM_OPS(fan_thermal_pm_ops, fan_thermal_suspend, fan_thermal_resume);

static int fan_thermal_remove(struct platform_device *pdev)
{
    struct fan_thermal_data *data = platform_get_drvdata(pdev);
    
    if (data->monitor_thread) kthread_stop(data->monitor_thread);
    
    /* 确保退出前释放 PWM */
    fan_actuator_apply_speed(data, 0);
    
    device_destroy(data->class, data->devt);
    class_destroy(data->class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->devt, 1);

    dev_info(&pdev->dev, "Fan controller driver removed\n");
    return 0;
}

static const struct of_device_id fan_thermal_of_match[] = {
    { .compatible = "atk,fan-thermal-controller", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fan_thermal_of_match);

static struct platform_driver fan_thermal_driver = {
    .driver = { 
        .name = DRIVER_NAME, 
        .of_match_table = fan_thermal_of_match,
        .pm = &fan_thermal_pm_ops
    },
    .probe = fan_thermal_probe, .remove = fan_thermal_remove,
};

module_platform_driver(fan_thermal_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KARLIS");
MODULE_DESCRIPTION("Definitive Final Fan Controller (Dynamic PWM)");



// PLAN  B

// #include <linux/module.h>
// #include <linux/platform_device.h>
// #include <linux/of.h>
// #include <linux/thermal.h>
// #include <linux/kthread.h>
// #include <linux/mutex.h>
// #include <linux/pwm.h>
// #include <linux/fs.h>
// #include <linux/cdev.h>
// #include <linux/uaccess.h>
// #include <linux/slab.h>
// #include <linux/delay.h>
// #include <linux/pm_runtime.h>
// #include <linux/freezer.h>
// #include "fan_controller_ioctl.h"

// #define DRIVER_NAME "fan_controller"
// #define CLASS_NAME  "fan_controller_class"
// #define STABILIZATION_CYCLES_REQUIRED 3

// enum temp_trend { TEMP_TREND_STABLE, TEMP_TREND_RISING, TEMP_TREND_DROPPING };

// struct fan_thermal_data {
//     struct platform_device *pdev;
//     struct thermal_zone_device *tz;
    
//     /* 
//      * 注意：这里不再保存 struct pwm_device *pwm 指针。
//      * 我们只保存必要的参数，以便每次临时申请 PWM 时使用。
//      */
//     u64 pwm_period; 

//     struct task_struct *monitor_thread;
//     struct fan_config config;
//     struct mutex config_lock;     // 保护配置数据
//     struct mutex actuator_lock;   // 保护 PWM 操作序列
    
//     int active_level_idx;
//     int stabilization_count;
//     int last_temp;
//     enum temp_trend trend;
//     u32 polling_ms;
    
//     struct cdev cdev;
//     dev_t devt;
//     struct class *class;
    
//     bool is_suspended;
// };

// /* 
//  * 核心执行函数：即用即弃模式 (Stateless PWM Operation)
//  * 流程：Get -> Set -> Apply -> Put
//  * 
//  * 优点：
//  * 1. 硬件寄存器保持输出，风扇继续转。
//  * 2. 引用计数归零，不阻碍底层 PWM 控制器休眠。
//  */
// static void fan_actuator_apply_speed(struct fan_thermal_data *data, int speed_percent)
// {
//     struct pwm_device *pwm_temp;
//     struct pwm_state state;
//     u64 inverted_duty_cycle;
//     int ret;

//     /* 互斥锁保护：防止多线程同时申请/释放同一个 PWM 通道 */
//     mutex_lock(&data->actuator_lock);

//     /* 1. 临时申请 PWM 设备 */
//     pwm_temp = pwm_get(&data->pdev->dev, NULL);
//     if (IS_ERR(pwm_temp)) {
//         /* 
//          * 如果申请失败（例如底层驱动已卸载或出错），
//          * 我们只能打印错误并返回，不能强行操作。
//          */
//         dev_err_ratelimited(&data->pdev->dev, "Failed to get PWM device: %ld\n", PTR_ERR(pwm_temp));
//         mutex_unlock(&data->actuator_lock);
//         return;
//     }

//     /* 2. 获取当前状态或初始化状态 */
//     pwm_init_state(pwm_temp, &state);

//     /* 确保周期正确（使用 Probe 时缓存的值或默认值） */
//     if (data->pwm_period > 0) {
//         state.period = data->pwm_period;
//     } else if (state.period == 0) {
//         state.period = 40000; // 25kHz fallback
//     }

//     /* 3. 计算占空比 */
//     if (speed_percent <= 0) {
//         state.enabled = false;
//         state.duty_cycle = 0;
//     } else {
//         if (speed_percent > 100) speed_percent = 100;
        
//         // 反向逻辑 (根据硬件调整：通常 Period - Duty = Low Active)
//         inverted_duty_cycle = state.period - ((state.period * speed_percent) / 100);
//         state.duty_cycle = inverted_duty_cycle;
//         state.enabled = true;
//     }

//     /* 4. 应用配置 (写硬件寄存器) */
//     ret = pwm_apply_might_sleep(pwm_temp, &state);
//     if (ret) {
//         dev_err(&data->pdev->dev, "Failed to apply PWM state: %d\n", ret);
//     }

//     /* 
//      * 5. 立即释放 PWM 设备 
//      * 此时硬件寄存器已写入，波形会保持（取决于硬件特性）。
//      * 引用计数归零。
//      */
//     pwm_put(pwm_temp);

//     mutex_unlock(&data->actuator_lock);
// }

// static void fan_actuator_set_speed(struct fan_thermal_data *data, int speed_percent)
// {
//     // 休眠期间不操作硬件，避免唤醒或竞争
//     if (data->is_suspended) return;
//     fan_actuator_apply_speed(data, speed_percent);
// }

// static void fan_policy_engine_update(struct fan_thermal_data *data, int current_temp)
// {
//     struct fan_config local_config;
//     int target_level_idx = -1, i;
    
//     mutex_lock(&data->config_lock);
//     memcpy(&local_config, &data->config, sizeof(local_config));
//     mutex_unlock(&data->config_lock);
    
//     if (local_config.num_levels == 0) { fan_actuator_set_speed(data, 0); return; }
    
//     for (i = local_config.num_levels - 1; i >= 0; i--) {
//         if (current_temp >= local_config.levels[i].temp) { target_level_idx = i; break; }
//     }
    
//     if (target_level_idx > data->active_level_idx) {
//         data->active_level_idx = target_level_idx;
//         data->stabilization_count = 0;
//         fan_actuator_set_speed(data, local_config.levels[target_level_idx].fan_speed);
//     } else if (target_level_idx < data->active_level_idx) {
//         const struct fan_level_config *active_level = &local_config.levels[data->active_level_idx];
//         if (current_temp < (active_level->temp - active_level->hyst)) {
//             if (data->trend == TEMP_TREND_RISING) data->stabilization_count = 0;
//             else data->stabilization_count++;
            
//             if (data->stabilization_count >= STABILIZATION_CYCLES_REQUIRED) {
//                 data->active_level_idx = target_level_idx;
//                 data->stabilization_count = 0;
//                 int new_speed = (target_level_idx == -1) ? 0 : local_config.levels[target_level_idx].fan_speed;
//                 fan_actuator_set_speed(data, new_speed);
//             }
//         } else { data->stabilization_count = 0; }
//     } else { data->stabilization_count = 0; }
// }

// static int fan_monitor_thread(void *priv)
// {
//     struct fan_thermal_data *data = priv;
//     int current_temp, ret;
//     u32 local_polling_ms;

//     set_freezable();
//     while (!kthread_should_stop()) {
//         /* 响应系统休眠冻结 */
//         if (try_to_freeze())
//             continue;
        
//         if (data->is_suspended) {
//             msleep_interruptible(100);
//             continue;
//         }
        
//         ret = thermal_zone_get_temp(data->tz, &current_temp);
//         if (ret) {
//             msleep_interruptible(data->polling_ms);
//             continue;
//         }
        
//         if (current_temp > data->last_temp) data->trend = TEMP_TREND_RISING;
//         else if (current_temp < data->last_temp) data->trend = TEMP_TREND_DROPPING;
//         else data->trend = TEMP_TREND_STABLE;
//         data->last_temp = current_temp;

//         fan_policy_engine_update(data, current_temp);

//         mutex_lock(&data->config_lock);
//         local_polling_ms = data->polling_ms;
//         mutex_unlock(&data->config_lock);

//         msleep_interruptible(local_polling_ms);
//     }
//     return 0;
// }

// static long fan_thermal_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
// {
//     struct fan_thermal_data *data = f->private_data;
//     int ret = 0;
//     switch (cmd) {
//     case FAN_CTRL_IOC_SET_CONFIG: {
//         struct fan_control_package new_pkg;
//         if (copy_from_user(&new_pkg, (void __user *)arg, sizeof(new_pkg))) return -EFAULT;
//         if (new_pkg.config.num_levels > MAX_FAN_LEVELS) return -EINVAL;
//         mutex_lock(&data->config_lock);
//         memcpy(&data->config, &new_pkg.config, sizeof(data->config));
//         data->polling_ms = new_pkg.polling_ms;
//         data->active_level_idx = -1;
//         mutex_unlock(&data->config_lock);
//         if (data->monitor_thread) wake_up_process(data->monitor_thread);
//         break;
//     }
//     case FAN_CTRL_IOC_GET_CONFIG: {
//         mutex_lock(&data->config_lock);
//         if (copy_to_user((void __user *)arg, &data->config, sizeof(data->config))) ret = -EFAULT;
//         mutex_unlock(&data->config_lock);
//         break;
//     }
//     default: ret = -ENOTTY;
//     }
//     return ret;
// }

// static int fan_thermal_open(struct inode *inode, struct file *f) { f->private_data = container_of(inode->i_cdev, struct fan_thermal_data, cdev); return 0; }
// static const struct file_operations fan_fops = { .owner = THIS_MODULE, .open = fan_thermal_open, .unlocked_ioctl = fan_thermal_ioctl };

// static int fan_thermal_probe(struct platform_device *pdev)
// {
//     struct device *dev = &pdev->dev;
//     struct fan_thermal_data *data;
//     const char *zone_name;
//     int ret;
//     struct pwm_device *temp_pwm;
//     struct pwm_state state;

//     data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
//     if (!data) return -ENOMEM;

//     data->pdev = pdev;
//     platform_set_drvdata(pdev, data);
//     mutex_init(&data->config_lock);
//     mutex_init(&data->actuator_lock);

//     data->polling_ms = 2000;
//     data->config.num_levels = 1;
//     data->config.levels[0].temp = 120000; data->config.levels[0].hyst = 0; data->config.levels[0].fan_speed = 0;

//     ret = of_property_read_string(dev->of_node, "monitored-zone", &zone_name);
//     if (ret) { dev_err(dev, "Failed to get 'monitored-zone' property\n"); return ret; }

//     data->tz = thermal_zone_get_zone_by_name(zone_name);
//     if (IS_ERR(data->tz)) { dev_err(dev, "Could not get thermal zone '%s'\n", zone_name); return PTR_ERR(data->tz); }

//     /* 
//      * 探测阶段：
//      * 1. 验证 PWM 是否可用。
//      * 2. 获取设备树中配置的 Period 并缓存下来。
//      * 3. 立即释放。
//      */
//     temp_pwm = pwm_get(dev, NULL);
//     if (IS_ERR(temp_pwm)) { 
//         dev_err(dev, "Could not get PWM device (probe check)\n"); 
//         return PTR_ERR(temp_pwm); 
//     }
    
//     pwm_get_state(temp_pwm, &state);
//     data->pwm_period = state.period;
//     if (data->pwm_period == 0) {
//         dev_warn(dev, "PWM period is 0, using default 40000ns\n");
//         data->pwm_period = 40000;
//     }
    
//     /* 确保初始状态是关闭的，或者根据需要保持 */
//     fan_actuator_apply_speed(data, 0); 
    
//     /* 立即释放，不持有 */
//     pwm_put(temp_pwm);

//     data->active_level_idx = -1;

//     ret = alloc_chrdev_region(&data->devt, 0, 1, DRIVER_NAME);
//     if (ret < 0) { dev_err(dev, "Failed to allocate chrdev region\n"); return ret; }

//     cdev_init(&data->cdev, &fan_fops);
//     data->cdev.owner = THIS_MODULE;
//     ret = cdev_add(&data->cdev, data->devt, 1);
//     if (ret < 0) { dev_err(dev, "Failed to add cdev\n"); goto unreg_chrdev; }

//     data->class = class_create(CLASS_NAME);
//     if (IS_ERR(data->class)) { ret = PTR_ERR(data->class); dev_err(dev, "Failed to create device class\n"); goto del_cdev; }

//     device_create(data->class, dev, data->devt, NULL, DRIVER_NAME);

//     data->monitor_thread = kthread_run(fan_monitor_thread, data, "fan_monitor");
//     if (IS_ERR(data->monitor_thread)) {
//         ret = PTR_ERR(data->monitor_thread);
//         dev_err(dev, "Failed to create kthread\n");
//         goto destroy_device;
//     }

//     dev_info(dev, "Fan controller initialized (Stateless PWM Mode)\n");
//     return 0;

// destroy_device: device_destroy(data->class, data->devt); class_destroy(data->class);
// del_cdev: cdev_del(&data->cdev);
// unreg_chrdev: unregister_chrdev_region(data->devt, 1);
//     return ret;
// }

// /*
//  * Suspend 回调
//  * 
//  * 此时我们不持有 PWM 句柄 (refcount=0)。
//  * 我们不需要做任何 PWM 操作。
//  * 
//  * 1. 标记 is_suspended = true，阻止线程操作。
//  * 2. 内核会继续遍历设备链表，找到 STM32 PWM 控制器。
//  * 3. STM32 PWM 驱动发现 refcount=0，成功执行 suspend (通常会 disable 硬件输出)。
//  */
// static int fan_thermal_suspend(struct device *dev)
// {
//     struct fan_thermal_data *data = dev_get_drvdata(dev);
//     if (data) {
//         data->is_suspended = true;
//         dev_info(dev, "Fan controller suspended (PWM released)\n");
//     }
//     return 0;
// }

// /*
//  * Resume 回调
//  * 
//  * 1. 标记 is_suspended = false。
//  * 2. 唤醒监控线程。
//  * 3. 线程醒来后，会读取温度，重新计算速度，并调用 fan_actuator_apply_speed。
//  * 4. fan_actuator_apply_speed 会重新 pwm_get -> enable -> pwm_put。
//  * 5. 风扇恢复转动。
//  */
// static int fan_thermal_resume(struct device *dev)
// {
//     struct fan_thermal_data *data = dev_get_drvdata(dev);
//     if (data) {
//         data->is_suspended = false;
//         if (data->monitor_thread)
//             wake_up_process(data->monitor_thread);
//         dev_info(dev, "Fan controller resumed\n");
//     }
//     return 0;
// }

// static SIMPLE_DEV_PM_OPS(fan_thermal_pm_ops, fan_thermal_suspend, fan_thermal_resume);

// static int fan_thermal_remove(struct platform_device *pdev)
// {
//     struct fan_thermal_data *data = platform_get_drvdata(pdev);
    
//     if (data->monitor_thread) kthread_stop(data->monitor_thread);
    
//     /* 退出前确保风扇停转 */
//     fan_actuator_apply_speed(data, 0);
    
//     device_destroy(data->class, data->devt);
//     class_destroy(data->class);
//     cdev_del(&data->cdev);
//     unregister_chrdev_region(data->devt, 1);

//     dev_info(&pdev->dev, "Fan controller driver removed\n");
//     return 0;
// }

// static const struct of_device_id fan_thermal_of_match[] = {
//     { .compatible = "atk,fan-thermal-controller", },
//     { /* sentinel */ }
// };
// MODULE_DEVICE_TABLE(of, fan_thermal_of_match);

// static struct platform_driver fan_thermal_driver = {
//     .driver = { 
//         .name = DRIVER_NAME, 
//         .of_match_table = fan_thermal_of_match,
//         .pm = &fan_thermal_pm_ops
//     },
//     .probe = fan_thermal_probe, .remove = fan_thermal_remove,
// };

// module_platform_driver(fan_thermal_driver);

// MODULE_LICENSE("GPL");
// MODULE_AUTHOR("KARLIS");
// MODULE_DESCRIPTION("Stateless Fan Controller for STM32MP2");
