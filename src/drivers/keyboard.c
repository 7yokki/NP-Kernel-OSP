#include <npk/arch.h>
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/process.h>
#include <npk/timer.h>

#define QUEUE_SIZE 1024
#define EVENT_QUEUE_SIZE 128
static uint8_t queue[QUEUE_SIZE];
static volatile uint32_t head;
static volatile uint32_t tail;
static thread_t *input_waiter;
static npk_input_event_t event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t event_head;
static volatile uint32_t event_tail;
static bool turkish = true;
static bool shift_down;
static bool altgr_down;
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
    [0x0c]="*", [0x0d]="-", [0x2b]=",", [0x29]="\"", [0x35]=".",
    [0x10]="q", [0x11]="w", [0x12]="e", [0x13]="r", [0x14]="t", [0x15]="y", [0x16]="u", [0x17]="ı", [0x18]="o", [0x19]="p", [0x1a]="ğ", [0x1b]="ü",
    [0x1e]="a", [0x1f]="s", [0x20]="d", [0x21]="f", [0x22]="g", [0x23]="h", [0x24]="j", [0x25]="k", [0x26]="l", [0x27]="ş", [0x28]="i",
    [0x2c]="z", [0x2d]="x", [0x2e]="c", [0x2f]="v", [0x30]="b", [0x31]="n", [0x32]="m", [0x33]="ö", [0x34]="ç", [0x39]=" ",
};
static const char *tr_upper[0x40] = {
    [0x02]="!", [0x03]="'", [0x04]="^", [0x05]="+", [0x06]="%", [0x07]="&", [0x08]="/", [0x09]="(", [0x0a]=")", [0x0b]="=",
    [0x0c]="?", [0x0d]="_", [0x2b]=";", [0x29]="é", [0x35]=":",
    [0x10]="Q", [0x11]="W", [0x12]="E", [0x13]="R", [0x14]="T", [0x15]="Y", [0x16]="U", [0x17]="I", [0x18]="O", [0x19]="P", [0x1a]="Ğ", [0x1b]="Ü",
    [0x1e]="A", [0x1f]="S", [0x20]="D", [0x21]="F", [0x22]="G", [0x23]="H", [0x24]="J", [0x25]="K", [0x26]="L", [0x27]="Ş", [0x28]="İ",
    [0x2c]="Z", [0x2d]="X", [0x2e]="C", [0x2f]="V", [0x30]="B", [0x31]="N", [0x32]="M", [0x33]="Ö", [0x34]="Ç", [0x39]=" ",
};

static uint64_t keyboard_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void keyboard_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}

