#include <npk/device_manager.h>
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/string.h>
#include <npk/usb.h>
#include <npk/usb_generic_keyboard.h>

#define HID_CLASS 0x03U
#define HID_SUBCLASS_BOOT 0x01U
#define HID_PROTOCOL_KEYBOARD 0x01U
#define HID_DESCRIPTOR_REPORT 0x22U
#define HID_GET_DESCRIPTOR 0x06U
#define HID_SET_PROTOCOL 0x0bU
#define USB_GENERIC_KEYBOARD_MAX_REPORT_DESCRIPTOR 512U
#define USB_GENERIC_KEYBOARD_MAX_SLOTS 16U

static usb_generic_keyboard_state_t keyboard_states[USB_GENERIC_KEYBOARD_MAX_SLOTS + 1U];
static uint8_t report_descriptor[USB_GENERIC_KEYBOARD_MAX_REPORT_DESCRIPTOR];

static const npk_device_id_t keyboard_ids[] = {{
    .bus = NPK_DEVICE_BUS_USB,
    .match_flags = NPK_DEVICE_MATCH_CLASS | NPK_DEVICE_MATCH_SUBCLASS |
                   NPK_DEVICE_MATCH_PROTOCOL,
    .class_code = HID_CLASS,
    .subclass = HID_SUBCLASS_BOOT,
    .protocol = HID_PROTOCOL_KEYBOARD
}};

static bool report_descriptor_has_keyboard_application(const uint8_t *bytes,
                                                        size_t length) {
    if (!bytes || !length || length > USB_GENERIC_KEYBOARD_MAX_REPORT_DESCRIPTOR)
        return false;
    uint32_t usage_page = 0;
    uint32_t local_usage = UINT32_MAX;
    unsigned collection_depth = 0;
    bool keyboard_application = false;

    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = bytes[offset++];
        if (prefix == 0xfeU) {
            if (length - offset < 2U) return false;
            size_t long_length = bytes[offset++];
            ++offset; /* Long item tag. */
            if (long_length > length - offset) return false;
            offset += long_length;
            continue;
        }
        unsigned size_code = prefix & 3U;
        size_t item_size = size_code == 3U ? 4U : size_code;
        if (item_size > length - offset) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < item_size; ++i)
            value |= (uint32_t)bytes[offset + i] << (8U * i);
        offset += item_size;
        unsigned item_type = (prefix >> 2) & 3U;
        unsigned tag = prefix >> 4;
        if (item_type == 1U) { /* Global item. */
            if (tag == 0U) usage_page = value;
        } else if (item_type == 2U) { /* Local item. */
            if (tag == 0U) local_usage = value;
        } else if (item_type == 0U) { /* Main item. */
            if (tag == 10U) { /* Collection. */
                if (collection_depth >= 16U) return false;
                if (collection_depth == 0U && value == 1U &&
                    usage_page == 1U && local_usage == 6U)
                    keyboard_application = true;
                ++collection_depth;
                local_usage = UINT32_MAX;
            } else if (tag == 12U) { /* End Collection. */
                if (!collection_depth) return false;
                --collection_depth;
                local_usage = UINT32_MAX;
            } else {
                local_usage = UINT32_MAX;
            }
        }
    }
    return keyboard_application && collection_depth == 0U;
}

static bool report_contains_usage(const uint8_t *report, uint8_t usage) {
    for (size_t i = 2; i < 8U; ++i)
        if (report[i] == usage) return true;
    return false;
}

static bool key_array_contains(const uint8_t keys[USB_GENERIC_KEYBOARD_KEYS],
                               uint8_t usage) {
    for (size_t i = 0; i < USB_GENERIC_KEYBOARD_KEYS; ++i)
        if (keys[i] == usage) return true;
    return false;
}

