#ifndef NPK_USB_GENERIC_KEYBOARD_H
#define NPK_USB_GENERIC_KEYBOARD_H

#include <npk/types.h>

#define USB_GENERIC_KEYBOARD_KEYS 6U

typedef struct {
    uint8_t modifiers;
    uint8_t keys[USB_GENERIC_KEYBOARD_KEYS];
    bool initialized;
} usb_generic_keyboard_state_t;

int usb_generic_keyboard_register_driver(void);
bool usb_generic_keyboard_process_report(usb_generic_keyboard_state_t *state,
                                         const uint8_t *report, size_t length,
                                         uint8_t slot_id);
void usb_generic_keyboard_receive_report(uint8_t slot_id,
                                         const uint8_t *report, size_t length);

#endif
