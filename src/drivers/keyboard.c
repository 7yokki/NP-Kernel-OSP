#include <npk/arch.h>
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/timer.h>

#define QUEUE_SIZE 1024
#define EVENT_QUEUE_SIZE 128
static uint8_t queue[QUEUE_SIZE];
static volatile uint32_t head;
static volatile uint32_t tail;
static npk_input_event_t event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t event_head;
static volatile uint32_t event_tail;
static bool turkish = true;
static bool shift_down;
static bool caps_lock;
static bool extended;

static const char *us_lower[0x40] = {
    [0x02]="1", [0x03]="2", [0x04]="3", [0x05]="4", [0x06]="5", [0x07]="6", [0x08]="7", [0x09]="8", [0x0a]="9", [0x0b]="0",
    [0x10]="q", [0x11]="w", [0x12]="e", [0x13]="r", [0x14]="t", [0x15]="y", [0x16]="u", [0x17]="i", [0x18]="o", [0x19]="p",
    [0x1e]="a", [0x1f]="s", [0x20]="d", [0x21]="f", [0x22]="g", [0x23]="h", [0x24]="j", [0x25]="k", [0x26]="l",
    [0x2c]="z", [0x2d]="x", [0x2e]="c", [0x2f]="v", [0x30]="b", [0x31]="n", [0x32]="m", [0x39]=" ",
};
static const char *us_upper[0x40] = {
    [0x02]="!", [0x03]="@", [0x04]="#", [0x05]="$", [0x06]="%", [0x07]="^", [0x08]="&", [0x09]="*", [0x0a]="(", [0x0b]=")",
    [0x10]="Q", [0x11]="W", [0x12]="E", [0x13]="R", [0x14]="T", [0x15]="Y", [0x16]="U", [0x17]="I", [0x18]="O", [0x19]="P",
    [0x1e]="A", [0x1f]="S", [0x20]="D", [0x21]="F", [0x22]="G", [0x23]="H", [0x24]="J", [0x25]="K", [0x26]="L",
    [0x2c]="Z", [0x2d]="X", [0x2e]="C", [0x2f]="V", [0x30]="B", [0x31]="N", [0x32]="M", [0x39]=" ",
};
static const char *tr_lower[0x40] = {
    [0x02]="1", [0x03]="2", [0x04]="3", [0x05]="4", [0x06]="5", [0x07]="6", [0x08]="7", [0x09]="8", [0x0a]="9", [0x0b]="0",
    [0x10]="q", [0x11]="w", [0x12]="e", [0x13]="r", [0x14]="t", [0x15]="y", [0x16]="u", [0x17]="ı", [0x18]="o", [0x19]="p", [0x1a]="ğ", [0x1b]="ü",
    [0x1e]="a", [0x1f]="s", [0x20]="d", [0x21]="f", [0x22]="g", [0x23]="h", [0x24]="j", [0x25]="k", [0x26]="l", [0x27]="ş", [0x28]="i",
    [0x2c]="z", [0x2d]="x", [0x2e]="c", [0x2f]="v", [0x30]="b", [0x31]="n", [0x32]="m", [0x33]="ö", [0x34]="ç", [0x39]=" ",
};
static const char *tr_upper[0x40] = {
    [0x02]="1", [0x03]="2", [0x04]="3", [0x05]="4", [0x06]="5", [0x07]="6", [0x08]="7", [0x09]="8", [0x0a]="9", [0x0b]="0",
    [0x10]="Q", [0x11]="W", [0x12]="E", [0x13]="R", [0x14]="T", [0x15]="Y", [0x16]="U", [0x17]="I", [0x18]="O", [0x19]="P", [0x1a]="Ğ", [0x1b]="Ü",
    [0x1e]="A", [0x1f]="S", [0x20]="D", [0x21]="F", [0x22]="G", [0x23]="H", [0x24]="J", [0x25]="K", [0x26]="L", [0x27]="Ş", [0x28]="İ",
    [0x2c]="Z", [0x2d]="X", [0x2e]="C", [0x2f]="V", [0x30]="B", [0x31]="N", [0x32]="M", [0x33]="Ö", [0x34]="Ç", [0x39]=" ",
};