bool usb_generic_keyboard_process_report(usb_generic_keyboard_state_t *state,
                                         const uint8_t *report, size_t length,
                                         uint8_t slot_id) {
    if (!state || !report || length != 8U) return false;
    /* HID 1.11 Appendix F.4: the second boot-report byte is implementation-specific. */

    bool had_previous_report = state->initialized;
    uint8_t old_modifiers = had_previous_report ? state->modifiers : 0U;
    uint8_t new_modifiers = report[0];
    for (uint8_t bit = 0; bit < 8U; ++bit) {
        uint8_t mask = (uint8_t)(1U << bit);
        if ((old_modifiers & mask) == (new_modifiers & mask)) continue;
        uint8_t usage = (uint8_t)(0xe0U + bit);
        bool pressed = (new_modifiers & mask) != 0;
        keyboard_usb_hid_event(usage, pressed, new_modifiers);
        LOG_INFOF("usb_generic_keyboard", "HID keyboard usage/state",
                  ((uint64_t)usage << 32) | (pressed ? 1U : 0U));
    }
    state->modifiers = new_modifiers;
    state->initialized = true;

    bool rollover = false;
    for (size_t i = 2; i < 8U; ++i)
        if (report[i] == 1U) rollover = true;
    if (rollover) {
        log_message(LOG_WARN, "usb_generic_keyboard",
                    "boot report rollover; preserving previous key array");
        return true;
    }

    if (!had_previous_report) memset(state->keys, 0, sizeof(state->keys));
    for (size_t i = 0; i < USB_GENERIC_KEYBOARD_KEYS; ++i) {
        uint8_t usage = state->keys[i];
        if (usage && !report_contains_usage(report, usage)) {
            keyboard_usb_hid_event(usage, false, new_modifiers);
            LOG_INFOF("usb_generic_keyboard", "HID keyboard usage/state",
                      ((uint64_t)usage << 32));
        }
    }
    for (size_t i = 2; i < 8U; ++i) {
        uint8_t usage = report[i];
        if (!usage || key_array_contains(state->keys, usage)) continue;
        keyboard_usb_hid_event(usage, true, new_modifiers);
        LOG_INFOF("usb_generic_keyboard", "HID keyboard usage/state",
                  ((uint64_t)usage << 32) | 1U);
    }
    memcpy(state->keys, &report[2], USB_GENERIC_KEYBOARD_KEYS);
    (void)slot_id;
    return true;
}

static int usb_generic_keyboard_probe(npk_device_t *device) {
    if (!device || device->identity.bus != NPK_DEVICE_BUS_USB ||
        device->identity.interface_number == 0xffU)
        return -22;
    uint8_t slot_id = (uint8_t)device->identity.location;
    if (!slot_id || slot_id > USB_GENERIC_KEYBOARD_MAX_SLOTS) return -19;

    usb_hid_keyboard_info_t info;
    if (!usb_get_hid_keyboard_info(slot_id, device->identity.interface_number, &info)) {
        log_message(LOG_WARN, "usb_generic_keyboard", "no validated boot-keyboard interface metadata");
        return -19;
    }
    if (!info.report_descriptor_length ||
        info.report_descriptor_length > sizeof(report_descriptor)) {
        log_message(LOG_WARN, "usb_generic_keyboard", "HID report descriptor exceeds supported bound");
        return -22;
    }

    uint16_t actual = 0;
    if (!xhci_control_request(slot_id, 0x81U, HID_GET_DESCRIPTOR,
                              (uint16_t)((HID_DESCRIPTOR_REPORT << 8) | 0U),
                              info.interface_number, report_descriptor,
                              info.report_descriptor_length, sizeof(report_descriptor), &actual) ||
        actual != info.report_descriptor_length ||
        !report_descriptor_has_keyboard_application(report_descriptor, actual)) {
        log_message(LOG_WARN, "usb_generic_keyboard", "HID report descriptor validation failed");
        return -22;
    }

    /* HID 1.11: SET_PROTOCOL value 0 selects the standardized boot report. */
    if (!xhci_control_request(slot_id, 0x21U, HID_SET_PROTOCOL, 0U,
                              info.interface_number, NULL, 0U, 0U, NULL)) {
        log_message(LOG_WARN, "usb_generic_keyboard", "HID boot protocol selection failed");
        return -5;
    }

    memset(&keyboard_states[slot_id], 0, sizeof(keyboard_states[slot_id]));
    if (!xhci_start_interrupt_in(slot_id)) {
        log_message(LOG_WARN, "usb_generic_keyboard", "xHCI interrupt-IN transfer could not be started");
        return -5;
    }
    log_message(LOG_INFO, "usb_generic_keyboard", "boot-protocol input enabled; polling HID reports");
    return 0;
}

static const npk_device_driver_t keyboard_driver = {
    .name = "usb_hid_keyboard",
    .ids = keyboard_ids,
    .id_count = sizeof(keyboard_ids) / sizeof(keyboard_ids[0]),
    .probe = usb_generic_keyboard_probe
};

int usb_generic_keyboard_register_driver(void) {
    return devmgr_register_driver(&keyboard_driver);
}

void usb_generic_keyboard_receive_report(uint8_t slot_id,
                                         const uint8_t *report, size_t length) {
    if (!slot_id || slot_id > USB_GENERIC_KEYBOARD_MAX_SLOTS) return;
    usb_generic_keyboard_state_t *state = &keyboard_states[slot_id];
    if (!usb_generic_keyboard_process_report(state, report, length, slot_id))
        LOG_WARNF("usb_generic_keyboard", "invalid HID boot report for slot", slot_id);
}
