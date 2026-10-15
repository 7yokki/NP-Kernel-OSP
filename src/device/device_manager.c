#include <npk/device_manager.h>
#include <npk/device_config.h>
#include <npk/log.h>
#include <npk/string.h>

#define NPK_DEVICE_MANAGER_EINVAL 22
#define NPK_DEVICE_MANAGER_ENOSPC 28
#define NPK_DEVICE_MANAGER_EEXIST 17

#define NPK_DEVICE_MATCH_ALL \
    (NPK_DEVICE_MATCH_VENDOR | NPK_DEVICE_MATCH_PRODUCT | \
     NPK_DEVICE_MATCH_CLASS | NPK_DEVICE_MATCH_SUBCLASS | \
     NPK_DEVICE_MATCH_PROTOCOL | NPK_DEVICE_MATCH_INTERFACE)

static npk_device_t devices[NPK_DEVICE_MANAGER_MAX_DEVICES];
static const npk_device_driver_t *drivers[NPK_DEVICE_MANAGER_MAX_DRIVERS];
static size_t device_count;
static size_t driver_count;
static uint32_t next_instance_id = 1;
static bool initialized;

static bool valid_bus(npk_device_bus_t bus) {
    return bus == NPK_DEVICE_BUS_PCI || bus == NPK_DEVICE_BUS_USB;
}

static bool copy_label(char *destination, size_t capacity, const char *source) {
    if (!destination || !source || capacity == 0) return false;
    size_t i = 0;
    while (source[i] != '\0' && i + 1U < capacity) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
    return source[i] == '\0';
}

static bool names_equal(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static void resolve_usb_config(npk_device_t *device) {
    if (!device || device->identity.bus != NPK_DEVICE_BUS_USB) return;
    if (device->identity.interface_number == 0xffU) {
        (void)copy_label(device->category, sizeof(device->category), "usb_device");
        (void)copy_label(device->display_name, sizeof(device->display_name), "USB device");
        device->expected_driver[0] = '\0';
        return;
    }
    npk_device_config_match_t match;
    memset(&match, 0, sizeof(match));
    if (device_config_resolve(&device->identity, &match)) {
        (void)copy_label(device->category, sizeof(device->category), match.type);
        (void)copy_label(device->display_name, sizeof(device->display_name), match.display_name);
        (void)copy_label(device->expected_driver, sizeof(device->expected_driver), match.driver_name);
    } else {
        (void)copy_label(device->category, sizeof(device->category), "unknown");
        (void)copy_label(device->display_name, sizeof(device->display_name), "Unknown USB interface");
        device->expected_driver[0] = '\0';
    }
}

static void log_identity(const npk_device_t *device) {
    const npk_device_identity_t *identity = &device->identity;
    LOG_INFOF("devmgr", "device instance", device->instance_id);
    if (identity->bus == NPK_DEVICE_BUS_PCI) {
        log_message(LOG_INFO, "devmgr", "PCI function discovered");
        LOG_INFOF("devmgr", "PCI BDF", identity->location);
        LOG_INFOF("devmgr", "PCI VID:DID",
                  ((uint64_t)identity->vendor_id << 16) | identity->product_id);
        LOG_INFOF("devmgr", "PCI class/subclass/prog-if",
                  ((uint64_t)identity->class_code << 16) |
                  ((uint64_t)identity->subclass << 8) | identity->protocol);
    } else {
        log_message(LOG_INFO, "devmgr",
                    identity->interface_number == 0xffU ? "USB device discovered" : "USB interface discovered");
        LOG_INFOF("devmgr", "USB port path", identity->location);
        LOG_INFOF("devmgr", "USB VID:PID",
                  ((uint64_t)identity->vendor_id << 16) | identity->product_id);
        LOG_INFOF("devmgr", "USB class/subclass/protocol",
                  ((uint64_t)identity->class_code << 16) |
                  ((uint64_t)identity->subclass << 8) | identity->protocol);
        LOG_INFOF("devmgr", "USB interface number", identity->interface_number);
        if (device->category[0] != '\0') {
            log_message(LOG_INFO, "devmgr", "USB category");
            log_message(LOG_INFO, "devmgr", device->category);
            log_message(LOG_INFO, "devmgr", "resolved USB name");
            log_message(LOG_INFO, "devmgr", device->display_name);
            if (device->expected_driver[0] != '\0') {
                log_message(LOG_INFO, "devmgr", "configured USB driver");
                log_message(LOG_INFO, "devmgr", device->expected_driver);
            }
        }
    }
}

static bool id_matches(const npk_device_id_t *id, const npk_device_identity_t *identity) {
    if (id->bus != identity->bus) return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_VENDOR) && id->vendor_id != identity->vendor_id)
        return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_PRODUCT) && id->product_id != identity->product_id)
        return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_CLASS) && id->class_code != identity->class_code)
        return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_SUBCLASS) && id->subclass != identity->subclass)
        return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_PROTOCOL) && id->protocol != identity->protocol)
        return false;
    if ((id->match_flags & NPK_DEVICE_MATCH_INTERFACE) &&
        id->interface_number != identity->interface_number)
        return false;
    return true;
}

static bool driver_matches(const npk_device_driver_t *driver,
                           const npk_device_identity_t *identity) {
    for (size_t i = 0; i < driver->id_count; ++i)
        if (id_matches(&driver->ids[i], identity)) return true;
    return false;
}

