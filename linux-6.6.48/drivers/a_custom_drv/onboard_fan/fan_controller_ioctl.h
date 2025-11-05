// fan_controller_ioctl.h

#ifndef FAN_CONTROLLER_IOCTL_H
#define FAN_CONTROLLER_IOCTL_H

#include <linux/ioctl.h>

#define MAX_FAN_LEVELS 10

// 描述一个温度触发等级
struct fan_level_config {
    int temp;      // 触发温度 (millicelsius)
    int hyst;      // 迟滞温度 (millicelsius)
    int fan_speed; // 风扇速度 (0-100%)
};

// 描述风扇的温度/速度曲线
struct fan_config {
    unsigned int num_levels;
    struct fan_level_config levels[MAX_FAN_LEVELS];
};

// *** OPTIMIZATION: New comprehensive structure for IOCTL ***
// This package contains ALL settings that can be configured from user space.
struct fan_control_package {
    struct fan_config config; // The fan curve
    unsigned int polling_ms;  // The temperature polling interval
};

// IOCTL command definitions
#define FAN_CTRL_IOC_MAGIC 'f'

// The SET command now uses the new, comprehensive package.
#define FAN_CTRL_IOC_SET_CONFIG _IOW(FAN_CTRL_IOC_MAGIC, 1, struct fan_control_package)

// The GET command can be updated as well for consistency, or left as is if not needed.
#define FAN_CTRL_IOC_GET_CONFIG _IOR(FAN_CTRL_IOC_MAGIC, 2, struct fan_config)

#endif // FAN_CONTROLLER_IOCTL_H