#ifndef NPK_DEVICE_MANAGER_H
#define NPK_DEVICE_MANAGER_H

#include "types.h"

#define NPK_DEVICE_MANAGER_MAX_DEVICES 128U
#define NPK_DEVICE_MANAGER_MAX_DRIVERS 32U
#define NPK_DEVICE_DRIVER_MAX_IDS 32U
#define NPK_DEVICE_TYPE_NAME_CAPACITY 24U
#define NPK_DEVICE_DISPLAY_NAME_CAPACITY 64U
#define NPK_DEVICE_DRIVER_NAME_CAPACITY 32U

typedef enum {
    NPK_DEVICE_BUS_NONE = 0,
    NPK_DEVICE_BUS_PCI = 1,
    NPK_DEVICE_BUS_USB = 2
} npk_device_bus_t;

typedef enum {
    NPK_DEVICE_DISCOVERED = 0,
    NPK_DEVICE_UNBOUND,
    NPK_DEVICE_PROBE_FAILED,
    NPK_DEVICE_BOUND
} npk_device_state_t;

enum {
    NPK_DEVICE_MATCH_VENDOR       = 1U << 0,
    NPK_DEVICE_MATCH_PRODUCT      = 1U << 1,
    NPK_DEVICE_MATCH_CLASS        = 1U << 2,
    NPK_DEVICE_MATCH_SUBCLASS     = 1U << 3,
    NPK_DEVICE_MATCH_PROTOCOL     = 1U << 4,
    NPK_DEVICE_MATCH_INTERFACE    = 1U << 5
};

typedef struct {
    npk_device_bus_t bus;
    /* PCI: packed BDF (bus<<16 | device<<8 | function).
     * USB: host-controller/root-port path, packed by the USB bus driver. */
    uint64_t location;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol; /* PCI programming interface or USB interface protocol. */
    uint8_t revision;
    uint8_t interface_number; /* 0xff when the bus object has no interface. */
} npk_device_identity_t;

struct npk_device;
typedef struct npk_device npk_device_t;
typedef int (*npk_device_probe_fn)(npk_device_t *device);

typedef struct {
    npk_device_bus_t bus;
    uint16_t match_flags;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t interface_number;
} npk_device_id_t;

typedef struct npk_device_driver {
    const char *name;
    const npk_device_id_t *ids;
    size_t id_count;
    npk_device_probe_fn probe;
} npk_device_driver_t;

struct npk_device {
    uint32_t instance_id;
    npk_device_identity_t identity;
    char category[NPK_DEVICE_TYPE_NAME_CAPACITY];
    char display_name[NPK_DEVICE_DISPLAY_NAME_CAPACITY];
    char expected_driver[NPK_DEVICE_DRIVER_NAME_CAPACITY];
    npk_device_state_t state;
    const npk_device_driver_t *driver;
    int last_probe_result;
    uint32_t attempted_driver_mask;
};

/* Boot-time, bounded registry. Calls are currently BSP-only and not IRQ-safe;
 * hotplug drivers must defer discovery/probe work to a serialized worker. */
void devmgr_init(void);
int devmgr_load_default_config(void);
int devmgr_register_driver(const npk_device_driver_t *driver);
npk_device_t *devmgr_register_device(const npk_device_identity_t *identity);
size_t devmgr_device_count(void);
size_t devmgr_driver_count(void);
const npk_device_t *devmgr_device_at(size_t index);
const npk_device_driver_t *devmgr_driver_at(size_t index);

#endif