static void bind_device(npk_device_t *device) {
    bool matching_driver_seen = false;
    int last_result = device->last_probe_result;
    for (size_t i = 0; i < driver_count; ++i) {
        const npk_device_driver_t *driver = drivers[i];
        if (!driver_matches(driver, &device->identity)) continue;
        if (device->identity.bus == NPK_DEVICE_BUS_USB &&
            device->expected_driver[0] != '\0' &&
            !names_equal(driver->name, device->expected_driver)) continue;
        matching_driver_seen = true;
        uint32_t driver_bit = 1U << (uint32_t)i;
        if (device->attempted_driver_mask & driver_bit) continue;
        device->attempted_driver_mask |= driver_bit;
        log_message(LOG_INFO, driver->name, "matched device; probing");
        int result = driver->probe(device);
        device->last_probe_result = result;
        if (result == 0) {
            device->driver = driver;
            device->state = NPK_DEVICE_BOUND;
            log_message(LOG_INFO, driver->name, "bound to device");
            LOG_INFOF("devmgr", "bound device instance", device->instance_id);
            return;
        }
        last_result = result;
        log_message(LOG_WARN, driver->name, "probe failed; trying next match");
        LOG_WARNF("devmgr", "driver probe result", (uint64_t)(uint32_t)result);
    }
    device->driver = NULL;
    if (matching_driver_seen) {
        device->state = NPK_DEVICE_PROBE_FAILED;
        device->last_probe_result = last_result;
        log_message(LOG_WARN, "devmgr", "matching drivers failed to bind device");
    } else {
        device->state = NPK_DEVICE_UNBOUND;
        log_message(LOG_INFO, "devmgr", "no registered driver matches device yet");
    }
}

void devmgr_init(void) {
    device_count = 0;
    driver_count = 0;
    next_instance_id = 1;
    initialized = true;
    device_config_reset();
    log_message(LOG_INFO, "devmgr", "bounded device/driver registry initialized");
}

int devmgr_load_default_config(void) {
    if (!initialized) return -NPK_DEVICE_MANAGER_EINVAL;
    int result = device_config_load_vfs("/ddc.json");
    if (result != 0) {
        log_message(LOG_WARN, "devmgr", "VFS /ddc.json load/validation failed");
        LOG_WARNF("devmgr", "device catalog error", (uint64_t)(uint32_t)result);
        return result;
    }
    log_message(LOG_INFO, "devmgr", "device catalog loaded through VFS /ddc.json");
    for (size_t i = 0; i < device_count; ++i) {
        if (devices[i].identity.bus != NPK_DEVICE_BUS_USB || devices[i].state == NPK_DEVICE_BOUND)
            continue;
        resolve_usb_config(&devices[i]);
        log_identity(&devices[i]);
        bind_device(&devices[i]);
    }
    return 0;
}

int devmgr_register_driver(const npk_device_driver_t *driver) {
    if (!initialized || !driver || !driver->name || !driver->ids || !driver->id_count ||
        driver->id_count > NPK_DEVICE_DRIVER_MAX_IDS || !driver->probe)
        return -NPK_DEVICE_MANAGER_EINVAL;
    for (size_t i = 0; i < driver->id_count; ++i) {
        const npk_device_id_t *id = &driver->ids[i];
        if (!valid_bus(id->bus) || !id->match_flags || (id->match_flags & ~NPK_DEVICE_MATCH_ALL))
            return -NPK_DEVICE_MANAGER_EINVAL;
    }
    for (size_t i = 0; i < driver_count; ++i)
        if (drivers[i] == driver) return -NPK_DEVICE_MANAGER_EEXIST;
    if (driver_count >= NPK_DEVICE_MANAGER_MAX_DRIVERS) return -NPK_DEVICE_MANAGER_ENOSPC;

    drivers[driver_count++] = driver;
    log_message(LOG_INFO, driver->name, "driver registered");
    for (size_t i = 0; i < device_count; ++i)
        if (devices[i].state != NPK_DEVICE_BOUND) bind_device(&devices[i]);
    return 0;
}

npk_device_t *devmgr_register_device(const npk_device_identity_t *identity) {
    if (!initialized || !identity || !valid_bus(identity->bus)) {
        log_message(LOG_ERROR, "devmgr", "rejected invalid device identity");
        return NULL;
    }
    if (device_count >= NPK_DEVICE_MANAGER_MAX_DEVICES) {
        log_message(LOG_ERROR, "devmgr", "device registry capacity reached");
        return NULL;
    }
    npk_device_t *device = &devices[device_count++];
    *device = (npk_device_t){
        .instance_id = next_instance_id++,
        .identity = *identity,
        .state = NPK_DEVICE_DISCOVERED,
        .driver = NULL,
        .last_probe_result = 0
    };
    resolve_usb_config(device);
    log_identity(device);
    bind_device(device);
    return device;
}

size_t devmgr_device_count(void) { return device_count; }
size_t devmgr_driver_count(void) { return driver_count; }

const npk_device_t *devmgr_device_at(size_t index) {
    return index < device_count ? &devices[index] : NULL;
}

const npk_device_driver_t *devmgr_driver_at(size_t index) {
    return index < driver_count ? drivers[index] : NULL;
}
