#include <npk/arch.h>
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/mouse.h>

#define PS2_STATUS 0x64U
#define PS2_COMMAND 0x64U
#define PS2_DATA 0x60U
#define PS2_STATUS_OUTPUT_FULL 0x01U
#define PS2_STATUS_INPUT_FULL 0x02U
#define PS2_COMMAND_WRITE_AUX 0xd4U
#define PS2_MOUSE_ENABLE 0xf4U
#define PS2_MOUSE_ACK 0xfau

static bool ready;
static uint8_t packet[3];
static uint8_t packet_index;
static uint8_t button_state;

static bool wait_input_clear(void) {
    for (uint32_t spin = 0; spin < 100000U; ++spin)
        if ((inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL) == 0) return true;
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t spin = 0; spin < 100000U; ++spin)
        if ((inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) != 0) return true;
    return false;
}

static bool mouse_command(uint8_t command) {
    if (!wait_input_clear()) return false;
    outb(PS2_COMMAND, PS2_COMMAND_WRITE_AUX);
    if (!wait_input_clear()) return false;
    outb(PS2_DATA, command);
    if (!wait_output_full()) return false;
    return inb(PS2_DATA) == PS2_MOUSE_ACK;
}

void mouse_init(void) {
    ready = false;
    packet_index = 0;
    button_state = 0;
    /* The controller may not expose an auxiliary port on all firmware paths;
     * failure is non-fatal and leaves keyboard/text input available. */
    if (!mouse_command(PS2_MOUSE_ENABLE)) {
        log_message(LOG_WARN, "mouse", "PS/2 auxiliary device unavailable; pointer events disabled");
        return;
    }
    ready = true;
    log_message(LOG_INFO, "mouse", "PS/2 three-byte pointer events enabled");
}

bool mouse_ready(void) { return ready; }

void mouse_irq(void) {
    if (!ready) {
        (void)inb(PS2_DATA);
        return;
    }
    uint8_t value = inb(PS2_DATA);
    if (packet_index == 0 && (value & 0x08U) == 0) return;
    packet[packet_index++] = value;
    if (packet_index != 3) return;
    packet_index = 0;
    uint8_t buttons = packet[0] & 0x07U;
    int32_t dx = (int8_t)packet[1];
    int32_t dy = -(int8_t)packet[2];
    if ((packet[0] & 0xc0U) == 0) {
        if (dx != 0) keyboard_enqueue_input_event(NPK_INPUT_EVENT_REL, 0, dx, 0);
        if (dy != 0) keyboard_enqueue_input_event(NPK_INPUT_EVENT_REL, 1, dy, 0);
    }
    uint8_t changed = buttons ^ button_state;
    if (changed & 0x01U) keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, NPK_MOUSE_BUTTON_LEFT,
                                                       (buttons & 0x01U) != 0, 0);
    if (changed & 0x02U) keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, NPK_MOUSE_BUTTON_RIGHT,
                                                       (buttons & 0x02U) != 0, 0);
    if (changed & 0x04U) keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, NPK_MOUSE_BUTTON_MIDDLE,
                                                       (buttons & 0x04U) != 0, 0);
    button_state = buttons;
}
