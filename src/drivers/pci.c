#include <npk/arch.h>
#include <npk/device_manager.h>
#include <npk/log.h>
#include <npk/pci.h>

#define PCI_CONFIG_ADDRESS 0xcf8U
#define PCI_CONFIG_DATA 0xcfcU
#define PCI_CONFIG_ENABLE 0x80000000U

static npk_pci_device_t devices[NPK_PCI_MAX_DEVICES];
static size_t device_count;

static bool valid_config_offset(uint8_t offset) {
    return (offset & 3U) == 0;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    if (slot >= 32 || function >= 8 || !valid_config_offset(offset)) return UINT32_MAX;
    uint32_t address = PCI_CONFIG_ENABLE | ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) | ((uint32_t)function << 8) | offset;
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    if (slot >= 32 || function >= 8 || !valid_config_offset(offset)) return;
    uint32_t address = PCI_CONFIG_ENABLE | ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) | ((uint32_t)function << 8) | offset;
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

bool pci_get_bar_info(const npk_pci_device_t *device, uint8_t bar_index,
                      npk_pci_bar_info_t *info) {
    if (!device || !info || bar_index >= 6 || (device->header_type & 0x7fU) != 0)
        return false;
    bool registered = false;
    for (size_t i = 0; i < device_count; ++i)
        if (&devices[i] == device) { registered = true; break; }
    if (!registered) return false;

    uint8_t bus = device->bus, slot = device->slot, function = device->function;
    uint8_t offset = (uint8_t)(0x10U + bar_index * 4U);
    uint32_t command_status = pci_config_read32(bus, slot, function, 0x04);
    uint16_t command = (uint16_t)command_status;
    uint32_t original_low = pci_config_read32(bus, slot, function, offset);
    uint32_t original_high = 0;
    bool is_io = (original_low & 1U) != 0;
    bool is_64bit = false;
    bool prefetchable = false;

    if (original_low == UINT32_MAX) return false;
    if (!is_io) {
        uint8_t memory_type = (uint8_t)((original_low >> 1) & 3U);
        prefetchable = (original_low & 8U) != 0;
        if (memory_type == 2U) {
            if (bar_index == 5) return false;
            is_64bit = true;
            original_high = pci_config_read32(bus, slot, function, (uint8_t)(offset + 4U));
        } else if (memory_type != 0U) {
            return false;
        }
    }

    /* Write only the 16-bit command field. The upper status word is W1C. */
    pci_config_write32(bus, slot, function, 0x04, (uint32_t)(command & ~3U));
    uint32_t mask_low = 0, mask_high = 0;
    if (is_64bit) {
        pci_config_write32(bus, slot, function, (uint8_t)(offset + 4U), UINT32_MAX);
        pci_config_write32(bus, slot, function, offset, UINT32_MAX);
        mask_low = pci_config_read32(bus, slot, function, offset);
        mask_high = pci_config_read32(bus, slot, function, (uint8_t)(offset + 4U));
        pci_config_write32(bus, slot, function, offset, original_low);
        pci_config_write32(bus, slot, function, (uint8_t)(offset + 4U), original_high);
    } else {
        pci_config_write32(bus, slot, function, offset, UINT32_MAX);
        mask_low = pci_config_read32(bus, slot, function, offset);
        pci_config_write32(bus, slot, function, offset, original_low);
    }
    pci_config_write32(bus, slot, function, 0x04, command);

    uint64_t base, mask, size;
    if (is_io) {
        base = original_low & ~3U;
        mask = mask_low & ~3U;
        size = (uint32_t)(~(uint32_t)mask + 1U);
    } else if (is_64bit) {
        base = ((uint64_t)original_high << 32) | (original_low & ~0x0fU);
        mask = ((uint64_t)mask_high << 32) | (mask_low & ~0x0fU);
        size = ~mask + 1U;
    } else {
        base = original_low & ~0x0fU;
        mask = mask_low & ~0x0fU;
        size = (uint32_t)(~(uint32_t)mask + 1U);
    }
    if (!mask || !size || (size & (size - 1U)) || base > UINT64_MAX - size)
        return false;
    *info = (npk_pci_bar_info_t){
        .base = base,
        .size = size,
        .is_io = is_io,
        .is_64bit = is_64bit,
        .prefetchable = prefetchable
    };
    return true;
}

void pci_init(void) {
    device_count = 0;
    for (uint32_t bus = 0; bus < 256 && device_count < NPK_PCI_MAX_DEVICES; ++bus) {
        for (uint32_t slot = 0; slot < 32 && device_count < NPK_PCI_MAX_DEVICES; ++slot) {
            for (uint32_t function = 0; function < 8 && device_count < NPK_PCI_MAX_DEVICES; ++function) {
                uint32_t vendor_device = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                            (uint8_t)function, 0x00);
                if ((vendor_device & 0xffffU) == 0xffffU) continue;
                npk_pci_device_t *device = &devices[device_count++];
                uint32_t class_revision = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                             (uint8_t)function, 0x08);
                uint32_t header = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                    (uint8_t)function, 0x0c);
                uint32_t interrupt = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                        (uint8_t)function, 0x3c);
                device->bus = (uint8_t)bus;
                device->slot = (uint8_t)slot;
                device->function = (uint8_t)function;
                device->vendor_id = (uint16_t)(vendor_device & 0xffffU);
                device->device_id = (uint16_t)(vendor_device >> 16);
                device->revision = (uint8_t)(class_revision & 0xffU);
                device->prog_if = (uint8_t)(class_revision >> 8);
                device->subclass = (uint8_t)(class_revision >> 16);
                device->class_code = (uint8_t)(class_revision >> 24);
                device->header_type = (uint8_t)(header >> 16);
                device->irq_line = (uint8_t)(interrupt & 0xffU);
                device->irq_pin = (uint8_t)(interrupt >> 8);
                for (unsigned bar = 0; bar < 6; ++bar)
                    device->bars[bar] = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                           (uint8_t)function, (uint8_t)(0x10 + bar * 4));
                npk_device_identity_t identity = {
                    .bus = NPK_DEVICE_BUS_PCI,
                    .location = ((uint64_t)bus << 16) | ((uint64_t)slot << 8) | function,
                    .vendor_id = device->vendor_id,
                    .product_id = device->device_id,
                    .class_code = device->class_code,
                    .subclass = device->subclass,
                    .protocol = device->prog_if,
                    .revision = device->revision,
                    .interface_number = 0xff
                };
                (void)devmgr_register_device(&identity);
            }
        }
    }
    LOG_INFOF("pci", "enumerated devices", device_count);
}

size_t pci_device_count(void) { return device_count; }

const npk_pci_device_t *pci_device_at(size_t index) {
    return index < device_count ? &devices[index] : NULL;
}
