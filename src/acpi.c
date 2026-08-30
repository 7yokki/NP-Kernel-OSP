#include <npk/acpi.h>
#include <npk/arch.h>
#include <npk/boot.h>
#include <npk/log.h>
#include <npk/memory.h>

#define ACPI_RSDP_REVISION_2 2U
#define ACPI_SYSTEM_MEMORY 0U
#define ACPI_SYSTEM_IO 1U
#define ACPI_PWRBTN_BIT 8U
#define ACPI_SLP_EN_BIT 13U
#define ACPI_SLP_TYP_SHIFT 10U
#define ACPI_MAX_TABLE_LENGTH (1U << 20)
#define ACPI_PKG_MAX_BYTES 4U

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} NPK_PACKED;

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} NPK_PACKED acpi_rsdp_t;

typedef struct {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_width;
    uint64_t address;
} NPK_PACKED acpi_gas_t;

typedef struct {
    struct acpi_sdt_header header;
    uint32_t firmware_control;
    uint32_t dsdt;
    uint8_t model;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t gpe1_base;
    uint8_t cst_control;
    uint16_t p_lvl2_latency;
    uint16_t p_lvl3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_arch_flags;
    uint8_t reserved2;
    uint32_t flags;
    acpi_gas_t reset_register;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_event_block;
    acpi_gas_t x_pm1b_event_block;
    acpi_gas_t x_pm1a_control_block;
    acpi_gas_t x_pm1b_control_block;
} NPK_PACKED acpi_fadt_t;

typedef struct {
    uint64_t address;
    uint8_t address_space;
    bool valid;
} acpi_reg_t;

typedef struct {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} NPK_PACKED acpi_madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} NPK_PACKED acpi_madt_entry_header_t;

typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} NPK_PACKED acpi_madt_ioapic_entry_t;

typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} NPK_PACKED acpi_madt_iso_entry_t;

static bool initialized;
static bool ready;
static bool sci_ioapic;
static uint32_t sci_gsi;
static uint8_t sci_vector = 41;
static uint8_t sci_irq = 0xff;
static acpi_reg_t pm1a_event;
static acpi_reg_t pm1b_event;
static acpi_reg_t pm1a_control;
static acpi_reg_t pm1b_control;
static uint8_t pm1_event_length;
static uint8_t pm1_control_length;
static uint8_t sleep_type_a;
static uint8_t sleep_type_b;
static bool sleep_type_b_valid;

static bool bytes_equal(const uint8_t *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (left[i] != (uint8_t)right[i]) return false;
    return true;
}

static void *acpi_map_physical(uint64_t physical) {
    if (!physical || !g_boot_info.hhdm) return NULL;
    uint64_t offset = g_boot_info.hhdm->offset;
    if (physical > UINT64_MAX - offset) return NULL;
    return (void *)(physical + offset);
}

static acpi_rsdp_t *acpi_rsdp_pointer(void) {
    if (!g_boot_info.rsdp) return NULL;
    if (g_boot_info.rsdp_is_virtual)
        return (acpi_rsdp_t *)(uint64_t)g_boot_info.rsdp;
    return (acpi_rsdp_t *)acpi_map_physical(g_boot_info.rsdp);
}