static void enqueue_byte(uint8_t value) {
    uint64_t flags = keyboard_irq_save();
    uint32_t next = (head + 1) % QUEUE_SIZE;
    if (next != tail) {
        queue[head] = value;
        head = next;
        if (input_waiter) {
            thread_t *waiter = input_waiter;
            input_waiter = NULL;
            scheduler_wake_thread(waiter);
        }
    }
    keyboard_irq_restore(flags);
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

static uint16_t usb_usage_to_keycode(uint8_t usage, bool *is_extended) {
    static const uint8_t letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
        0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c
    };
    static const uint8_t digits[10] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b
    };
    if (is_extended) *is_extended = false;
    if (usage >= 0x04U && usage <= 0x1dU) return letters[usage - 0x04U];
    if (usage >= 0x1eU && usage <= 0x27U) return digits[usage - 0x1eU];
    switch (usage) {
        case 0x28: return 0x1c;
        case 0x29: return 0x01;
        case 0x2a: return 0x0e;
        case 0x2b: return 0x0f;
        case 0x2c: return 0x39;
        case 0x2d: return 0x0c;
        case 0x2e: return 0x0d;
        case 0x2f: return 0x1a;
        case 0x30: return 0x1b;
        case 0x31: return 0x2b;
        case 0x32: return 0x2b;
        case 0x33: return 0x27;
        case 0x34: return 0x28;
        case 0x35: return 0x29;
        case 0x36: return 0x33;
        case 0x37: return 0x34;
        case 0x38: return 0x35;
        case 0x39: return 0x3a;
        case 0x3a: return 0x3b;
        case 0x3b: return 0x3c;
        case 0x3c: return 0x3d;
        case 0x3d: return 0x3e;
        case 0x3e: return 0x3f;
        case 0x3f: return 0x40;
        case 0x40: return 0x41;
        case 0x41: return 0x42;
        case 0x42: return 0x43;
        case 0x43: return 0x44;
        case 0x44: return 0x57;
        case 0x45: return 0x58;
        case 0x46: if (is_extended) *is_extended = true; return 0x37;
        case 0x47: return 0x46;
        case 0x48: return 0x45;
        case 0x58: if (is_extended) *is_extended = true; return 0x1c;
        case 0x49: if (is_extended) *is_extended = true; return 0x52;
        case 0x4a: if (is_extended) *is_extended = true; return 0x47;
        case 0x4b: if (is_extended) *is_extended = true; return 0x49;
        case 0x4c: if (is_extended) *is_extended = true; return 0x53;
        case 0x4d: if (is_extended) *is_extended = true; return 0x4f;
        case 0x4e: if (is_extended) *is_extended = true; return 0x51;
        case 0x4f: if (is_extended) *is_extended = true; return 0x4d;
        case 0x50: if (is_extended) *is_extended = true; return 0x4b;
        case 0x51: if (is_extended) *is_extended = true; return 0x50;
        case 0x52: if (is_extended) *is_extended = true; return 0x48;
        case 0x53: return 0x45;
        case 0x54: if (is_extended) *is_extended = true; return 0x35;
        case 0x55: return 0x37;
        case 0x56: return 0x4a;
        case 0x57: return 0x4e;
        case 0x59: return 0x4f;
        case 0x5a: return 0x50;
        case 0x5b: return 0x51;
        case 0x5c: return 0x4b;
        case 0x5d: return 0x4c;
        case 0x5e: return 0x4d;
        case 0x5f: return 0x47;
        case 0x60: return 0x48;
        case 0x61: return 0x49;
        case 0x62: return 0x52;
        case 0x63: return 0x53;
        case 0x64: return 0x56;
        case 0x65: if (is_extended) *is_extended = true; return 0x5d;
        case 0x66: if (is_extended) *is_extended = true; return 0x5e;
        case 0x67: return 0x0d;
        case 0xe0: return 0x1d;
        case 0xe1: return 0x2a;
        case 0xe2: return 0x38;
        case 0xe3: if (is_extended) *is_extended = true; return 0x5b;
        case 0xe4: if (is_extended) *is_extended = true; return 0x1d;
        case 0xe5: return 0x36;
        case 0xe6: if (is_extended) *is_extended = true; return 0x38;
        case 0xe7: if (is_extended) *is_extended = true; return 0x5c;
        default: return 0;
    }
}

