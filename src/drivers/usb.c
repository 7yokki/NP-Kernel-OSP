#include <npk/device_manager.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/usb.h>
#include <npk/usb_generic_keyboard.h>

#define USB_DESCRIPTOR_DEVICE 1U
#define USB_DESCRIPTOR_CONFIGURATION 2U
#define USB_DESCRIPTOR_INTERFACE 4U
#define USB_DESCRIPTOR_ENDPOINT 5U
#define USB_DESCRIPTOR_HID 0x21U
#define USB_DESCRIPTOR_HID_REPORT 0x22U
#define USB_REQUEST_SET_CONFIGURATION 9U
#define USB_DEVICE_DESCRIPTOR_SIZE 18U
#define USB_DEVICE_DESCRIPTOR_MINIMUM 8U
#define USB_CONFIGURATION_HEADER_SIZE 9U
#define USB_INTERFACE_DESCRIPTOR_SIZE 9U
#define USB_ENDPOINT_DESCRIPTOR_SIZE 7U
#define USB_MAX_CONFIGURATION_BYTES NPK_PAGE_SIZE
#define USB_MAX_HID_KEYBOARD_INTERFACES 16U

static uint8_t configuration_buffer[USB_MAX_CONFIGURATION_BYTES];
static usb_hid_keyboard_info_t keyboard_interfaces[USB_MAX_HID_KEYBOARD_INTERFACES];
static size_t keyboard_interface_count;

typedef struct {
    bool active;
    uint8_t number;
    uint8_t alternate;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t endpoint_count;
    uint8_t interrupt_in_count;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
    uint16_t report_descriptor_length;
    bool unsupported_packet_encoding;
} usb_interface_parse_t;

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static bool valid_ep0_packet_size(uint8_t speed, uint8_t descriptor_value,
                                  uint16_t *xhci_packet_size) {
    if (!xhci_packet_size) return false;
    if (speed == 1U) {
        if (descriptor_value != 8U && descriptor_value != 16U &&
            descriptor_value != 32U && descriptor_value != 64U) return false;
        *xhci_packet_size = descriptor_value;
        return true;
    }
    if (speed == 2U) {
        if (descriptor_value != 8U) return false;
        *xhci_packet_size = 8U;
        return true;
    }
    if (speed == 3U) {
        if (descriptor_value != 64U) return false;
        *xhci_packet_size = 64U;
        return true;
    }
    if (speed == 4U) {
        if (descriptor_value != 9U) return false;
        *xhci_packet_size = 512U;
        return true;
    }
    return false;
}

static bool valid_boot_keyboard_candidate(const usb_interface_parse_t *interface) {
    return interface && interface->active && interface->alternate == 0U &&
           interface->class_code == 0x03U && interface->subclass == 0x01U &&
           interface->protocol == 0x01U && interface->endpoint_count == 1U &&
           interface->interrupt_in_count == 1U &&
           !interface->unsupported_packet_encoding &&
           interface->report_descriptor_length != 0U &&
           interface->max_packet_size >= 8U && interface->interval != 0U;
}

static void remember_keyboard_interface(const usb_hid_keyboard_info_t *info) {
    if (!info || keyboard_interface_count >= USB_MAX_HID_KEYBOARD_INTERFACES) {
        log_message(LOG_WARN, "usb", "boot keyboard metadata table is full");
        return;
    }
    keyboard_interfaces[keyboard_interface_count++] = *info;
}

bool usb_get_hid_keyboard_info(uint8_t slot_id, uint8_t interface_number,
                               usb_hid_keyboard_info_t *info) {
    if (!info) return false;
    for (size_t i = 0; i < keyboard_interface_count; ++i) {
        const usb_hid_keyboard_info_t *candidate = &keyboard_interfaces[i];
        if (candidate->slot_id == slot_id &&
            candidate->interface_number == interface_number) {
            *info = *candidate;
            return true;
        }
    }
    return false;
}

static bool parse_hid_report_descriptor_length(usb_interface_parse_t *interface,
                                               const uint8_t *descriptor,
                                               size_t length) {
    if (!interface || !descriptor || length < 6U || descriptor[0] > length ||
        descriptor[1] != USB_DESCRIPTOR_HID) return false;
    size_t declared = descriptor[5];
    if (!declared || declared > (length - 6U) / 3U) return false;
    for (size_t i = 0; i < declared; ++i) {
        size_t item = 6U + i * 3U;
        if (descriptor[item] == USB_DESCRIPTOR_HID_REPORT) {
            uint16_t report_length = read_le16(&descriptor[item + 1U]);
            if (!report_length) return false;
            interface->report_descriptor_length = report_length;
            return true;
        }
    }
    return false;
}

