#ifndef NPK_DEVICE_CONFIG_H
#define NPK_DEVICE_CONFIG_H

#include <npk/device_manager.h>

typedef struct {
    char type[NPK_DEVICE_TYPE_NAME_CAPACITY];
    char display_name[NPK_DEVICE_DISPLAY_NAME_CAPACITY];
    char driver_name[NPK_DEVICE_DRIVER_NAME_CAPACITY];
} npk_device_config_match_t;

/* Reads and validates a bounded JSON catalog through the VFS API. */
int device_config_load_vfs(const char *path);
void device_config_reset(void);
bool device_config_resolve(const npk_device_identity_t *identity,
                           npk_device_config_match_t *match);

#endif
