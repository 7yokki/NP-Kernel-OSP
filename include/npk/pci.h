#ifndef NPK_PCI_H
#define NPK_PCI_H

#include "types.h"

#define NPK_PCI_MAX_DEVICES 64U

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t revision;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint32_t bars[6];
} npk_pci_device_t;

void pci_init(void);
size_t pci_device_count(void);
const npk_pci_device_t *pci_device_at(size_t index);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);

#endif