static bool prepare_boot_keyboard(uint8_t slot_id, uint8_t root_port, uint8_t speed,
                                  uint8_t configuration_value,
                                  const usb_interface_parse_t *interface) {
    if (!valid_boot_keyboard_candidate(interface) || !configuration_value) return false;
    if (interface->max_packet_size > 0x07ffU ||
        (interface->endpoint_address & 0x80U) == 0U ||
        (interface->endpoint_address & 0x0fU) == 0U) return false;

    /* xHCI requires Configure Endpoint to succeed before SET_CONFIGURATION. */
    if (!xhci_configure_interrupt_in(slot_id, speed, interface->endpoint_address,
                                     interface->max_packet_size, interface->interval)) {
        log_message(LOG_WARN, "usb", "xHCI rejected the boot keyboard interrupt endpoint");
        return false;
    }
    if (!xhci_control_request(slot_id, 0x00U, USB_REQUEST_SET_CONFIGURATION,
                              configuration_value, 0U, NULL, 0U, 0U, NULL)) {
        log_message(LOG_WARN, "usb", "USB SET_CONFIGURATION request failed");
        return false;
    }

    usb_hid_keyboard_info_t info = {
        .root_port = root_port,
        .slot_id = slot_id,
        .speed = speed,
        .interface_number = interface->number,
        .interface_protocol = interface->protocol,
        .configuration_value = configuration_value,
        .endpoint_address = interface->endpoint_address,
        .interval = interface->interval,
        .max_packet_size = interface->max_packet_size,
        .report_descriptor_length = interface->report_descriptor_length
    };
    remember_keyboard_interface(&info);
    log_message(LOG_INFO, "usb", "boot keyboard endpoint configured and device selected");
    return true;
}

static void register_configuration_interfaces(uint8_t slot_id, uint8_t root_port,
                                             uint8_t speed, uint16_t vendor_id,
                                             uint16_t product_id, uint8_t revision,
                                             uint16_t total_length) {
    size_t offset = 0;
    unsigned registered = 0;
    unsigned alternate_zero_interfaces = 0;
    usb_interface_parse_t current = {0};
    usb_interface_parse_t boot_keyboard = {0};
    bool has_boot_keyboard = false;

    while (offset < total_length) {
        size_t remaining = (size_t)total_length - offset;
        if (remaining < 2U) {
            log_message(LOG_WARN, "usb", "truncated configuration descriptor header");
            return;
        }
        uint8_t descriptor_length = configuration_buffer[offset];
        uint8_t descriptor_type = configuration_buffer[offset + 1U];
        if (descriptor_length < 2U || descriptor_length > remaining) {
            log_message(LOG_WARN, "usb", "invalid configuration descriptor length");
            return;
        }
        const uint8_t *descriptor = &configuration_buffer[offset];
        if (descriptor_type == USB_DESCRIPTOR_INTERFACE) {
            if (descriptor_length < USB_INTERFACE_DESCRIPTOR_SIZE) {
                log_message(LOG_WARN, "usb", "short USB interface descriptor");
                return;
            }
            current = (usb_interface_parse_t){
                .active = descriptor[3] == 0U,
                .number = descriptor[2],
                .alternate = descriptor[3],
                .class_code = descriptor[5],
                .subclass = descriptor[6],
                .protocol = descriptor[7],
                .endpoint_count = descriptor[4]
            };
            if (current.active) ++alternate_zero_interfaces;
            if (valid_boot_keyboard_candidate(&current)) {
                /* Endpoint/HID descriptors follow their interface descriptor. */
                boot_keyboard = current;
                has_boot_keyboard = true;
            }
        } else if (current.active && descriptor_type == USB_DESCRIPTOR_HID) {
            if (!parse_hid_report_descriptor_length(&current, descriptor,
                                                    descriptor_length)) {
                current.report_descriptor_length = 0;
            }
            if (current.class_code == 0x03U && current.subclass == 0x01U &&
                current.protocol == 0x01U) {
                boot_keyboard.report_descriptor_length = current.report_descriptor_length;
                has_boot_keyboard = true;
            }
        } else if (current.active && descriptor_type == USB_DESCRIPTOR_ENDPOINT) {
            if (descriptor_length < USB_ENDPOINT_DESCRIPTOR_SIZE) {
                log_message(LOG_WARN, "usb", "short USB endpoint descriptor");
                return;
            }
            uint8_t endpoint_address = descriptor[2];
            uint8_t attributes = descriptor[3];
            if ((attributes & 0x03U) == 0x03U && (endpoint_address & 0x80U)) {
                ++current.interrupt_in_count;
                current.endpoint_address = endpoint_address;
                uint16_t raw_packet_size = read_le16(&descriptor[4]);
                current.unsupported_packet_encoding = (raw_packet_size & 0xf800U) != 0U;
                current.max_packet_size = (uint16_t)(raw_packet_size & 0x07ffU);
                current.interval = descriptor[6];
                if (current.class_code == 0x03U && current.subclass == 0x01U &&
                    current.protocol == 0x01U) {
                    boot_keyboard = current;
                    has_boot_keyboard = true;
                }
            }
        }
        offset += descriptor_length;
    }

    uint8_t configuration_value = configuration_buffer[5];
    if (has_boot_keyboard && alternate_zero_interfaces == 1U &&
        configuration_buffer[4] == 1U &&
        valid_boot_keyboard_candidate(&boot_keyboard)) {
        (void)prepare_boot_keyboard(slot_id, root_port, speed, configuration_value,
                                   &boot_keyboard);
    } else if (has_boot_keyboard) {
        log_message(LOG_WARN, "usb", "keyboard config is outside the single-interface boot subset");
    }

    offset = 0;
    while (offset < total_length) {
        size_t remaining = (size_t)total_length - offset;
        uint8_t descriptor_length = configuration_buffer[offset];
        uint8_t descriptor_type = configuration_buffer[offset + 1U];
        if (descriptor_length < 2U || descriptor_length > remaining) return;
        if (descriptor_type == USB_DESCRIPTOR_INTERFACE &&
            descriptor_length >= USB_INTERFACE_DESCRIPTOR_SIZE) {
            uint8_t interface_number = configuration_buffer[offset + 2U];
            uint8_t alternate_setting = configuration_buffer[offset + 3U];
            if (alternate_setting == 0U) {
                uint8_t class_code = configuration_buffer[offset + 5U];
                uint8_t subclass = configuration_buffer[offset + 6U];
                uint8_t protocol = configuration_buffer[offset + 7U];
                npk_device_identity_t identity = {
                    .bus = NPK_DEVICE_BUS_USB,
                    .location = ((uint64_t)root_port << 32) | slot_id,
                    .vendor_id = vendor_id,
                    .product_id = product_id,
                    .class_code = class_code,
                    .subclass = subclass,
                    .protocol = protocol,
                    .revision = revision,
                    .interface_number = interface_number
                };
                if (!devmgr_register_device(&identity)) {
                    log_message(LOG_ERROR, "usb", "interface identity could not be added to manager");
                } else {
                    LOG_INFOF("usb", "USB interface class/subclass/protocol",
                              ((uint64_t)class_code << 16) |
                              ((uint64_t)subclass << 8) | protocol);
                    LOG_INFOF("usb", "USB interface number", interface_number);
                    ++registered;
                }
            }
        }
        offset += descriptor_length;
    }
    LOG_INFOF("usb", "configuration interfaces registered", registered);
}

