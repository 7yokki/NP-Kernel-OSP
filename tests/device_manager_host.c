#include <npk/device_manager.h>
#include <npk/log.h>
#include <npk/vfs.h>

#include <stdio.h>
#include <string.h>

static unsigned failing_probe_calls;
static unsigned xhci_probe_calls;
static unsigned hid_probe_calls;
static bool vfs_catalog_path_seen;
static const char test_catalog[] =
    "{\"version\":1,\"categories\":["
    "{\"bus\":\"usb\",\"class\":3,\"subclass\":1,\"protocol\":1,"
    "\"type\":\"keyboard\",\"generic_name\":\"Generic Keyboard\","
    "\"driver\":\"test-usb-hid-keyboard\"},"
    "{\"bus\":\"usb\",\"class\":3,\"subclass\":1,\"protocol\":2,"
    "\"type\":\"mouse\",\"generic_name\":\"Generic USB Mouse\","
    "\"driver\":\"test-usb-hid-mouse\"}],"
    "\"products\":[{\"vid\":1575,\"pid\":1,\"type\":\"keyboard\","
    "\"name\":\"QEMU USB Keyboard\"}]}";

void log_message(log_level_t level, const char *component, const char *message) {
    (void)level;
    (void)component;
    (void)message;
}

void logf(log_level_t level, const char *component, const char *message, uint64_t value) {
    (void)level;
    (void)component;
    (void)message;
    (void)value;
}

int vfs_open(const char *path) {
    if (path && strcmp(path, "/ddc.json") == 0) {
        vfs_catalog_path_seen = true;
        return 7;
    }
    return -2;
}

ssize_t vfs_size(int fd) {
    return fd == 7 ? (ssize_t)(sizeof(test_catalog) - 1U) : -9;
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    if (fd != 7 || !buffer || count != sizeof(test_catalog) - 1U) return -9;
    memcpy(buffer, test_catalog, count);
    return (ssize_t)count;
}

int vfs_close(int fd) { return fd == 7 ? 0 : -9; }

static int fail_probe(npk_device_t *device) {
    (void)device;
    ++failing_probe_calls;
    return -5;
}

static int xhci_probe(npk_device_t *device) {
    if (!device || device->identity.bus != NPK_DEVICE_BUS_PCI) return -22;
    ++xhci_probe_calls;
    return 0;
}

static int hid_keyboard_probe(npk_device_t *device) {
    if (!device || device->identity.bus != NPK_DEVICE_BUS_USB ||
        device->identity.interface_number == 0xffU)
        return -22;
    ++hid_probe_calls;
    return 0;
}

static const npk_device_id_t xhci_ids[] = {{
    .bus = NPK_DEVICE_BUS_PCI,
    .match_flags = NPK_DEVICE_MATCH_CLASS | NPK_DEVICE_MATCH_SUBCLASS | NPK_DEVICE_MATCH_PROTOCOL,
    .class_code = 0x0c,
    .subclass = 0x03,
    .protocol = 0x30
}};

static const npk_device_id_t hid_keyboard_ids[] = {{
    .bus = NPK_DEVICE_BUS_USB,
    .match_flags = NPK_DEVICE_MATCH_CLASS | NPK_DEVICE_MATCH_SUBCLASS | NPK_DEVICE_MATCH_PROTOCOL,
    .class_code = 0x03,
    .subclass = 0x01,
    .protocol = 0x01
}};

static const npk_device_driver_t failing_driver = {
    .name = "test-xhci-failing",
    .ids = xhci_ids,
    .id_count = 1,
    .probe = fail_probe
};

static const npk_device_driver_t xhci_driver = {
    .name = "test-xhci",
    .ids = xhci_ids,
    .id_count = 1,
    .probe = xhci_probe
};

static const npk_device_driver_t hid_keyboard_driver = {
    .name = "test-usb-hid-keyboard",
    .ids = hid_keyboard_ids,
    .id_count = 1,
    .probe = hid_keyboard_probe
};

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", label); \
        return 1; \
    } \
} while (0)

