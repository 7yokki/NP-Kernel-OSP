#include <npk/device_config.h>
#include <npk/string.h>
#include <npk/vfs.h>

#define DDC_MAX_FILE_BYTES 16384U
#define DDC_MAX_CATEGORIES 16U
#define DDC_MAX_PRODUCTS 64U
#define DDC_TEXT_CAPACITY 64U
#define DDC_BUS_CAPACITY 8U
#define DDC_MAX_JSON_DEPTH 8U

#define DDC_EINVAL 22
#define DDC_EFBIG 27
#define DDC_EIO 5

typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    char type[NPK_DEVICE_TYPE_NAME_CAPACITY];
    char generic_name[NPK_DEVICE_DISPLAY_NAME_CAPACITY];
    char driver_name[NPK_DEVICE_DRIVER_NAME_CAPACITY];
} ddc_category_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char type[NPK_DEVICE_TYPE_NAME_CAPACITY];
    char model_name[NPK_DEVICE_DISPLAY_NAME_CAPACITY];
} ddc_product_t;

typedef struct {
    const char *data;
    size_t length;
    size_t position;
} json_cursor_t;

static ddc_category_t categories[DDC_MAX_CATEGORIES];
static ddc_product_t products[DDC_MAX_PRODUCTS];
static size_t category_count;
static size_t product_count;
static bool loaded;
static char config_buffer[DDC_MAX_FILE_BYTES + 1U];

