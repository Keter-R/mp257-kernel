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

#include "fan_controller_ioctl.h"

#define DRIVER_NAME "fan_controller"
#define CLASS_NAME  "fan_controller_class"
#define STABILIZATION_CYCLES_REQUIRED 3

enum temp_trend { TEMP_TREND_STABLE, TEMP_TREND_RISING, TEMP_TREND_DROPPING };

struct fan_thermal_data {
	struct platform_device *pdev;
	struct thermal_zone_device *tz;
	struct pwm_device *pwm;
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
};


static void fan_actuator_set_speed(struct fan_thermal_data *data, int speed_percent)
{
	struct pwm_state state;
	u64 inverted_duty_cycle;
	int ret;

	if (!data->pwm) return;

	pwm_get_state(data->pwm, &state);
	if (state.period == 0) state.period = 40000; // 25kHz fallback
	if (speed_percent < 0) speed_percent = 0;
	if (speed_percent > 100) speed_percent = 100;

	inverted_duty_cycle = state.period - ((state.period * speed_percent) / 100);
	state.duty_cycle = inverted_duty_cycle;
	state.enabled = true;

	ret = pwm_apply_might_sleep(data->pwm, &state);
	if (ret) {
		dev_err(&data->pdev->dev, "Failed to apply PWM state: %d\n", ret);
	}
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

	while (!kthread_should_stop()) {
		ret = thermal_zone_get_temp(data->tz, &current_temp);
		if (ret) {
			pr_err(DRIVER_NAME ": Failed to read temperature\n");
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
	case FAN_CTRL_IOC_SET_CONFIG:
	{

		struct fan_control_package new_pkg;

		if (copy_from_user(&new_pkg, (void __user *)arg, sizeof(new_pkg))) {
			pr_err(DRIVER_NAME ": ioctl SET_CONFIG - copy_from_user failed\n");
			return -EFAULT;
		}

		if (new_pkg.config.num_levels > MAX_FAN_LEVELS) {
			pr_warn(DRIVER_NAME ": ioctl SET_CONFIG - num_levels (%u) exceeds MAX_FAN_LEVELS (%u)\n",
				new_pkg.config.num_levels, MAX_FAN_LEVELS);
			return -EINVAL;
		}

		mutex_lock(&data->config_lock);


		memcpy(&data->config, &new_pkg.config, sizeof(data->config));
		data->polling_ms = new_pkg.polling_ms;
		data->active_level_idx = -1;
		mutex_unlock(&data->config_lock);

		pr_info(DRIVER_NAME ": New config loaded: %u levels, polling %u ms\n",
			new_pkg.config.num_levels, new_pkg.polling_ms);

		if (data->monitor_thread)
			wake_up_process(data->monitor_thread);

		break;
	}
	case FAN_CTRL_IOC_GET_CONFIG:
	{

		mutex_lock(&data->config_lock);
		if (copy_to_user((void __user *)arg, &data->config, sizeof(data->config))) {
			pr_err(DRIVER_NAME ": ioctl GET_CONFIG - copy_to_user failed\n");
			ret = -EFAULT;
		}
		mutex_unlock(&data->config_lock);
		break;
	}
	default:
		pr_warn(DRIVER_NAME ": Received unknown ioctl command: 0x%x\n", cmd);
		ret = -ENOTTY;
	}

	return ret;
}


static int fan_thermal_open(struct inode *inode, struct file *f) { f->private_data = container_of(inode->i_cdev, struct fan_thermal_data, cdev); return 0; }
static const struct file_operations fan_fops = { .owner = THIS_MODULE, .open = fan_thermal_open, .unlocked_ioctl = fan_thermal_ioctl };

static int fan_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fan_thermal_data *data;
	const char *zone_name;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) return -ENOMEM;

	data->pdev = pdev;
	platform_set_drvdata(pdev, data);
	mutex_init(&data->config_lock);


	data->polling_ms = 2000;
	data->config.num_levels = 1;
	data->config.levels[0].temp = 120000; data->config.levels[0].hyst = 0; data->config.levels[0].fan_speed = 0;

	ret = of_property_read_string(dev->of_node, "monitored-zone", &zone_name);
	if (ret) { dev_err(dev, "Failed to get 'monitored-zone' property\n"); return ret; }

	data->tz = thermal_zone_get_zone_by_name(zone_name);
	if (IS_ERR(data->tz)) { dev_err(dev, "Could not get thermal zone '%s'\n", zone_name); return PTR_ERR(data->tz); }

	data->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(data->pwm)) { dev_err(dev, "Could not get PWM device\n"); return PTR_ERR(data->pwm); }

	if (!data->pwm->chip || !data->pwm->chip->dev) {
		dev_err(dev, "PWM chip or its device is not available\n");
		return -ENODEV;
	}
	pm_runtime_enable(data->pwm->chip->dev);
	ret = pm_runtime_get_sync(data->pwm->chip->dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync on pwm device failed: %d\n", ret);
		pm_runtime_disable(data->pwm->chip->dev);
		return ret;
	}

	fan_actuator_set_speed(data, 0);
	data->active_level_idx = -1;

	ret = alloc_chrdev_region(&data->devt, 0, 1, DRIVER_NAME);
	if (ret < 0) { dev_err(dev, "Failed to allocate chrdev region\n"); goto pm_put_exit; }

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

	dev_info(dev, "Fan controller driver initialized successfully\n");
	return 0;

destroy_device: device_destroy(data->class, data->devt); class_destroy(data->class);
del_cdev: cdev_del(&data->cdev);
unreg_chrdev: unregister_chrdev_region(data->devt, 1);
pm_put_exit: pm_runtime_put_sync(data->pwm->chip->dev); pm_runtime_disable(data->pwm->chip->dev);
	return ret;
}

static int fan_thermal_remove(struct platform_device *pdev)
{
	struct fan_thermal_data *data = platform_get_drvdata(pdev);
	if (data->monitor_thread) kthread_stop(data->monitor_thread);
	fan_actuator_set_speed(data, 0);
	device_destroy(data->class, data->devt);
	class_destroy(data->class);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devt, 1);

	if (data->pwm && data->pwm->chip && data->pwm->chip->dev) {
		pm_runtime_put_sync(data->pwm->chip->dev);
		pm_runtime_disable(data->pwm->chip->dev);
	}

	dev_info(&pdev->dev, "Fan controller driver removed\n");
	return 0;
}

static const struct of_device_id fan_thermal_of_match[] = {
	{ .compatible = "atk,fan-thermal-controller", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fan_thermal_of_match);

static struct platform_driver fan_thermal_driver = {
	.driver = { .name = DRIVER_NAME, .of_match_table = fan_thermal_of_match },
	.probe = fan_thermal_probe, .remove = fan_thermal_remove,
};

module_platform_driver(fan_thermal_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KARLIS");
MODULE_DESCRIPTION("Definitive Final Fan Controller (with manual inversion for non-standard SoCs)");