int main(void) {
    devmgr_init();
    CHECK(devmgr_load_default_config() == 0 && vfs_catalog_path_seen,
          "device manager loads ddc.json through VFS path /ddc.json");

    npk_device_identity_t xhci = {
        .bus = NPK_DEVICE_BUS_PCI,
        .location = 0x00000800,
        .vendor_id = 0x1b36,
        .product_id = 0x000d,
        .class_code = 0x0c,
        .subclass = 0x03,
        .protocol = 0x30,
        .revision = 0x01,
        .interface_number = 0xff
    };
    npk_device_t *xhci_device = devmgr_register_device(&xhci);
    CHECK(xhci_device && xhci_device->state == NPK_DEVICE_UNBOUND,
          "device-before-driver remains inventoried/unbound");
    CHECK(devmgr_register_driver(&failing_driver) == 0,
          "register matching driver whose probe fails");
    CHECK(devmgr_device_at(0)->state == NPK_DEVICE_PROBE_FAILED && failing_probe_calls == 1,
          "failed probe is recorded without losing device identity");
    CHECK(devmgr_register_driver(&xhci_driver) == 0,
          "register fallback xHCI driver after device discovery");
    CHECK(devmgr_device_at(0)->state == NPK_DEVICE_BOUND &&
          devmgr_device_at(0)->driver == &xhci_driver &&
          failing_probe_calls == 1 && xhci_probe_calls == 1,
          "late registration probes only the new matching driver after a prior failure");

    npk_device_identity_t keyboard_interface = {
        .bus = NPK_DEVICE_BUS_USB,
        .location = 0x00010002,
        .vendor_id = 0x0627,
        .product_id = 0x0001,
        .class_code = 0x03,
        .subclass = 0x01,
        .protocol = 0x01,
        .interface_number = 2
    };
    CHECK(devmgr_register_driver(&hid_keyboard_driver) == 0,
          "register configured USB HID boot-keyboard driver");
    npk_device_t *keyboard = devmgr_register_device(&keyboard_interface);
    CHECK(keyboard && keyboard->state == NPK_DEVICE_BOUND &&
          keyboard->driver == &hid_keyboard_driver && hid_probe_calls == 1,
          "USB interface category delegates to its configured keyboard driver");
    CHECK(strcmp(keyboard->category, "keyboard") == 0 &&
          strcmp(keyboard->display_name, "QEMU USB Keyboard") == 0,
          "exact VID/PID resolves a model-specific product name");

    npk_device_identity_t generic_identity = keyboard_interface;
    generic_identity.location = 0x00010003;
    generic_identity.vendor_id = 0x1234;
    generic_identity.product_id = 0x5678;
    npk_device_t *generic_keyboard = devmgr_register_device(&generic_identity);
    CHECK(generic_keyboard && generic_keyboard->state == NPK_DEVICE_BOUND &&
          generic_keyboard->driver == &hid_keyboard_driver,
          "unknown keyboard VID/PID still delegates by category");
    CHECK(strcmp(generic_keyboard->display_name, "Generic Keyboard") == 0,
          "unknown product uses category generic name");

    npk_device_identity_t other_usb_interface = keyboard_interface;
    other_usb_interface.location = 0x00010004;
    other_usb_interface.protocol = 0x02;
    other_usb_interface.interface_number = 3;
    npk_device_t *other = devmgr_register_device(&other_usb_interface);
    CHECK(other && other->state == NPK_DEVICE_UNBOUND &&
          strcmp(other->category, "mouse") == 0 &&
          strcmp(other->display_name, "Generic USB Mouse") == 0,
          "different interface protocol resolves a separate category without misbinding");
    CHECK(devmgr_device_count() == 4 && devmgr_driver_count() == 3,
          "bounded registry reports consistent counts");
    CHECK(devmgr_device_at(4) == NULL && devmgr_driver_at(3) == NULL,
          "out-of-range registry access returns NULL");

    puts("DEVICE_MANAGER_TESTS passed=12 failed=0");
    return 0;
}