void keyboard_usb_hid_event(uint8_t usage, bool pressed, uint8_t hid_modifiers) {
    bool is_extended = false;
    uint16_t key = usb_usage_to_keycode(usage, &is_extended);
    if (!key) return;
    uint16_t event_key = key;
    switch (usage) {
        case 0x49: event_key = NPK_KEY_INSERT; break;
        case 0x4a: event_key = NPK_KEY_HOME; break;
        case 0x4b: event_key = NPK_KEY_PAGEUP; break;
        case 0x4c: event_key = NPK_KEY_DELETE; break;
        case 0x4d: event_key = NPK_KEY_END; break;
        case 0x4e: event_key = NPK_KEY_PAGEDOWN; break;
        case 0x4f: event_key = NPK_KEY_RIGHT; break;
        case 0x50: event_key = NPK_KEY_LEFT; break;
        case 0x51: event_key = NPK_KEY_DOWN; break;
        case 0x52: event_key = NPK_KEY_UP; break;
        case 0x53: event_key = 0x45; break;
        default: break;
    }

    uint32_t modifiers = 0;
    if (hid_modifiers & ((1U << 1) | (1U << 5))) modifiers |= NPK_INPUT_MOD_SHIFT;
    if (hid_modifiers & ((1U << 0) | (1U << 4))) modifiers |= NPK_INPUT_MOD_CTRL;
    if (hid_modifiers & (1U << 2)) modifiers |= NPK_INPUT_MOD_ALT;
    if (hid_modifiers & (1U << 6)) modifiers |= NPK_INPUT_MOD_ALT | NPK_INPUT_MOD_ALTGR;
    if (hid_modifiers & ((1U << 3) | (1U << 7))) modifiers |= NPK_INPUT_MOD_GUI;
    if (usage == 0x39U && pressed) caps_lock = !caps_lock;
    if (caps_lock) modifiers |= NPK_INPUT_MOD_CAPSLOCK;
    if (is_extended) modifiers |= NPK_INPUT_MOD_EXTENDED;
    keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, event_key,
                                 pressed ? NPK_INPUT_KEY_PRESS : NPK_INPUT_KEY_RELEASE,
                                 modifiers);

    if (!pressed) return;
    if (usage == 0x28U) { enqueue_byte('\n'); return; }
    if (usage == 0x2aU) { enqueue_byte('\b'); return; }
    if (usage == 0x2bU) { enqueue_byte('\t'); return; }
    if (usage == 0x58U) { enqueue_byte('\n'); return; }
    if (is_extended) return;

    const char *text = NULL;
    if (key < 0x40U) {
        const char **lower = turkish ? tr_lower : us_lower;
        const char **upper = turkish ? tr_upper : us_upper;
        bool letter = usage >= 0x04U && usage <= 0x1dU;
        bool use_upper = (modifiers & NPK_INPUT_MOD_SHIFT) != 0;
        if (letter && (modifiers & NPK_INPUT_MOD_CAPSLOCK)) use_upper = !use_upper;
        if (modifiers & NPK_INPUT_MOD_ALTGR) {
            if (key == 0x12U) text = "€";
        } else {
            text = use_upper ? upper[key] : lower[key];
        }
    }
    if (text) enqueue_text(text);
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
    input_waiter = NULL;
    event_head = event_tail = 0;
    shift_down = altgr_down = caps_lock = extended = false;
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
                         (altgr_down ? (NPK_INPUT_MOD_ALT | NPK_INPUT_MOD_ALTGR) : 0U) |
                         (extended ? NPK_INPUT_MOD_EXTENDED : 0U);
    uint16_t event_code = extended_key_code(key);
    if (event_code != 0) keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, event_code,
                                                         released ? NPK_INPUT_KEY_RELEASE : NPK_INPUT_KEY_PRESS, modifiers);
    else keyboard_enqueue_input_event(NPK_INPUT_EVENT_KEY, key,
                                      released ? NPK_INPUT_KEY_RELEASE : NPK_INPUT_KEY_PRESS, modifiers);
    if (key == 0x2a || key == 0x36) { shift_down = !released; extended = false; return; }
    if (extended && key == 0x38) { altgr_down = !released; extended = false; return; }
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
        if (altgr_down) {
            if (key == 0x12U) text = "€";
        } else {
            bool letter = (key >= 0x10U && key <= 0x1bU) ||
                          (key >= 0x1eU && key <= 0x28U) ||
                          (key >= 0x2cU && key <= 0x34U);
            bool use_upper = shift_down;
            if (letter && caps_lock) use_upper = !use_upper;
            text = use_upper ? upper[key] : lower[key];
        }
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
    uint64_t flags = keyboard_irq_save();
    int value = -1;
    if (tail != head) {
        value = queue[tail];
        tail = (tail + 1) % QUEUE_SIZE;
    }
    keyboard_irq_restore(flags);
    return value;
}

int keyboard_getchar_blocking(void) {
    for (;;) {
        uint64_t flags = keyboard_irq_save();
        if (tail != head) {
            int value = queue[tail];
            tail = (tail + 1) % QUEUE_SIZE;
            keyboard_irq_restore(flags);
            return value;
        }
        thread_t *thread = scheduler_current();
        if (!thread) {
            keyboard_irq_restore(flags);
            __asm__ volatile("sti; hlt" ::: "memory");
            continue;
        }
        input_waiter = thread;
        thread->state = THREAD_BLOCKED;
        uint8_t user_ring = thread->privilege_ring;
        thread->privilege_ring = 0;
        scheduler_yield();
        thread->privilege_ring = user_ring;
        if (user_ring == 3) {
            arch_set_fs_base(thread->fs_base);
            arch_set_user_gs_base(thread->gs_base);
        }
        keyboard_irq_restore(flags);
    }
}
