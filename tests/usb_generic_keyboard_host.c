#include <npk/log.h>
#include <npk/usb_generic_keyboard.h>
#include <stdio.h>
#include <string.h>

#define TEST_EVENT_CAPACITY 32U

typedef struct {
    uint8_t usage;
    bool pressed;
    uint8_t modifiers;
} captured_event_t;

static captured_event_t events[TEST_EVENT_CAPACITY];
static size_t event_count;
static unsigned checks;
static unsigned failures;

void keyboard_usb_hid_event(uint8_t usage, bool pressed, uint8_t modifiers) {
    if (event_count < TEST_EVENT_CAPACITY)
        events[event_count++] = (captured_event_t){usage, pressed, modifiers};
}

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

#define CHECK(condition, label) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", (label)); \
        ++failures; \
    } \
} while (0)

int main(void) {
    usb_generic_keyboard_state_t state = {0};
    const uint8_t shift_a[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};
    const uint8_t empty[8] = {0};
    const uint8_t b[8] = {0, 0, 0x05, 0, 0, 0, 0, 0};
    const uint8_t rollover[8] = {0, 0, 0x01, 0, 0, 0, 0, 0};

    CHECK(usb_generic_keyboard_process_report(&state, shift_a, sizeof(shift_a), 1),
          "accept a valid boot report");
    CHECK(event_count == 2, "first report emits modifier and key presses");
    CHECK(events[0].usage == 0xe1 && events[0].pressed,
          "left shift press is emitted");
    CHECK(events[1].usage == 0x04 && events[1].pressed && events[1].modifiers == 0x02,
          "usage A press retains current modifier bits");

    CHECK(usb_generic_keyboard_process_report(&state, shift_a, sizeof(shift_a), 1),
          "accept a repeated unchanged report");
    CHECK(event_count == 2, "unchanged report does not duplicate key presses");

    CHECK(usb_generic_keyboard_process_report(&state, empty, sizeof(empty), 1),
          "accept key release report");
    CHECK(event_count == 4, "release report emits modifier and key releases");
    CHECK(events[2].usage == 0xe1 && !events[2].pressed,
          "left shift release is emitted");
    CHECK(events[3].usage == 0x04 && !events[3].pressed,
          "A release is emitted");

    CHECK(usb_generic_keyboard_process_report(&state, b, sizeof(b), 1),
          "accept B press report");
    CHECK(event_count == 5 && events[4].usage == 0x05 && events[4].pressed,
          "B press is emitted");
    CHECK(usb_generic_keyboard_process_report(&state, rollover, sizeof(rollover), 1),
          "rollover report is safely ignored");
    CHECK(event_count == 5 && state.keys[0] == 0x05,
          "rollover preserves held-key state without synthetic release");
    CHECK(usb_generic_keyboard_process_report(&state, empty, sizeof(empty), 1),
          "normal report recovers after rollover");
    CHECK(event_count == 6 && events[5].usage == 0x05 && !events[5].pressed,
          "held key releases correctly after rollover recovery");

    CHECK(!usb_generic_keyboard_process_report(&state, empty, 7U, 1),
          "reject a short report");
    uint8_t extension_byte[8] = {0, 0xff, 0, 0, 0, 0, 0, 0};
    CHECK(usb_generic_keyboard_process_report(&state, extension_byte, sizeof(extension_byte), 1),
          "accept a nonzero implementation-specific second byte");
    CHECK(event_count == 6, "ignore the boot-report extension byte");

    if (failures) {
        fprintf(stderr, "USB_GENERIC_KEYBOARD_TESTS failed=%u\n", failures);
        return 1;
    }
    printf("USB_GENERIC_KEYBOARD_TESTS passed=%u failed=0\n", checks);
    return 0;
}
