#include <npk/arch.h>
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
            }
        }
    }
    LOG_INFOF("pci", "enumerated devices", device_count);
}

size_t pci_device_count(void) { return device_count; }

const npk_pci_device_t *pci_device_at(size_t index) {
    return index < device_count ? &devices[index] : NULL;
}