static void enqueue_byte(uint8_t value) {
    uint32_t next = (head + 1) % QUEUE_SIZE;
    if (next == tail) return;
    queue[head] = value; head = next;
}

static void enqueue_text(const char *text) { while (*text) enqueue_byte((uint8_t)*text++); }

void keyboard_enqueue_input_event(uint16_t type, uint16_t code, int32_t value, uint32_t modifiers) {
    uint32_t next = (event_head + 1) % EVENT_QUEUE_SIZE;
    if (next == event_tail) return;
    event_queue[event_head] = (npk_input_event_t){
        .timestamp = timer_ticks(),
        .type = type,
        .code = code,
        .value = value,
        .modifiers = modifiers,
        .reserved = 0,
    };
    event_head = next;
}

static uint16_t extended_key_code(uint8_t key) {
    switch (key) {
        case 0x48: return NPK_KEY_UP;
        case 0x50: return NPK_KEY_DOWN;
        case 0x4b: return NPK_KEY_LEFT;
        case 0x4d: return NPK_KEY_RIGHT;
        default: return 0;
    }
}

void keyboard_init(void) {
    head = tail = 0;
    event_head = event_tail = 0;
    shift_down = caps_lock = extended = false;
    log_message(LOG_INFO, "keyboard", "PS/2 set-1; Turkish Q layout enabled");
}

void keyboard_set_turkish_layout(bool enabled) { turkish = enabled; }

void keyboard_irq(void) {
    uint8_t code = inb(0x60);
    if (code == 0xe0) { extended = true; return; }
    bool released = (code & 0x80) != 0;
    uint8_t key = code & 0x7f;
    uint32_t modifiers = (shift_down ? NPK_INPUT_MOD_SHIFT : 0U) |
                         (caps_lock ? NPK_INPUT_MOD_CAPSLOCK : 0U) |
                         (extended ? NPK_INPUT_MOD_EXTENDED : 0U);
    uint16_t event_code = extended_key_code(key);
    if (event_code != 0) keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, event_code,
                                                         released ? NPK_INPUT_KEY_RELEASE : NPK_INPUT_KEY_PRESS, modifiers);
    else keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, key,
                                      released ? NPK_INPUT_KEY_RELEASE : NPK_INPUT_KEY_PRESS, modifiers);
    if (key == 0x2a || key == 0x36) { shift_down = !released; extended = false; return; }
    if (!released && key == 0x3a) { caps_lock = !caps_lock; extended = false; return; }
    if (released) { extended = false; return; }
    if (extended) { if (key == 0x1c) enqueue_byte('\n'); extended = false; return; }
    if (key == 0x1c) { enqueue_byte('\n'); return; }
    if (key == 0x0e) { enqueue_byte('\b'); return; }
    if (key == 0x0f) { enqueue_byte('\t'); return; }
    const char *text = NULL;
    if (key < 0x40) {
        const char **lower = turkish ? tr_lower : us_lower;
        const char **upper = turkish ? tr_upper : us_upper;
        text = (shift_down || caps_lock) ? upper[key] : lower[key];
    }
    if (text) enqueue_text(text);
}

bool keyboard_has_data(void) { return tail != head; }

bool keyboard_get_event(npk_input_event_t *event) {
    if (!event || event_tail == event_head) return false;
    *event = event_queue[event_tail];
    event_tail = (event_tail + 1) % EVENT_QUEUE_SIZE;
    return true;
}

void keyboard_clear_events(void) { event_tail = event_head; }

int keyboard_getchar(void) {
    if (tail == head) return -1;
    uint8_t value = queue[tail]; tail = (tail + 1) % QUEUE_SIZE;
    return value;
}