void usb_init(void) {
    keyboard_interface_count = 0;
    log_message(LOG_INFO, "usb", "USB HID and xHCI drivers initializing");
    int result = usb_generic_keyboard_register_driver();
    if (result != 0) {
        LOG_ERRORF("usb", "generic HID keyboard driver registration failed",
                   (uint64_t)(uint32_t)result);
        return;
    }
    result = xhci_register_driver();
    if (result != 0) {
        LOG_ERRORF("usb", "xHCI driver registration failed", (uint64_t)(uint32_t)result);
        return;
    }
    log_message(LOG_INFO, "usb", "USB HID keyboard and xHCI drivers registered");
}

void usb_poll(void) {
    xhci_poll_keyboard_inputs();
}

void usb_identify_xhci_device(uint8_t slot_id, uint8_t root_port, uint8_t speed) {
    uint8_t descriptor[USB_DEVICE_DESCRIPTOR_SIZE] = {0};
    if (!xhci_get_descriptor(slot_id, USB_DESCRIPTOR_DEVICE, 0U,
                             USB_DEVICE_DESCRIPTOR_MINIMUM, descriptor, sizeof(descriptor))) {
        LOG_WARNF("usb", "initial 8-byte device descriptor request failed", slot_id);
        return;
    }
    if (descriptor[0] != USB_DEVICE_DESCRIPTOR_SIZE ||
        descriptor[1] != USB_DESCRIPTOR_DEVICE) {
        LOG_WARNF("usb", "invalid initial USB device descriptor header", slot_id);
        return;
    }

    uint16_t packet_size;
    if (!valid_ep0_packet_size(speed, descriptor[7], &packet_size)) {
        LOG_WARNF("usb", "invalid EP0 packet size for reported speed", descriptor[7]);
        return;
    }
    if (!xhci_update_ep0_max_packet(slot_id, packet_size)) {
        LOG_WARNF("usb", "EP0 context packet-size update failed", packet_size);
        return;
    }

    if (!xhci_get_descriptor(slot_id, USB_DESCRIPTOR_DEVICE, 0U,
                             USB_DEVICE_DESCRIPTOR_SIZE, descriptor, sizeof(descriptor))) {
        LOG_WARNF("usb", "full device descriptor request failed", slot_id);
        return;
    }
    if (descriptor[0] != USB_DEVICE_DESCRIPTOR_SIZE ||
        descriptor[1] != USB_DESCRIPTOR_DEVICE) {
        LOG_WARNF("usb", "invalid full USB device descriptor", slot_id);
        return;
    }

    uint16_t usb_bcd = read_le16(&descriptor[2]);
    uint16_t vendor_id = read_le16(&descriptor[8]);
    uint16_t product_id = read_le16(&descriptor[10]);
    uint16_t device_bcd = read_le16(&descriptor[12]);

    log_message(LOG_INFO, "usb", "USB standard device descriptor received");
    LOG_INFOF("usb", "root port", root_port);
    LOG_INFOF("usb", "xHCI slot", slot_id);
    LOG_INFOF("usb", "port speed code", speed);
    LOG_INFOF("usb", "USB VID:PID", ((uint64_t)vendor_id << 16) | product_id);
    LOG_INFOF("usb", "USB BCD version", usb_bcd);
    LOG_INFOF("usb", "device BCD version", device_bcd);
    LOG_INFOF("usb", "device class/subclass/protocol",
              ((uint64_t)descriptor[4] << 16) |
              ((uint64_t)descriptor[5] << 8) | descriptor[6]);
    LOG_INFOF("usb", "EP0 maximum packet size", packet_size);
    LOG_INFOF("usb", "manufacturer/product/serial string indexes",
              ((uint64_t)descriptor[14] << 16) |
              ((uint64_t)descriptor[15] << 8) | descriptor[16]);
    LOG_INFOF("usb", "configuration count", descriptor[17]);

    npk_device_identity_t device_identity = {
        .bus = NPK_DEVICE_BUS_USB,
        .location = ((uint64_t)root_port << 32) | slot_id,
        .vendor_id = vendor_id,
        .product_id = product_id,
        .class_code = descriptor[4],
        .subclass = descriptor[5],
        .protocol = descriptor[6],
        .revision = (uint8_t)usb_bcd,
        .interface_number = 0xff
    };
    if (!devmgr_register_device(&device_identity))
        log_message(LOG_ERROR, "usb", "device identity could not be added to manager");

    if (descriptor[17] == 0U) {
        log_message(LOG_INFO, "usb", "device has no configuration descriptors");
        return;
    }

    uint8_t configuration_header[USB_CONFIGURATION_HEADER_SIZE] = {0};
    if (!xhci_get_descriptor(slot_id, USB_DESCRIPTOR_CONFIGURATION, 0U,
                             USB_CONFIGURATION_HEADER_SIZE, configuration_header,
                             sizeof(configuration_header))) {
        LOG_WARNF("usb", "configuration descriptor header request failed", slot_id);
        return;
    }
    if (configuration_header[0] < USB_CONFIGURATION_HEADER_SIZE ||
        configuration_header[1] != USB_DESCRIPTOR_CONFIGURATION) {
        LOG_WARNF("usb", "invalid USB configuration descriptor header", slot_id);
        return;
    }
    uint16_t total_length = read_le16(&configuration_header[2]);
    if (total_length < USB_CONFIGURATION_HEADER_SIZE ||
        total_length > USB_MAX_CONFIGURATION_BYTES) {
        LOG_WARNF("usb", "USB configuration total length outside supported bound", total_length);
        return;
    }
    if (!xhci_get_descriptor(slot_id, USB_DESCRIPTOR_CONFIGURATION, 0U,
                             total_length, configuration_buffer,
                             sizeof(configuration_buffer))) {
        LOG_WARNF("usb", "full configuration descriptor request failed", slot_id);
        return;
    }
    if (configuration_buffer[0] < USB_CONFIGURATION_HEADER_SIZE ||
        configuration_buffer[0] > total_length ||
        configuration_buffer[1] != USB_DESCRIPTOR_CONFIGURATION ||
        read_le16(&configuration_buffer[2]) != total_length) {
        LOG_WARNF("usb", "invalid full USB configuration descriptor", slot_id);
        return;
    }

    log_message(LOG_INFO, "usb", "USB configuration descriptor received");
    LOG_INFOF("usb", "configuration descriptor bytes", total_length);
    LOG_INFOF("usb", "declared interface count", configuration_buffer[4]);
    register_configuration_interfaces(slot_id, root_port, speed, vendor_id, product_id,
                                      (uint8_t)usb_bcd, total_length);
}