static bool acpi_checksum(const void *address, size_t length) {
    const uint8_t *bytes = (const uint8_t *)address;
    uint8_t sum = 0;
    if (!address || length == 0 || length > ACPI_MAX_TABLE_LENGTH) return false;
    for (size_t i = 0; i < length; ++i) sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static bool acpi_sdt_valid(const struct acpi_sdt_header *header, const char *signature) {
    if (!header || header->length < sizeof(*header) || header->length > ACPI_MAX_TABLE_LENGTH)
        return false;
    if (signature && !bytes_equal((const uint8_t *)header->signature, signature, 4))
        return false;
    return acpi_checksum(header, header->length);
}

static struct acpi_sdt_header *acpi_table_from_physical(uint64_t physical, const char *signature) {
    struct acpi_sdt_header *header = (struct acpi_sdt_header *)acpi_map_physical(physical);
    return acpi_sdt_valid(header, signature) ? header : NULL;
}

static struct acpi_sdt_header *acpi_find_table(const struct acpi_sdt_header *root,
                                                bool xsdt, const char *signature) {
    if (!root || root->length < sizeof(*root)) return NULL;
    size_t entry_size = xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t payload = root->length - sizeof(*root);
    if (payload % entry_size != 0) return NULL;
    size_t count = payload / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (size_t i = 0; i < count; ++i) {
        uint64_t physical;
        if (xsdt)
            physical = ((const uint64_t *)entries)[i];
        else
            physical = ((const uint32_t *)entries)[i];
        struct acpi_sdt_header *candidate = acpi_table_from_physical(physical, signature);
        if (candidate) return candidate;
    }
    return NULL;
}

static bool aml_package_length(const uint8_t *aml, size_t available, size_t *length,
                               size_t *encoded_bytes) {
    if (!aml || !length || !encoded_bytes || available == 0) return false;
    uint8_t follow = (uint8_t)(aml[0] >> 6);
    if (follow >= ACPI_PKG_MAX_BYTES || (size_t)follow + 1 > available) return false;
    size_t value = aml[0] & 0x0f;
    for (uint8_t i = 0; i < follow; ++i)
        value |= (size_t)aml[1 + i] << (4 + 8 * i);
    *length = value;
    *encoded_bytes = (size_t)follow + 1;
    return true;
}

static bool aml_integer(const uint8_t *aml, size_t available, uint8_t *value, size_t *used) {
    if (!aml || !value || !used || available == 0) return false;
    switch (aml[0]) {
        case 0x00: *value = 0; *used = 1; return true;
        case 0x01: *value = 1; *used = 1; return true;
        case 0x0a:
            if (available < 2) return false;
            *value = aml[1]; *used = 2; return true;
        case 0x0b:
            if (available < 3 || aml[2] != 0) return false;
            *value = aml[1]; *used = 3; return true;
        case 0x0c:
            if (available < 5 || aml[2] != 0 || aml[3] != 0 || aml[4] != 0) return false;
            *value = aml[1]; *used = 5; return true;
        case 0x0e:
            if (available < 9 || aml[2] != 0 || aml[3] != 0 || aml[4] != 0 ||
                aml[5] != 0 || aml[6] != 0 || aml[7] != 0 || aml[8] != 0) return false;
            *value = aml[1]; *used = 9; return true;
        default:
            return false;
    }
}

static bool acpi_find_s5(const struct acpi_sdt_header *dsdt, uint8_t *type_a,
                         uint8_t *type_b, bool *has_type_b) {
    if (!dsdt || !type_a || !type_b || !has_type_b || dsdt->length < sizeof(*dsdt)) return false;
    const uint8_t *aml = (const uint8_t *)dsdt + sizeof(*dsdt);
    size_t length = dsdt->length - sizeof(*dsdt);
    for (size_t i = 0; i + 6 < length; ++i) {
        if (aml[i] != 0x08 || !bytes_equal(aml + i + 1, "_S5_", 4) || aml[i + 5] != 0x12)
            continue;
        size_t package_length, package_length_bytes;
        if (!aml_package_length(aml + i + 6, length - i - 6, &package_length,
                                &package_length_bytes)) continue;
        if (package_length < package_length_bytes + 1 ||
            package_length > length - i - 6) continue;
        const uint8_t *package = aml + i + 6 + package_length_bytes;
        size_t package_bytes = package_length - package_length_bytes;
        uint8_t count = package[0];
        if (count < 1) continue;
        size_t used_a;
        if (!aml_integer(package + 1, package_bytes - 1, type_a, &used_a)) continue;
        *has_type_b = false;
        if (count >= 2 && used_a + 1 < package_bytes) {
            size_t used_b;
            if (aml_integer(package + 1 + used_a, package_bytes - 1 - used_a,
                            type_b, &used_b)) *has_type_b = true;
        }
        return *type_a < 8;
    }
    return false;
}

static acpi_reg_t acpi_reg_from_gas(acpi_gas_t gas) {
    acpi_reg_t result = { .address = gas.address, .address_space = gas.address_space,
                          .valid = gas.address != 0 &&
                                   (gas.address_space == ACPI_SYSTEM_MEMORY ||
                                    gas.address_space == ACPI_SYSTEM_IO) };
    return result;
}

static acpi_reg_t acpi_reg_from_legacy(uint32_t address) {
    acpi_reg_t result = { .address = address, .address_space = ACPI_SYSTEM_IO,
                          .valid = address != 0 };
    return result;
}

static uint16_t acpi_read16(acpi_reg_t reg, uint16_t offset) {
    if (!reg.valid || reg.address > UINT64_MAX - offset) return 0;
    uint64_t address = reg.address + offset;
    if (reg.address_space == ACPI_SYSTEM_IO) {
        if (address > UINT16_MAX) return 0;
        return inw((uint16_t)address);
    }
    volatile uint16_t *memory = (volatile uint16_t *)acpi_map_physical(address);
    return memory ? *memory : 0;
}

static bool acpi_write16(acpi_reg_t reg, uint16_t offset, uint16_t value) {
    if (!reg.valid || reg.address > UINT64_MAX - offset) return false;
    uint64_t address = reg.address + offset;
    if (reg.address_space == ACPI_SYSTEM_IO) {
        if (address > UINT16_MAX) return false;
        outw((uint16_t)address, value);
        return true;
    }
    volatile uint16_t *memory = (volatile uint16_t *)acpi_map_physical(address);
    if (!memory) return false;
    *memory = value;
    compiler_barrier();
    return true;
}

static volatile uint32_t *acpi_mmio32(uint64_t physical) {
    return (volatile uint32_t *)acpi_map_physical(physical);
}

static uint32_t ioapic_read(uint64_t physical, uint8_t register_number) {
    volatile uint32_t *select = acpi_mmio32(physical);
    volatile uint32_t *window = acpi_mmio32(physical + 0x10);
    if (!select || !window) return 0;
    *select = register_number;
    compiler_barrier();
    return *window;
}

static bool ioapic_write(uint64_t physical, uint8_t register_number, uint32_t value) {
    volatile uint32_t *select = acpi_mmio32(physical);
    volatile uint32_t *window = acpi_mmio32(physical + 0x10);
    if (!select || !window) return false;
    *select = register_number;
    compiler_barrier();
    *window = value;
    compiler_barrier();
    return true;
}

static bool acpi_route_sci_ioapic(const struct acpi_sdt_header *root, bool xsdt) {
    if (sci_irq == 0xff) return false;
    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table(root, xsdt, "APIC");
    if (!madt || madt->header.length < sizeof(*madt)) return false;
    uint32_t gsi = sci_irq;
    const uint8_t *cursor = (const uint8_t *)madt + sizeof(*madt);
    size_t remaining = madt->header.length - sizeof(*madt);
    while (remaining >= sizeof(acpi_madt_entry_header_t)) {
        const acpi_madt_entry_header_t *entry = (const acpi_madt_entry_header_t *)cursor;
        if (entry->length < sizeof(*entry) || entry->length > remaining) break;
        if (entry->type == 2 && entry->length >= sizeof(acpi_madt_iso_entry_t)) {
            const acpi_madt_iso_entry_t *iso = (const acpi_madt_iso_entry_t *)cursor;
            if (iso->bus == 0 && iso->source_irq == sci_irq) gsi = iso->gsi;
        }
        cursor += entry->length;
        remaining -= entry->length;
    }
    cursor = (const uint8_t *)madt + sizeof(*madt);
    remaining = madt->header.length - sizeof(*madt);
    uint64_t ioapic_physical = 0;
    uint32_t ioapic_gsi_base = 0;
    uint32_t ioapic_max_redirection = 0;
    while (remaining >= sizeof(acpi_madt_entry_header_t)) {
        const acpi_madt_entry_header_t *entry = (const acpi_madt_entry_header_t *)cursor;
        if (entry->length < sizeof(*entry) || entry->length > remaining) break;
        if (entry->type == 1 && entry->length >= sizeof(acpi_madt_ioapic_entry_t)) {
            const acpi_madt_ioapic_entry_t *io = (const acpi_madt_ioapic_entry_t *)cursor;
            uint32_t max_redirection = (ioapic_read(io->address, 1) >> 16) & 0xffU;
            if (gsi >= io->gsi_base && gsi - io->gsi_base <= max_redirection) {
                ioapic_physical = io->address;
                ioapic_gsi_base = io->gsi_base;
                ioapic_max_redirection = max_redirection;
                break;
            }
        }
        cursor += entry->length;
        remaining -= entry->length;
    }
    if (!ioapic_physical || gsi < ioapic_gsi_base || gsi - ioapic_gsi_base > ioapic_max_redirection)
        return false;
    volatile uint32_t *lapic = acpi_mmio32(madt->local_apic_address);
    if (!lapic) return false;
    uint32_t lapic_id = (*((volatile uint32_t *)((uint8_t *)lapic + 0x20)) >> 24) & 0xffU;
    uint8_t redirection = (uint8_t)(2U * (gsi - ioapic_gsi_base));
    uint32_t low = (uint32_t)sci_vector | (1U << 13) | (1U << 15); /* active-low, level */
    if (!ioapic_write(ioapic_physical, (uint8_t)(redirection + 1), lapic_id << 24)) return false;
    if (!ioapic_write(ioapic_physical, redirection, low)) return false;
    volatile uint32_t *spurious = (volatile uint32_t *)((uint8_t *)lapic + 0xf0);
    *spurious |= 0x100U | 0xffU;
    compiler_barrier();
    sci_ioapic = true;
    sci_gsi = gsi;
    return true;
}

static void acpi_enable_mode(const acpi_fadt_t *fadt) {
    if (!pm1a_control.valid || pm1_control_length < 2) return;
    if ((acpi_read16(pm1a_control, 0) & 1U) != 0) return;
    if (!fadt->smi_command || !fadt->acpi_enable) {
        log_message(LOG_WARN, "acpi", "firmware did not expose ACPI enable command");
        return;
    }
    outb((uint16_t)fadt->smi_command, fadt->acpi_enable);
    for (unsigned i = 0; i < 10000; ++i) {
        if ((acpi_read16(pm1a_control, 0) & 1U) != 0) return;
        __asm__ volatile ("pause");
    }
    log_message(LOG_WARN, "acpi", "firmware ACPI enable transition did not complete");
}

static void acpi_enable_power_button(void) {
    if (!pm1a_event.valid || pm1_event_length < 4) return;
    (void)acpi_write16(pm1a_event, 0, (uint16_t)(1U << ACPI_PWRBTN_BIT));
    uint16_t enable = (uint16_t)(acpi_read16(pm1a_event, 2) | (1U << ACPI_PWRBTN_BIT));
    (void)acpi_write16(pm1a_event, 2, enable);
    if (pm1b_event.valid) {
        (void)acpi_write16(pm1b_event, 0, (uint16_t)(1U << ACPI_PWRBTN_BIT));
        enable = (uint16_t)(acpi_read16(pm1b_event, 2) | (1U << ACPI_PWRBTN_BIT));
        (void)acpi_write16(pm1b_event, 2, enable);
    }
}

void acpi_init(void) {
    initialized = true;
    ready = false;
    if (!g_boot_info.rsdp) {
        log_message(LOG_WARN, "acpi", "Limine did not provide an RSDP");
        return;
    }
    acpi_rsdp_t *rsdp = acpi_rsdp_pointer();
    if (!rsdp || !bytes_equal((const uint8_t *)rsdp->signature, "RSD PTR ", 8) ||
        !acpi_checksum(rsdp, 20)) {
        log_message(LOG_WARN, "acpi", "invalid RSDP checksum or signature");
        return;
    }
    bool extended_valid = rsdp->revision < ACPI_RSDP_REVISION_2 ||
                          (rsdp->length >= sizeof(*rsdp) && acpi_checksum(rsdp, rsdp->length));
    bool use_xsdt = rsdp->revision >= ACPI_RSDP_REVISION_2 && extended_valid &&
                    rsdp->xsdt_address != 0;
    uint64_t root_physical = use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
    struct acpi_sdt_header *root = acpi_table_from_physical(root_physical, use_xsdt ? "XSDT" : "RSDT");
    if (!root) {
        log_message(LOG_WARN, "acpi", "valid ACPI root table not found");
        return;
    }
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table(root, use_xsdt, "FACP");
    if (!fadt || fadt->header.length < offsetof(acpi_fadt_t, pm1_control_length) + 2) {
        log_message(LOG_WARN, "acpi", "FADT unavailable or too short");
        return;
    }
    LOG_INFOF("acpi", use_xsdt ? "using XSDT" : "using RSDT", fadt->header.length);
    sci_irq = fadt->sci_interrupt <= 255 ? (uint8_t)fadt->sci_interrupt : 0xff;
    pm1_event_length = fadt->pm1_event_length;
    pm1_control_length = fadt->pm1_control_length;
    pm1a_event = acpi_reg_from_legacy(fadt->pm1a_event_block);
    pm1b_event = acpi_reg_from_legacy(fadt->pm1b_event_block);
    pm1a_control = acpi_reg_from_legacy(fadt->pm1a_control_block);
    pm1b_control = acpi_reg_from_legacy(fadt->pm1b_control_block);
    if (fadt->header.length >= offsetof(acpi_fadt_t, x_pm1b_control_block) + sizeof(acpi_gas_t)) {
        if (fadt->x_pm1a_event_block.address) pm1a_event = acpi_reg_from_gas(fadt->x_pm1a_event_block);
        if (fadt->x_pm1b_event_block.address) pm1b_event = acpi_reg_from_gas(fadt->x_pm1b_event_block);
        if (fadt->x_pm1a_control_block.address) pm1a_control = acpi_reg_from_gas(fadt->x_pm1a_control_block);
        if (fadt->x_pm1b_control_block.address) pm1b_control = acpi_reg_from_gas(fadt->x_pm1b_control_block);
    }
    uint64_t dsdt_physical = fadt->dsdt;
    if (fadt->header.length >= offsetof(acpi_fadt_t, x_dsdt) + sizeof(fadt->x_dsdt) && fadt->x_dsdt)
        dsdt_physical = fadt->x_dsdt;
    struct acpi_sdt_header *dsdt = acpi_table_from_physical(dsdt_physical, "DSDT");
    if (!dsdt) {
        log_message(LOG_WARN, "acpi", "DSDT table invalid or inaccessible");
        return;
    }
    bool found_s5 = acpi_find_s5(dsdt, &sleep_type_a, &sleep_type_b, &sleep_type_b_valid);
    if (!found_s5) {
        log_message(LOG_WARN, "acpi", "DSDT _S5_ sleep package not found");
        return;
    }
    acpi_enable_mode(fadt);
    acpi_enable_power_button();
    ready = pm1a_event.valid && pm1a_control.valid && pm1_event_length >= 4 && pm1_control_length >= 2;
    if (ready && acpi_route_sci_ioapic(root, use_xsdt)) {
        LOG_INFOF("acpi", "SCI IOAPIC GSI", sci_gsi);
        LOG_INFOF("acpi", "SCI IOAPIC vector", sci_vector);
    } else {
        sci_ioapic = false;
        log_message(LOG_WARN, "acpi", "SCI IOAPIC route unavailable; using PIC/poll fallback");
    }
    LOG_INFOF("acpi", "SCI IRQ", sci_irq);
    LOG_INFOF("acpi", "PM1a control", pm1a_control.address);
    LOG_INFOF("acpi", "S5 sleep type A", sleep_type_a);
    if (sleep_type_b_valid) LOG_INFOF("acpi", "S5 sleep type B", sleep_type_b);
    if (!ready) log_message(LOG_WARN, "acpi", "PM1 shutdown registers incomplete");
    else if (sci_ioapic) log_message(LOG_INFO, "acpi", "ACPI PM1 power button, SCI, and S5 ready");
    else log_message(LOG_INFO, "acpi", "ACPI PM1 power button and S5 ready; SCI fallback active");
}

bool acpi_available(void) { return initialized && ready; }
uint8_t acpi_sci_irq(void) { return sci_irq; }

static void acpi_handle_power_button(void) {
    if (!acpi_available()) return;
    uint16_t status = acpi_read16(pm1a_event, 0);
    uint16_t enable = acpi_read16(pm1a_event, 2);
    uint16_t power_button = (uint16_t)(1U << ACPI_PWRBTN_BIT);
    if ((status & power_button) == 0 || (enable & power_button) == 0) return;
    (void)acpi_write16(pm1a_event, 0, power_button);
    if (pm1b_event.valid) (void)acpi_write16(pm1b_event, 0, power_button);
    log_message(LOG_INFO, "acpi", "power button event; entering S5 soft-off");
    acpi_poweroff();
}

void acpi_irq_dispatch(void) { acpi_handle_power_button(); }
void acpi_poll(void) { acpi_handle_power_button(); }

NPK_NORETURN void acpi_poweroff(void) {
    if (!acpi_available()) {
        log_message(LOG_ERROR, "acpi", "poweroff requested without a valid PM1 backend");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint16_t value_a = (uint16_t)((sleep_type_a << ACPI_SLP_TYP_SHIFT) | (1U << ACPI_SLP_EN_BIT));
    uint16_t value_b = (uint16_t)((sleep_type_b << ACPI_SLP_TYP_SHIFT) | (1U << ACPI_SLP_EN_BIT));
    __asm__ volatile ("cli" ::: "memory");
    (void)acpi_write16(pm1a_control, 0, value_a);
    if (pm1b_control.valid && sleep_type_b_valid) (void)acpi_write16(pm1b_control, 0, value_b);
    log_message(LOG_ERROR, "acpi", "S5 write returned; firmware did not power off");
    for (;;) __asm__ volatile ("hlt");
}