static bool text_equal(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool text_copy(char *destination, size_t capacity, const char *source) {
    if (!destination || !capacity || !source) return false;
    size_t length = strlen(source);
    if (length == 0 || length >= capacity) return false;
    memcpy(destination, source, length + 1U);
    return true;
}

void device_config_reset(void) {
    memset(categories, 0, sizeof(categories));
    memset(products, 0, sizeof(products));
    memset(config_buffer, 0, sizeof(config_buffer));
    category_count = 0;
    product_count = 0;
    loaded = false;
}

static void skip_space(json_cursor_t *cursor) {
    while (cursor->position < cursor->length) {
        char c = cursor->data[cursor->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++cursor->position;
    }
}

static bool take_char(json_cursor_t *cursor, char expected) {
    skip_space(cursor);
    if (cursor->position >= cursor->length || cursor->data[cursor->position] != expected)
        return false;
    ++cursor->position;
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_json_string(json_cursor_t *cursor, char *output, size_t capacity) {
    if (!take_char(cursor, '"') || !output || capacity == 0) return false;
    size_t used = 0;
    while (cursor->position < cursor->length) {
        unsigned char c = (unsigned char)cursor->data[cursor->position++];
        if (c == '"') {
            output[used] = '\0';
            return used != 0;
        }
        if (c == '\\') {
            if (cursor->position >= cursor->length) return false;
            char escape = cursor->data[cursor->position++];
            switch (escape) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (cursor->length - cursor->position < 4U) return false;
                    unsigned code = 0;
                    for (unsigned i = 0; i < 4U; ++i) {
                        int digit = hex_digit(cursor->data[cursor->position++]);
                        if (digit < 0) return false;
                        code = (code << 4) | (unsigned)digit;
                    }
                    /* Catalog identifiers/names are UTF-8 literals; escaped
                     * Unicode is accepted only for ASCII to keep the parser bounded. */
                    if (code > 0x7fU) return false;
                    c = (unsigned char)code;
                    break;
                }
                default: return false;
            }
        }
        if (c < 0x20U || used + 1U >= capacity) return false;
        output[used++] = (char)c;
    }
    return false;
}

static bool skip_json_string(json_cursor_t *cursor) {
    if (!take_char(cursor, '"')) return false;
    while (cursor->position < cursor->length) {
        unsigned char c = (unsigned char)cursor->data[cursor->position++];
        if (c == '"') return true;
        if (c < 0x20U) return false;
        if (c == '\\') {
            if (cursor->position >= cursor->length) return false;
            char escape = cursor->data[cursor->position++];
            if (escape == 'u') {
                if (cursor->length - cursor->position < 4U) return false;
                for (unsigned i = 0; i < 4U; ++i)
                    if (hex_digit(cursor->data[cursor->position++]) < 0) return false;
            } else if (escape != '"' && escape != '\\' && escape != '/' &&
                       escape != 'b' && escape != 'f' && escape != 'n' &&
                       escape != 'r' && escape != 't') {
                return false;
            }
        }
    }
    return false;
}

static bool parse_uint32(json_cursor_t *cursor, uint32_t *value) {
    if (!value) return false;
    skip_space(cursor);
    if (cursor->position >= cursor->length) return false;
    size_t start = cursor->position;
    uint32_t parsed = 0;
    while (cursor->position < cursor->length) {
        char c = cursor->data[cursor->position];
        if (c < '0' || c > '9') break;
        uint32_t digit = (uint32_t)(c - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        ++cursor->position;
    }
    if (cursor->position == start) return false;
    if (cursor->position - start > 1U && cursor->data[start] == '0') return false;
    *value = parsed;
    return true;
}

static bool skip_json_value(json_cursor_t *cursor, unsigned depth);

static bool skip_json_object(json_cursor_t *cursor, unsigned depth) {
    if (depth > DDC_MAX_JSON_DEPTH || !take_char(cursor, '{')) return false;
    skip_space(cursor);
    if (take_char(cursor, '}')) return true;
    for (;;) {
        if (!skip_json_string(cursor) || !take_char(cursor, ':') ||
            !skip_json_value(cursor, depth + 1U)) return false;
        if (take_char(cursor, '}')) return true;
        if (!take_char(cursor, ',')) return false;
    }
}

static bool skip_json_array(json_cursor_t *cursor, unsigned depth) {
    if (depth > DDC_MAX_JSON_DEPTH || !take_char(cursor, '[')) return false;
    skip_space(cursor);
    if (take_char(cursor, ']')) return true;
    for (;;) {
        if (!skip_json_value(cursor, depth + 1U)) return false;
        if (take_char(cursor, ']')) return true;
        if (!take_char(cursor, ',')) return false;
    }
}

static bool skip_json_value(json_cursor_t *cursor, unsigned depth) {
    if (depth > DDC_MAX_JSON_DEPTH) return false;
    skip_space(cursor);
    if (cursor->position >= cursor->length) return false;
    char c = cursor->data[cursor->position];
    if (c == '"') return skip_json_string(cursor);
    if (c == '{') return skip_json_object(cursor, depth);
    if (c == '[') return skip_json_array(cursor, depth);
    size_t start = cursor->position;
    while (cursor->position < cursor->length) {
        c = cursor->data[cursor->position];
        if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' ||
            c == '\r' || c == '\n') break;
        ++cursor->position;
    }
    if (cursor->position == start) return false;
    return true;
}

static bool parse_category(json_cursor_t *cursor, ddc_category_t *category) {
    if (!category || !take_char(cursor, '{')) return false;
    char bus[DDC_BUS_CAPACITY] = {0};
    ddc_category_t parsed = {0};
    bool has_bus = false, has_class = false, has_subclass = false;
    bool has_protocol = false, has_type = false, has_name = false, has_driver = false;
    skip_space(cursor);
    if (!take_char(cursor, '}')) {
        for (;;) {
            char key[DDC_TEXT_CAPACITY];
            if (!parse_json_string(cursor, key, sizeof(key)) || !take_char(cursor, ':')) return false;
            uint32_t number;
            if (text_equal(key, "bus")) {
                if (!parse_json_string(cursor, bus, sizeof(bus))) return false;
                has_bus = true;
            } else if (text_equal(key, "class")) {
                if (!parse_uint32(cursor, &number) || number > 255U) return false;
                parsed.class_code = (uint8_t)number; has_class = true;
            } else if (text_equal(key, "subclass")) {
                if (!parse_uint32(cursor, &number) || number > 255U) return false;
                parsed.subclass = (uint8_t)number; has_subclass = true;
            } else if (text_equal(key, "protocol")) {
                if (!parse_uint32(cursor, &number) || number > 255U) return false;
                parsed.protocol = (uint8_t)number; has_protocol = true;
            } else if (text_equal(key, "type")) {
                if (!parse_json_string(cursor, parsed.type, sizeof(parsed.type))) return false;
                has_type = true;
            } else if (text_equal(key, "generic_name")) {
                if (!parse_json_string(cursor, parsed.generic_name, sizeof(parsed.generic_name))) return false;
                has_name = true;
            } else if (text_equal(key, "driver")) {
                if (!parse_json_string(cursor, parsed.driver_name, sizeof(parsed.driver_name))) return false;
                has_driver = true;
            } else if (!skip_json_value(cursor, 0)) {
                return false;
            }
            if (take_char(cursor, '}')) break;
            if (!take_char(cursor, ',')) return false;
        }
    }
    if (!has_bus || !text_equal(bus, "usb") || !has_class || !has_subclass ||
        !has_protocol || !has_type || !has_name || !has_driver) return false;
    *category = parsed;
    return true;
}

static bool parse_product(json_cursor_t *cursor, ddc_product_t *product) {
    if (!product || !take_char(cursor, '{')) return false;
    ddc_product_t parsed = {0};
    bool has_vid = false, has_pid = false, has_type = false, has_name = false;
    skip_space(cursor);
    if (!take_char(cursor, '}')) {
        for (;;) {
            char key[DDC_TEXT_CAPACITY];
            if (!parse_json_string(cursor, key, sizeof(key)) || !take_char(cursor, ':')) return false;
            uint32_t number;
            if (text_equal(key, "vid")) {
                if (!parse_uint32(cursor, &number) || number > 0xffffU) return false;
                parsed.vendor_id = (uint16_t)number; has_vid = true;
            } else if (text_equal(key, "pid")) {
                if (!parse_uint32(cursor, &number) || number > 0xffffU) return false;
                parsed.product_id = (uint16_t)number; has_pid = true;
            } else if (text_equal(key, "type")) {
                if (!parse_json_string(cursor, parsed.type, sizeof(parsed.type))) return false;
                has_type = true;
            } else if (text_equal(key, "name")) {
                if (!parse_json_string(cursor, parsed.model_name, sizeof(parsed.model_name))) return false;
                has_name = true;
            } else if (!skip_json_value(cursor, 0)) {
                return false;
            }
            if (take_char(cursor, '}')) break;
            if (!take_char(cursor, ',')) return false;
        }
    }
    if (!has_vid || !has_pid || !has_type || !has_name) return false;
    *product = parsed;
    return true;
}

static bool parse_categories(json_cursor_t *cursor) {
    if (!take_char(cursor, '[')) return false;
    skip_space(cursor);
    if (take_char(cursor, ']')) return true;
    for (;;) {
        if (category_count >= DDC_MAX_CATEGORIES ||
            !parse_category(cursor, &categories[category_count])) return false;
        ++category_count;
        if (take_char(cursor, ']')) return true;
        if (!take_char(cursor, ',')) return false;
    }
}

static bool parse_products(json_cursor_t *cursor) {
    if (!take_char(cursor, '[')) return false;
    skip_space(cursor);
    if (take_char(cursor, ']')) return true;
    for (;;) {
        if (product_count >= DDC_MAX_PRODUCTS ||
            !parse_product(cursor, &products[product_count])) return false;
        ++product_count;
        if (take_char(cursor, ']')) return true;
        if (!take_char(cursor, ',')) return false;
    }
}

static bool category_type_exists(const char *type) {
    for (size_t i = 0; i < category_count; ++i)
        if (text_equal(categories[i].type, type)) return true;
    return false;
}

static bool parse_document(const char *data, size_t length) {
    json_cursor_t cursor = { .data = data, .length = length, .position = 0 };
    uint32_t version = 0;
    bool has_version = false, has_categories = false;
    if (!take_char(&cursor, '{')) return false;
    skip_space(&cursor);
    if (!take_char(&cursor, '}')) {
        for (;;) {
            char key[DDC_TEXT_CAPACITY];
            if (!parse_json_string(&cursor, key, sizeof(key)) || !take_char(&cursor, ':')) return false;
            if (text_equal(key, "version")) {
                if (!parse_uint32(&cursor, &version)) return false;
                has_version = true;
            } else if (text_equal(key, "categories")) {
                if (!parse_categories(&cursor)) return false;
                has_categories = true;
            } else if (text_equal(key, "products")) {
                if (!parse_products(&cursor)) return false;
            } else if (!skip_json_value(&cursor, 0)) {
                return false;
            }
            if (take_char(&cursor, '}')) break;
            if (!take_char(&cursor, ',')) return false;
        }
    }
    skip_space(&cursor);
    if (cursor.position != cursor.length || !has_version || version != 1U ||
        !has_categories || category_count == 0) return false;
    for (size_t i = 0; i < category_count; ++i) {
        for (size_t j = i + 1U; j < category_count; ++j) {
            if (categories[i].class_code == categories[j].class_code &&
                categories[i].subclass == categories[j].subclass &&
                categories[i].protocol == categories[j].protocol) return false;
        }
    }
    for (size_t i = 0; i < product_count; ++i) {
        if (!category_type_exists(products[i].type)) return false;
        for (size_t j = i + 1U; j < product_count; ++j) {
            if (products[i].vendor_id == products[j].vendor_id &&
                products[i].product_id == products[j].product_id &&
                text_equal(products[i].type, products[j].type)) return false;
        }
    }
    return true;
}

int device_config_load_vfs(const char *path) {
    if (!path || path[0] != '/') return -DDC_EINVAL;
    if (loaded) return 0;
    device_config_reset();
    int fd = vfs_open(path);
    if (fd < 0) return fd;
    ssize_t file_size = vfs_size(fd);
    if (file_size <= 0 || (uint64_t)file_size > DDC_MAX_FILE_BYTES) {
        (void)vfs_close(fd);
        return file_size > (ssize_t)DDC_MAX_FILE_BYTES ? -DDC_EFBIG : -DDC_EINVAL;
    }
    ssize_t bytes = vfs_read(fd, config_buffer, (size_t)file_size);
    (void)vfs_close(fd);
    if (bytes != file_size) {
        device_config_reset();
        return -DDC_EIO;
    }
    config_buffer[file_size] = '\0';
    if (!parse_document(config_buffer, (size_t)file_size)) {
        device_config_reset();
        return -DDC_EINVAL;
    }
    loaded = true;
    return 0;
}

bool device_config_resolve(const npk_device_identity_t *identity,
                           npk_device_config_match_t *match) {
    if (!loaded || !identity || !match || identity->bus != NPK_DEVICE_BUS_USB ||
        identity->interface_number == 0xffU) return false;
    for (size_t i = 0; i < category_count; ++i) {
        const ddc_category_t *category = &categories[i];
        if (identity->class_code != category->class_code ||
            identity->subclass != category->subclass ||
            identity->protocol != category->protocol) continue;
        memset(match, 0, sizeof(*match));
        if (!text_copy(match->type, sizeof(match->type), category->type) ||
            !text_copy(match->display_name, sizeof(match->display_name), category->generic_name) ||
            !text_copy(match->driver_name, sizeof(match->driver_name), category->driver_name))
            return false;
        for (size_t p = 0; p < product_count; ++p) {
            const ddc_product_t *product = &products[p];
            if (product->vendor_id == identity->vendor_id &&
                product->product_id == identity->product_id &&
                text_equal(product->type, category->type)) {
                if (!text_copy(match->display_name, sizeof(match->display_name), product->model_name))
                    return false;
                break;
            }
        }
        return true;
    }
    return false;
}
