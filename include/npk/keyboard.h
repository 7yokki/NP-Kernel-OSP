#ifndef NPK_KEYBOARD_H
#define NPK_KEYBOARD_H

#include "types.h"

typedef struct {
    uint64_t timestamp;
    uint16_t type;
    uint16_t code;
    int32_t value;
    uint32_t modifiers;
    uint32_t reserved;
} npk_input_event_t;

#define NPK_INPUT_EVENT_SYN 0U
#define NPK_INPUT_EVENT_KEY 1U
#define NPK_INPUT_EVENT_REL 2U
#define NPK_INPUT_EVENT_ABS 3U
#define NPK_INPUT_KEY_PRESS 1
#define NPK_INPUT_KEY_RELEASE 0
#define NPK_INPUT_MOD_SHIFT (1U << 0)
#define NPK_INPUT_MOD_CAPSLOCK (1U << 1)
#define NPK_INPUT_MOD_EXTENDED (1U << 2)
#define NPK_KEY_ESC 1U
#define NPK_KEY_ENTER 28U
#define NPK_KEY_TAB 15U
#define NPK_KEY_BACKSPACE 14U
#define NPK_KEY_SPACE 57U
#define NPK_KEY_LEFT 105U
#define NPK_KEY_RIGHT 106U
#define NPK_KEY_UP 103U
#define NPK_KEY_DOWN 108U
#define NPK_KEY_LEFT_SHIFT 42U
#define NPK_KEY_RIGHT_SHIFT 54U
#define NPK_KEY_CAPSLOCK 58U

void keyboard_init(void);
void keyboard_irq(void);
int keyboard_getchar(void);
bool keyboard_has_data(void);
bool keyboard_get_event(npk_input_event_t *event);
void keyboard_enqueue_input_event(uint16_t type, uint16_t code, int32_t value, uint32_t modifiers);
void keyboard_clear_events(void);
void keyboard_set_turkish_layout(bool enabled);

#endif
