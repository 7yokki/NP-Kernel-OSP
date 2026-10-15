#ifndef NPK_USB_H
#define NPK_USB_H

#include "types.h"

typedef struct {
    uint8_t root_port;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t interface_number;
    uint8_t interface_protocol;
    uint8_t configuration_value;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
    uint16_t report_descriptor_length;
} usb_hid_keyboard_info_t;

void usb_init(void);
void usb_poll(void);
void usb_identify_xhci_device(uint8_t slot_id, uint8_t root_port, uint8_t speed);
bool usb_get_hid_keyboard_info(uint8_t slot_id, uint8_t interface_number,
                               usb_hid_keyboard_info_t *info);

bool xhci_get_descriptor(uint8_t slot_id, uint8_t descriptor_type,
                         uint8_t descriptor_index, uint16_t length,
                         uint8_t *buffer, size_t capacity);
bool xhci_control_request(uint8_t slot_id, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, uint8_t *buffer,
                          uint16_t length, size_t capacity, uint16_t *actual_length);
bool xhci_get_device_descriptor(uint8_t slot_id, uint16_t length,
                                uint8_t *buffer, size_t capacity);
bool xhci_update_ep0_max_packet(uint8_t slot_id, uint16_t max_packet_size);
bool xhci_configure_interrupt_in(uint8_t slot_id, uint8_t speed,
                                 uint8_t endpoint_address, uint16_t max_packet_size,
                                 uint8_t interval);
bool xhci_start_interrupt_in(uint8_t slot_id);
void xhci_poll_keyboard_inputs(void);
int xhci_register_driver(void);

#endif
