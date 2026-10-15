#include <npk/arch.h>
#include <npk/device_manager.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/pci.h>
#include <npk/string.h>
#include <npk/usb.h>
#include <npk/usb_generic_keyboard.h>

#define XHCI_TRB_COUNT 16U
#define XHCI_EVENT_COUNT 64U
#define XHCI_MAX_SLOTS 16U
#define XHCI_MAX_PORTS 64U
#define XHCI_DMA_PAGE_LIMIT 96U
#define XHCI_MMIO_LIMIT (16U * 1024U * 1024U)
#define XHCI_POLL_LIMIT 20000000U

#define XHCI_CAPLENGTH 0x00U
#define XHCI_HCIVERSION 0x02U
#define XHCI_HCSPARAMS1 0x04U
#define XHCI_HCCPARAMS1 0x10U
#define XHCI_DBOFF 0x14U
#define XHCI_RTSOFF 0x18U

#define XHCI_USBCMD 0x00U
#define XHCI_USBSTS 0x04U
#define XHCI_PAGESIZE 0x08U
#define XHCI_CRCR 0x18U
#define XHCI_DCBAAP 0x30U
#define XHCI_CONFIG 0x38U
#define XHCI_PORT_BASE 0x400U
#define XHCI_PORT_STRIDE 0x10U

#define XHCI_CMD_RUN (1U << 0)
#define XHCI_CMD_RESET (1U << 1)
#define XHCI_STS_HALTED (1U << 0)
#define XHCI_STS_HOST_SYSTEM_ERROR (1U << 2)
#define XHCI_STS_NOT_READY (1U << 11)
#define XHCI_STS_CONTROLLER_ERROR (1U << 12)

#define XHCI_PORT_CONNECTED (1U << 0)
#define XHCI_PORT_ENABLED (1U << 1)
#define XHCI_PORT_RESET (1U << 4)
#define XHCI_PORT_POWER (1U << 9)
#define XHCI_PORT_SPEED_MASK (0x0fU << 10)
#define XHCI_PORT_CSC (1U << 17)
#define XHCI_PORT_PEC (1U << 18)
#define XHCI_PORT_WRC (1U << 19)
#define XHCI_PORT_OCC (1U << 20)
#define XHCI_PORT_PRC (1U << 21)
#define XHCI_PORT_PLC (1U << 22)
#define XHCI_PORT_CEC (1U << 23)
#define XHCI_PORT_WARM_RESET (1U << 31)
#define XHCI_PORT_CHANGE_MASK 0x00fe0000U

#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_DIR_IN (1U << 16)
#define XHCI_TRB_SETUP_TRT_IN (3U << 16)
#define XHCI_TRB_TYPE_LINK 6U
#define XHCI_TRB_TYPE_ENABLE_SLOT 9U
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11U
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT 12U
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT 13U
#define XHCI_TRB_TYPE_NORMAL 1U
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32U
#define XHCI_TRB_TYPE_COMMAND_EVENT 33U
#define XHCI_TRB_TYPE_PORT_EVENT 34U
#define XHCI_TRB_TYPE_HOST_EVENT 37U

#define XHCI_CC_SUCCESS 1U
#define XHCI_CC_SHORT_PACKET 13U
#define XHCI_ERST_SIZE XHCI_EVENT_COUNT
#define XHCI_EP0_DCI 1U

#define PCI_CLASS_SERIAL_BUS 0x0cU
#define PCI_SUBCLASS_USB 0x03U
#define PCI_PROGIF_XHCI 0x30U

#define XHCI_ECP_LEGACY_SUPPORT 1U
#define XHCI_ECP_SUPPORTED_PROTOCOL 2U
#define XHCI_LEGACY_BIOS_OWNED (1U << 16)
#define XHCI_LEGACY_OS_OWNED (1U << 24)

_Static_assert(sizeof(uint64_t) == 8, "xHCI requires 64-bit pointers");

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "xHCI TRB must be 16 bytes");

typedef struct {
    uint64_t ring_base;
    uint32_t ring_size;
    uint32_t reserved;
} xhci_erst_entry_t;

_Static_assert(sizeof(xhci_erst_entry_t) == 16, "xHCI ERST entry must be 16 bytes");

typedef struct {
    bool active;
    uint8_t slot_id;
    uint8_t root_port;
    uint8_t speed;
    uint16_t ep0_max_packet;
    paddr_t input_context_phys;
    paddr_t output_context_phys;
    paddr_t ep0_ring_phys;
    volatile xhci_trb_t *ep0_ring;
    uint8_t ep0_enqueue_index;
    uint8_t ep0_cycle;
    paddr_t interrupt_ring_phys;
    volatile xhci_trb_t *interrupt_ring;
    uint8_t interrupt_enqueue_index;
    uint8_t interrupt_cycle;
    uint8_t interrupt_endpoint_id;
    uint16_t interrupt_packet_size;
    paddr_t report_buffer_phys;
    uint8_t *report_buffer;
    uint64_t pending_transfer_phys;
    bool interrupt_configured;
    bool interrupt_started;
    bool interrupt_pending;
} xhci_slot_t;

typedef struct {
    bool active;
    bool port_power_control;
    bool context_64;
    uint8_t max_ports;
    uint8_t max_slots_enabled;
    uint16_t hci_version;
    uint32_t bar_command_original;
    bool pci_command_modified;
    bool start_attempted;
    uint32_t cap_length;
    uint32_t operational_offset;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint64_t bar_size;
    volatile uint8_t *mmio;
    volatile xhci_trb_t *command_ring;
    volatile xhci_trb_t *event_ring;
    xhci_erst_entry_t *erst;
    uint8_t command_enqueue_index;
    uint8_t command_cycle;
    uint16_t event_dequeue_index;
    uint8_t event_cycle;
    uint8_t port_protocol[XHCI_MAX_PORTS + 1U];
    xhci_slot_t slots[XHCI_MAX_SLOTS + 1U];
    paddr_t dcbaa_phys;
    uint64_t *dcbaa;
    paddr_t command_ring_phys;
    paddr_t event_ring_phys;
    paddr_t erst_phys;
    paddr_t descriptor_page_phys;
    uint8_t *descriptor_page;
    paddr_t dma_pages[XHCI_DMA_PAGE_LIMIT];
    size_t dma_page_count;
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_function;
} xhci_controller_t;

static xhci_controller_t xhci;
static bool xhci_quarantined;

static const npk_device_id_t xhci_ids[] = {{
    .bus = NPK_DEVICE_BUS_PCI,
    .match_flags = NPK_DEVICE_MATCH_CLASS | NPK_DEVICE_MATCH_SUBCLASS |
                   NPK_DEVICE_MATCH_PROTOCOL,
    .class_code = PCI_CLASS_SERIAL_BUS,
    .subclass = PCI_SUBCLASS_USB,
    .protocol = PCI_PROGIF_XHCI
}};

static int xhci_probe(npk_device_t *device);
static const npk_device_driver_t xhci_driver = {
    .name = "xhci",
    .ids = xhci_ids,
    .id_count = sizeof(xhci_ids) / sizeof(xhci_ids[0]),
    .probe = xhci_probe
};

static volatile uint32_t *reg32(uint32_t offset) {
    if (!xhci.mmio || offset > xhci.bar_size || xhci.bar_size - offset < sizeof(uint32_t))
        return NULL;
    return (volatile uint32_t *)(xhci.mmio + offset);
}

static uint32_t read_reg32(uint32_t offset) {
    volatile uint32_t *reg = reg32(offset);
    if (!reg) return UINT32_MAX;
    uint32_t value = *reg;
    compiler_barrier();
    return value;
}

static bool write_reg32(uint32_t offset, uint32_t value) {
    volatile uint32_t *reg = reg32(offset);
    if (!reg) return false;
    *reg = value;
    compiler_barrier();
    return true;
}

static bool write_reg64_low_high(uint32_t offset, uint64_t value) {
    if (!reg32(offset) || !reg32(offset + 4U)) return false;
    /* CRCR and ERSTBA commit/reinitialize on the high-dword write. */
    if (!write_reg32(offset, (uint32_t)value)) return false;
    return write_reg32(offset + 4U, (uint32_t)(value >> 32));
}

static bool write_reg64_high_low(uint32_t offset, uint64_t value) {
    if (!reg32(offset) || !reg32(offset + 4U)) return false;
    /* ERDP's EHB update is processed by the low-dword write. */
    if (!write_reg32(offset + 4U, (uint32_t)(value >> 32))) return false;
    return write_reg32(offset, (uint32_t)value);
}

static bool wait_reg(uint32_t offset, uint32_t mask, uint32_t expected) {
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; ++i) {
        uint32_t value = read_reg32(offset);
        if (value == UINT32_MAX) return false;
        if ((value & mask) == expected) return true;
        if (offset == xhci.operational_offset + XHCI_USBSTS &&
            (value & (XHCI_STS_HOST_SYSTEM_ERROR | XHCI_STS_CONTROLLER_ERROR)))
            return false;
        __asm__ volatile ("pause");
    }
    return false;
}

static void *alloc_dma_page(paddr_t *physical) {
    if (!physical || xhci.dma_page_count >= XHCI_DMA_PAGE_LIMIT) return NULL;
    paddr_t page = pmm_alloc_page();
    if (!page) return NULL;
    /* Current PMM coverage ends at 4 GiB; retain the check if it is widened. */
    if (page >= 0x100000000ULL) {
        pmm_free_page(page);
        return NULL;
    }
    void *virtual_address = phys_to_virt(page);
    memset(virtual_address, 0, NPK_PAGE_SIZE);
    xhci.dma_pages[xhci.dma_page_count++] = page;
    *physical = page;
    return virtual_address;
}

static void free_dma_pages(void) {
    for (size_t i = 0; i < xhci.dma_page_count; ++i)
        pmm_free_page(xhci.dma_pages[i]);
    xhci.dma_page_count = 0;
}

static void write_link_trb(volatile xhci_trb_t *ring, paddr_t ring_phys,
                           uint8_t cycle) {
    volatile xhci_trb_t *link = &ring[XHCI_TRB_COUNT - 1U];
    link->control = 0;
    link->parameter = ring_phys;
    link->status = 0;
    compiler_barrier();
    link->control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                    XHCI_TRB_TOGGLE_CYCLE | (cycle ? XHCI_TRB_CYCLE : 0U);
}

static bool next_event(xhci_trb_t *event) {
    volatile xhci_trb_t *source = &xhci.event_ring[xhci.event_dequeue_index];
    uint32_t control = source->control;
    if ((control & XHCI_TRB_CYCLE) != xhci.event_cycle) return false;
    compiler_barrier();
    event->parameter = source->parameter;
    event->status = source->status;
    event->control = control;

    ++xhci.event_dequeue_index;
    if (xhci.event_dequeue_index == XHCI_EVENT_COUNT) {
        xhci.event_dequeue_index = 0;
        xhci.event_cycle ^= 1U;
    }
    uint64_t dequeue = xhci.event_ring_phys +
                       (uint64_t)xhci.event_dequeue_index * sizeof(xhci_trb_t) | (1U << 3);
    (void)write_reg64_high_low(xhci.runtime_offset + 0x20U + 0x18U, dequeue);
    return true;
}

static bool wait_event(uint8_t expected_type, uint64_t target_pointer,
                       uint8_t expected_slot, xhci_trb_t *result) {
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; ++i) {
        xhci_trb_t event;
        if (!next_event(&event)) {
            __asm__ volatile ("pause");
            continue;
        }
        uint8_t type = (uint8_t)((event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3fU);
        if (type == XHCI_TRB_TYPE_HOST_EVENT) {
            log_message(LOG_ERROR, "xhci", "host controller event while polling");
            LOG_ERRORF("xhci", "host event completion code", event.status >> 24);
        }
        if (type != expected_type) continue;
        if (target_pointer && event.parameter != target_pointer) continue;
        if (expected_slot && (uint8_t)(event.control >> 24) != expected_slot) continue;
        if (result) *result = event;
        return true;
    }
    log_message(LOG_ERROR, "xhci", "timed out waiting for transfer/command event");
    return false;
}

static void advance_command_ring(void) {
    ++xhci.command_enqueue_index;
    if (xhci.command_enqueue_index == XHCI_TRB_COUNT - 1U) {
        xhci.command_enqueue_index = 0;
        write_link_trb(xhci.command_ring, xhci.command_ring_phys, xhci.command_cycle);
        xhci.command_cycle ^= 1U;
    }
}

static bool issue_command(uint8_t type, uint64_t parameter, uint32_t status,
                          uint32_t extra_control, uint8_t slot_id,
                          uint8_t *completion_slot) {
    volatile xhci_trb_t *command = &xhci.command_ring[xhci.command_enqueue_index];
    uint64_t command_phys = xhci.command_ring_phys +
                            (uint64_t)xhci.command_enqueue_index * sizeof(xhci_trb_t);
    uint32_t control = ((uint32_t)type << XHCI_TRB_TYPE_SHIFT) | extra_control |
                       (xhci.command_cycle ? XHCI_TRB_CYCLE : 0U) |
                       ((uint32_t)slot_id << 24);
    command->control = 0;
    command->parameter = parameter;
    command->status = status;
    compiler_barrier();
    command->control = control;
    compiler_barrier();
    if (!write_reg32(xhci.doorbell_offset, 0)) return false;

    xhci_trb_t event;
    if (!wait_event(XHCI_TRB_TYPE_COMMAND_EVENT, command_phys, 0, &event)) return false;
    advance_command_ring();
    uint8_t completion = (uint8_t)(event.status >> 24);
    if (completion_slot) *completion_slot = (uint8_t)(event.control >> 24);
    if (completion != XHCI_CC_SUCCESS) {
        LOG_ERRORF("xhci", "xHCI command completion code", completion);
        return false;
    }
    return true;
}

static void parse_extended_capabilities(void) {
    uint32_t hccparams = read_reg32(XHCI_HCCPARAMS1);
    uint32_t offset = ((hccparams >> 16) & 0xffffU) * 4U;
    for (unsigned count = 0; offset && count < 128U; ++count) {
        if (offset > xhci.bar_size || xhci.bar_size - offset < 4U) break;
        uint32_t header = read_reg32(offset);
        uint8_t cap_id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);
        if (cap_id == XHCI_ECP_LEGACY_SUPPORT &&
            xhci.bar_size - offset >= 8U) {
            uint32_t legacy = read_reg32(offset + 4U);
            if (legacy & XHCI_LEGACY_BIOS_OWNED) {
                (void)write_reg32(offset + 4U, legacy | XHCI_LEGACY_OS_OWNED);
                bool released = false;
                for (uint32_t wait = 0; wait < XHCI_POLL_LIMIT; ++wait) {
                    if (!(read_reg32(offset + 4U) & XHCI_LEGACY_BIOS_OWNED)) {
                        released = true;
                        break;
                    }
                    __asm__ volatile ("pause");
                }
                if (!released) log_message(LOG_WARN, "xhci", "BIOS ownership handoff timed out");
            }
        } else if (cap_id == XHCI_ECP_SUPPORTED_PROTOCOL &&
                   xhci.bar_size - offset >= 12U) {
            uint8_t major = (uint8_t)(header >> 24);
            uint32_t ports = read_reg32(offset + 8U);
            uint8_t first_port = (uint8_t)ports;
            uint8_t port_count = (uint8_t)(ports >> 8);
            if (major && first_port && port_count &&
                (uint16_t)first_port + port_count <= (uint16_t)xhci.max_ports + 1U) {
                for (uint16_t p = first_port; p < (uint16_t)first_port + port_count; ++p)
                    xhci.port_protocol[p] = major;
                LOG_INFOF("xhci", "supported protocol major version", major);
                LOG_INFOF("xhci", "supported protocol first port/count",
                          ((uint64_t)first_port << 8) | port_count);
            }
        }
        if (!next) break;
        uint32_t advance = (uint32_t)next * 4U;
        if (advance > xhci.bar_size - offset) break;
        offset += advance;
    }
    for (uint8_t port = 1; port <= xhci.max_ports; ++port)
        if (!xhci.port_protocol[port]) xhci.port_protocol[port] = 2U;
}

static bool allocate_controller_dma(void) {
    xhci.dcbaa = alloc_dma_page(&xhci.dcbaa_phys);
    xhci.command_ring = alloc_dma_page(&xhci.command_ring_phys);
    xhci.event_ring = alloc_dma_page(&xhci.event_ring_phys);
    xhci.erst = alloc_dma_page(&xhci.erst_phys);
    xhci.descriptor_page = alloc_dma_page(&xhci.descriptor_page_phys);
    if (!xhci.dcbaa || !xhci.command_ring || !xhci.event_ring || !xhci.erst ||
        !xhci.descriptor_page)
        return false;

    xhci.command_cycle = 1;
    xhci.event_cycle = 1;
    xhci.command_enqueue_index = 0;
    xhci.event_dequeue_index = 0;
    write_link_trb(xhci.command_ring, xhci.command_ring_phys, xhci.command_cycle);
    xhci.erst[0].ring_base = xhci.event_ring_phys;
    xhci.erst[0].ring_size = XHCI_EVENT_COUNT;
    xhci.erst[0].reserved = 0;
    return true;
}

static bool configure_rings(void) {
    uint32_t interrupter = xhci.runtime_offset + 0x20U;
    if (!write_reg32(interrupter + 0x00U, 0) ||
        !write_reg32(interrupter + 0x04U, 0) ||
        !write_reg32(interrupter + 0x08U, 1) ||
        !write_reg64_low_high(interrupter + 0x10U, xhci.erst_phys) ||
        !write_reg64_high_low(interrupter + 0x18U, xhci.event_ring_phys))
        return false;
    if (!write_reg64_low_high(xhci.operational_offset + XHCI_DCBAAP, xhci.dcbaa_phys) ||
        !write_reg64_low_high(xhci.operational_offset + XHCI_CRCR,
                              xhci.command_ring_phys | 1U) ||
        !write_reg32(xhci.operational_offset + XHCI_CONFIG, xhci.max_slots_enabled))
        return false;
    return true;
}

static bool stop_and_reset_controller(void) {
    uint32_t op = xhci.operational_offset;
    uint32_t command = read_reg32(op + XHCI_USBCMD);
    if (command == UINT32_MAX) return false;
    if (!write_reg32(op + XHCI_USBCMD, command & ~XHCI_CMD_RUN) ||
        !wait_reg(op + XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED)) {
        log_message(LOG_ERROR, "xhci", "controller did not halt");
        return false;
    }
    command = read_reg32(op + XHCI_USBCMD);
    if (!write_reg32(op + XHCI_USBCMD, command | XHCI_CMD_RESET) ||
        !wait_reg(op + XHCI_USBCMD, XHCI_CMD_RESET, 0) ||
        !wait_reg(op + XHCI_USBSTS, XHCI_STS_NOT_READY, 0)) {
        log_message(LOG_ERROR, "xhci", "controller reset or CNR wait failed");
        return false;
    }
    if (!(read_reg32(op + XHCI_PAGESIZE) & 1U)) {
        log_message(LOG_ERROR, "xhci", "controller does not support 4K DMA pages");
        return false;
    }
    return true;
}

static bool start_controller(void) {
    uint32_t cmd_offset = xhci.operational_offset + XHCI_USBCMD;
    xhci.start_attempted = true;
    if (!write_reg32(cmd_offset, XHCI_CMD_RUN) ||
        !wait_reg(xhci.operational_offset + XHCI_USBSTS, XHCI_STS_HALTED, 0)) {
        log_message(LOG_ERROR, "xhci", "controller failed to enter Running state");
        return false;
    }
    uint32_t status = read_reg32(xhci.operational_offset + XHCI_USBSTS);
    if (status & (XHCI_STS_HOST_SYSTEM_ERROR | XHCI_STS_CONTROLLER_ERROR)) {
        LOG_ERRORF("xhci", "controller status after start", status);
        return false;
    }
    xhci.active = true;
    log_message(LOG_INFO, "xhci", "controller running with polling event ring");
    return true;
}

static bool reset_port(uint8_t port, uint32_t port_offset) {
    uint32_t status = read_reg32(port_offset);
    if (!(status & XHCI_PORT_CONNECTED)) return false;
    if (xhci.port_power_control && !(status & XHCI_PORT_POWER)) {
        uint32_t power_write = (status & XHCI_PORT_CHANGE_MASK) | XHCI_PORT_POWER;
        if (!write_reg32(port_offset, power_write)) return false;
        for (uint32_t wait = 0; wait < XHCI_POLL_LIMIT / 8U; ++wait) {
            status = read_reg32(port_offset);
            if (status & XHCI_PORT_POWER) break;
            __asm__ volatile ("pause");
        }
    }
    status = read_reg32(port_offset);
    if (status & XHCI_PORT_ENABLED) return true;

    bool superspeed = xhci.port_protocol[port] >= 3U;
    uint32_t reset_bit = superspeed ? XHCI_PORT_WARM_RESET : XHCI_PORT_RESET;
    uint32_t reset_write = (status & XHCI_PORT_POWER) |
                           (status & XHCI_PORT_CHANGE_MASK) | reset_bit;
    if (!write_reg32(port_offset, reset_write)) return false;

    bool completed = false;
    for (uint32_t wait = 0; wait < XHCI_POLL_LIMIT; ++wait) {
        status = read_reg32(port_offset);
        if (!(status & XHCI_PORT_CONNECTED)) return false;
        uint32_t change = superspeed ? XHCI_PORT_WRC : XHCI_PORT_PRC;
        if ((status & XHCI_PORT_ENABLED) && !(status & reset_bit) && (status & change)) {
            completed = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    status = read_reg32(port_offset);
    if (completed) {
        (void)write_reg32(port_offset, (status & XHCI_PORT_POWER) |
                                      (status & XHCI_PORT_CHANGE_MASK));
        LOG_INFOF("xhci", "root port reset complete", port);
        return true;
    }
    LOG_WARNF("xhci", "root port reset timed out", port);
    return (status & XHCI_PORT_ENABLED) != 0;
}

static bool create_slot_context(uint8_t slot_id, uint8_t port, uint8_t speed) {
    if (!slot_id || slot_id > xhci.max_slots_enabled || slot_id > XHCI_MAX_SLOTS)
        return false;
    xhci_slot_t *slot = &xhci.slots[slot_id];
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = slot_id;
    slot->root_port = port;
    slot->speed = speed;
    slot->ep0_max_packet = (speed == 3U) ? 64U : (speed == 4U ? 512U : 8U);

    uint8_t *input = alloc_dma_page(&slot->input_context_phys);
    uint8_t *output = alloc_dma_page(&slot->output_context_phys);
    slot->ep0_ring = alloc_dma_page(&slot->ep0_ring_phys);
    if (!input || !output || !slot->ep0_ring) return false;
    xhci.dcbaa[slot_id] = slot->output_context_phys;

    uint32_t context_size = xhci.context_64 ? 64U : 32U;
    uint32_t *control = (uint32_t *)input;
    control[1] = (1U << 0) | (1U << 1); /* Add Slot and Default Control EP contexts. */

    uint32_t *slot_context = (uint32_t *)(input + context_size);
    slot_context[0] = ((uint32_t)speed << 20) | (1U << 27); /* Route=0; Context Entries=1. */
    slot_context[1] = (uint32_t)port << 16;                 /* Root Hub Port Number. */

    uint32_t *ep0_context = (uint32_t *)(input + 2U * context_size);
    ep0_context[1] = (3U << 1) | (4U << 3) | ((uint32_t)slot->ep0_max_packet << 16);
    ep0_context[2] = (uint32_t)slot->ep0_ring_phys | 1U;    /* TR Dequeue Pointer + DCS. */
    ep0_context[3] = (uint32_t)(slot->ep0_ring_phys >> 32);
    ep0_context[4] = 8U;                                   /* Average TRB Length. */

    slot->ep0_cycle = 1;
    slot->ep0_enqueue_index = 0;
    write_link_trb(slot->ep0_ring, slot->ep0_ring_phys, slot->ep0_cycle);

    uint8_t returned_slot = 0;
    if (!issue_command(XHCI_TRB_TYPE_ADDRESS_DEVICE, slot->input_context_phys, 0,
                       0, slot_id, &returned_slot) || returned_slot != slot_id) {
        LOG_ERRORF("xhci", "Address Device failed for slot", slot_id);
        return false;
    }
    slot->active = true;
    LOG_INFOF("xhci", "USB device addressed in slot", slot_id);
    return true;
}

static bool enable_and_address_device(uint8_t port, uint8_t speed, uint8_t *slot_id_out) {
    uint8_t slot_id = 0;
    if (!issue_command(XHCI_TRB_TYPE_ENABLE_SLOT, 0, 0, 0, 0, &slot_id) ||
        !slot_id || slot_id > xhci.max_slots_enabled || slot_id > XHCI_MAX_SLOTS) {
        log_message(LOG_ERROR, "xhci", "Enable Slot command failed or slot id is invalid");
        return false;
    }
    LOG_INFOF("xhci", "Enable Slot returned id", slot_id);
    if (!create_slot_context(slot_id, port, speed)) {
        LOG_ERRORF("xhci", "slot context initialization failed", slot_id);
        return false;
    }
    *slot_id_out = slot_id;
    return true;
}

static bool initialize_controller_registers(void) {
    uint32_t cap0 = read_reg32(0);
    xhci.cap_length = cap0 & 0xffU;
    xhci.hci_version = (uint16_t)(cap0 >> 16);
    uint32_t hcs1 = read_reg32(XHCI_HCSPARAMS1);
    uint32_t hcc1 = read_reg32(XHCI_HCCPARAMS1);
    uint32_t db_off = read_reg32(XHCI_DBOFF);
    uint32_t rt_off = read_reg32(XHCI_RTSOFF);
    uint8_t hardware_slots = (uint8_t)hcs1;
    xhci.max_ports = (uint8_t)(hcs1 >> 24);
    xhci.port_power_control = ((hcs1 >> 3) & 1U) != 0;
    xhci.context_64 = ((hcc1 >> 2) & 1U) != 0;

    if (xhci.cap_length < 0x20U || xhci.hci_version < 0x0100U ||
        !hardware_slots || !xhci.max_ports || xhci.max_ports > XHCI_MAX_PORTS) {
        log_message(LOG_ERROR, "xhci", "unsupported or invalid capability registers");
        return false;
    }
    xhci.max_slots_enabled = hardware_slots < XHCI_MAX_SLOTS ? hardware_slots : XHCI_MAX_SLOTS;
    xhci.operational_offset = xhci.cap_length;
    xhci.doorbell_offset = db_off & ~3U;
    xhci.runtime_offset = rt_off & ~0x1fU;

    uint64_t port_end = (uint64_t)xhci.operational_offset + XHCI_PORT_BASE +
                        (uint64_t)xhci.max_ports * XHCI_PORT_STRIDE;
    uint64_t doorbell_end = (uint64_t)xhci.doorbell_offset +
                            ((uint64_t)xhci.max_slots_enabled + 1U) * 4U;
    uint64_t runtime_end = (uint64_t)xhci.runtime_offset + 0x20U + 0x20U;
    if (port_end > xhci.bar_size || doorbell_end > xhci.bar_size ||
        runtime_end > xhci.bar_size) {
        log_message(LOG_ERROR, "xhci", "BAR/register window bounds are invalid");
        return false;
    }
    LOG_INFOF("xhci", "xHCI 64-bit DMA capability", hcc1 & 1U);
    LOG_INFOF("xhci", "xHCI interface version", xhci.hci_version);
    LOG_INFOF("xhci", "xHCI root ports", xhci.max_ports);
    LOG_INFOF("xhci", "xHCI enabled slots", xhci.max_slots_enabled);
    LOG_INFOF("xhci", "xHCI context size", xhci.context_64 ? 64U : 32U);
    parse_extended_capabilities();
    return stop_and_reset_controller();
}

static bool enumerate_connected_ports(void) {
    for (uint8_t port = 1; port <= xhci.max_ports; ++port) {
        uint32_t port_offset = xhci.operational_offset + XHCI_PORT_BASE +
                               ((uint32_t)port - 1U) * XHCI_PORT_STRIDE;
        uint32_t status = read_reg32(port_offset);
        if (status == UINT32_MAX || !(status & XHCI_PORT_CONNECTED)) continue;
        LOG_INFOF("xhci", "connected root port", port);
        if (!(status & XHCI_PORT_ENABLED) && !reset_port(port, port_offset)) continue;
        status = read_reg32(port_offset);
        if (!(status & XHCI_PORT_CONNECTED) || !(status & XHCI_PORT_ENABLED)) {
            LOG_WARNF("xhci", "root port did not become enabled", port);
            continue;
        }
        uint8_t speed = (uint8_t)((status & XHCI_PORT_SPEED_MASK) >> 10);
        if (speed < 1U || speed > 4U) {
            LOG_WARNF("xhci", "unsupported USB speed code", speed);
            continue;
        }
        uint8_t slot_id = 0;
        if (!enable_and_address_device(port, speed, &slot_id)) continue;
        usb_identify_xhci_device(slot_id, port, speed);
    }
    return true;
}

static bool start_xhci_for_pci_device(const npk_pci_device_t *pci_device) {
    npk_pci_bar_info_t bar;
    if (!pci_get_bar_info(pci_device, 0, &bar) || bar.is_io || !bar.base ||
        bar.size < 0x1000U || bar.size > XHCI_MMIO_LIMIT) {
        log_message(LOG_ERROR, "xhci", "BAR0 is absent or outside supported MMIO bounds");
        return false;
    }
    xhci.pci_bus = pci_device->bus;
    xhci.pci_slot = pci_device->slot;
    xhci.pci_function = pci_device->function;
    xhci.bar_size = bar.size;

    uint32_t command_status = pci_config_read32(pci_device->bus, pci_device->slot,
                                                pci_device->function, 0x04);
    xhci.bar_command_original = command_status & 0xffffU;
    pci_config_write32(pci_device->bus, pci_device->slot, pci_device->function,
                       0x04, (xhci.bar_command_original | (1U << 1) | (1U << 2)));
    xhci.pci_command_modified = true;
    xhci.mmio = (volatile uint8_t *)vmm_map_mmio(bar.base, (size_t)bar.size);
    if (!xhci.mmio) {
        log_message(LOG_ERROR, "xhci", "could not map controller BAR0 as uncached MMIO");
        return false;
    }
    LOG_INFOF("xhci", "controller BAR0 base", bar.base);
    LOG_INFOF("xhci", "controller BAR0 size", bar.size);
    if (!initialize_controller_registers() || !allocate_controller_dma() ||
        !configure_rings() || !start_controller())
        return false;
    (void)enumerate_connected_ports();
    return true;
}

static bool stop_after_failed_start(void) {
    if (!xhci.start_attempted) return true;
    uint32_t command_offset = xhci.operational_offset + XHCI_USBCMD;
    uint32_t status_offset = xhci.operational_offset + XHCI_USBSTS;
    uint32_t command = read_reg32(command_offset);
    if (command == UINT32_MAX || !write_reg32(command_offset, command & ~XHCI_CMD_RUN))
        return false;
    return wait_reg(status_offset, XHCI_STS_HALTED, XHCI_STS_HALTED);
}

static const npk_pci_device_t *find_pci_function(uint8_t bus, uint8_t slot, uint8_t function) {
    for (size_t i = 0; i < pci_device_count(); ++i) {
        const npk_pci_device_t *device = pci_device_at(i);
        if (device && device->bus == bus && device->slot == slot &&
            device->function == function)
            return device;
    }
    return NULL;
}

static int xhci_probe(npk_device_t *device) {
    if (!device || device->identity.bus != NPK_DEVICE_BUS_PCI) return -22;
    if (xhci.active || xhci_quarantined) {
        log_message(LOG_WARN, "xhci", "only one controller is supported in this build");
        return -16;
    }
    uint8_t bus = (uint8_t)(device->identity.location >> 16);
    uint8_t slot = (uint8_t)(device->identity.location >> 8);
    uint8_t function = (uint8_t)device->identity.location;
    const npk_pci_device_t *pci_device = find_pci_function(bus, slot, function);
    if (!pci_device) return -19;

    memset(&xhci, 0, sizeof(xhci));
    if (!start_xhci_for_pci_device(pci_device)) {
        if (!stop_after_failed_start()) {
            /* Keep DMA pages and PCI bus mastering pinned if stop was not
             * observed; releasing memory still reachable by the controller
             * would permit DMA into reallocated kernel pages. */
            xhci_quarantined = true;
            log_message(LOG_ERROR, "xhci", "controller stop failed; DMA pages quarantined");
            return -5;
        }
        free_dma_pages();
        if (xhci.pci_command_modified && xhci.pci_bus == bus &&
            xhci.pci_slot == slot && xhci.pci_function == function)
            pci_config_write32(bus, slot, function, 0x04, xhci.bar_command_original);
        memset(&xhci, 0, sizeof(xhci));
        return -5;
    }
    return 0;
}

int xhci_register_driver(void) {
    return devmgr_register_driver(&xhci_driver);
}

static bool enqueue_transfer_trb(xhci_slot_t *slot, uint64_t parameter,
                                 uint32_t status, uint32_t control,
                                 uint64_t *physical_address) {
    if (!slot || !slot->ep0_ring || slot->ep0_enqueue_index >= XHCI_TRB_COUNT - 1U)
        return false;
    uint8_t index = slot->ep0_enqueue_index;
    volatile xhci_trb_t *trb = &slot->ep0_ring[index];
    if (physical_address)
        *physical_address = slot->ep0_ring_phys + (uint64_t)index * sizeof(xhci_trb_t);
    trb->control = 0;
    trb->parameter = parameter;
    trb->status = status;
    compiler_barrier();
    trb->control = control | (slot->ep0_cycle ? XHCI_TRB_CYCLE : 0U);
    ++slot->ep0_enqueue_index;
    if (slot->ep0_enqueue_index == XHCI_TRB_COUNT - 1U) {
        slot->ep0_enqueue_index = 0;
        write_link_trb(slot->ep0_ring, slot->ep0_ring_phys, slot->ep0_cycle);
        slot->ep0_cycle ^= 1U;
    }
    return true;
}

bool xhci_control_request(uint8_t slot_id, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, uint8_t *buffer,
                          uint16_t length, size_t capacity, uint16_t *actual_length) {
    if (actual_length) *actual_length = 0;
    if (!xhci.active || !slot_id || slot_id > XHCI_MAX_SLOTS || length > NPK_PAGE_SIZE ||
        (length && (!buffer || capacity < length))) return false;
    xhci_slot_t *slot = &xhci.slots[slot_id];
    if (!slot->active || !xhci.descriptor_page) return false;

    bool direction_in = (request_type & 0x80U) != 0;
    memset(xhci.descriptor_page, 0, NPK_PAGE_SIZE);
    if (length && !direction_in) memcpy(xhci.descriptor_page, buffer, length);

    /* USB SETUP packet fields occupy the low-to-high bytes in wire order. */
    uint64_t setup = (uint64_t)request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    uint32_t transfer_type = length ? (direction_in ? (3U << 16) : (2U << 16)) : 0U;
    uint32_t setup_control = (2U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
                             XHCI_TRB_CHAIN | transfer_type;
    uint32_t status_control = (4U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    if (!length || direction_in) status_control |= XHCI_TRB_DIR_IN;
    uint64_t status_trb_phys = 0;
    if (!enqueue_transfer_trb(slot, setup, 8U, setup_control, NULL)) return false;
    if (length) {
        uint32_t data_control = (3U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_CHAIN;
        if (direction_in) data_control |= XHCI_TRB_DIR_IN;
        if (!enqueue_transfer_trb(slot, xhci.descriptor_page_phys, length,
                                  data_control, NULL)) return false;
    }
    if (!enqueue_transfer_trb(slot, 0, 0, status_control, &status_trb_phys)) return false;

    compiler_barrier();
    uint32_t doorbell = xhci.doorbell_offset + (uint32_t)slot_id * 4U;
    if (!write_reg32(doorbell, XHCI_EP0_DCI)) return false;
    xhci_trb_t event;
    if (!wait_event(XHCI_TRB_TYPE_TRANSFER_EVENT, status_trb_phys, slot_id, &event))
        return false;
    uint8_t completion = (uint8_t)(event.status >> 24);
    uint32_t residual = event.status & 0x00ffffffU;
    uint8_t endpoint_id = (uint8_t)((event.control >> 16) & 0x1fU);
    if ((completion != XHCI_CC_SUCCESS && completion != XHCI_CC_SHORT_PACKET) ||
        endpoint_id != XHCI_EP0_DCI || residual > length) {
        LOG_WARNF("xhci", "control transfer completion code", completion);
        LOG_WARNF("xhci", "control transfer residual bytes", residual);
        return false;
    }
    uint16_t transferred = (uint16_t)(length - residual);
    if (length && direction_in && transferred) memcpy(buffer, xhci.descriptor_page, transferred);
    if (actual_length) *actual_length = transferred;
    return true;
}

bool xhci_get_descriptor(uint8_t slot_id, uint8_t descriptor_type,
                         uint8_t descriptor_index, uint16_t length,
                         uint8_t *buffer, size_t capacity) {
    if (!descriptor_type || !length) return false;
    uint16_t value = ((uint16_t)descriptor_type << 8) | descriptor_index;
    uint16_t actual = 0;
    return xhci_control_request(slot_id, 0x80U, 6U, value, 0U,
                                buffer, length, capacity, &actual) && actual == length;
}

bool xhci_get_device_descriptor(uint8_t slot_id, uint16_t length,
                                uint8_t *buffer, size_t capacity) {
    return xhci_get_descriptor(slot_id, 1U, 0U, length, buffer, capacity);
}

bool xhci_update_ep0_max_packet(uint8_t slot_id, uint16_t max_packet_size) {
    if (!xhci.active || !slot_id || slot_id > XHCI_MAX_SLOTS || !max_packet_size)
        return false;
    xhci_slot_t *slot = &xhci.slots[slot_id];
    if (!slot->active) return false;
    if (slot->ep0_max_packet == max_packet_size) return true;

    uint8_t *input = alloc_dma_page(&slot->input_context_phys);
    if (!input) return false;
    uint32_t context_size = xhci.context_64 ? 64U : 32U;
    uint32_t *control = (uint32_t *)input;
    control[1] = 1U << 1; /* Add Context A1: Default Control Endpoint. */
    uint32_t *ep0 = (uint32_t *)(input + 2U * context_size);
    ep0[1] = (3U << 1) | (4U << 3) | ((uint32_t)max_packet_size << 16);
    ep0[2] = (uint32_t)slot->ep0_ring_phys | 1U;
    ep0[3] = (uint32_t)(slot->ep0_ring_phys >> 32);
    ep0[4] = 8U;

    if (!issue_command(XHCI_TRB_TYPE_EVALUATE_CONTEXT, slot->input_context_phys,
                       0, 0, slot_id, NULL)) {
        LOG_ERRORF("xhci", "Evaluate Context failed for slot", slot_id);
        return false;
    }
    slot->ep0_max_packet = max_packet_size;
    LOG_INFOF("xhci", "EP0 maximum packet size updated", max_packet_size);
    return true;
}

static bool interrupt_interval_context(uint8_t speed, uint8_t interval,
                                       uint8_t *context_interval) {
    if (!context_interval || !interval || speed > 3U || speed < 1U) return false;
    if (speed == 3U) {
        if (interval > 16U) return false;
        *context_interval = (uint8_t)(interval - 1U);
        return true;
    }
    uint32_t microframes = (uint32_t)interval * 8U;
    uint8_t exponent = 0;
    while (microframes > 1U) {
        microframes >>= 1;
        ++exponent;
    }
    if (exponent < 3U) exponent = 3U;
    if (exponent > 10U) exponent = 10U;
    *context_interval = exponent;
    return true;
}

bool xhci_configure_interrupt_in(uint8_t slot_id, uint8_t speed,
                                 uint8_t endpoint_address, uint16_t max_packet_size,
                                 uint8_t interval) {
    if (!xhci.active || !slot_id || slot_id > XHCI_MAX_SLOTS ||
        !(endpoint_address & 0x80U) || (endpoint_address & 0x0fU) == 0U ||
        !max_packet_size || max_packet_size > 1024U) return false;
    xhci_slot_t *slot = &xhci.slots[slot_id];
    if (!slot->active || slot->interrupt_configured || speed != slot->speed) return false;
    uint8_t interval_field;
    if (!interrupt_interval_context(speed, interval, &interval_field)) {
        log_message(LOG_WARN, "xhci", "interrupt endpoint speed/interval is unsupported");
        return false;
    }
    uint8_t endpoint_number = endpoint_address & 0x0fU;
    uint8_t endpoint_id = (uint8_t)(endpoint_number * 2U + 1U);
    if (endpoint_id < 2U || endpoint_id > 31U) return false;

    uint8_t *input = phys_to_virt(slot->input_context_phys);
    uint8_t *output = phys_to_virt(slot->output_context_phys);
    slot->interrupt_ring = alloc_dma_page(&slot->interrupt_ring_phys);
    slot->report_buffer = alloc_dma_page(&slot->report_buffer_phys);
    if (!input || !output || !slot->interrupt_ring || !slot->report_buffer) return false;

    uint32_t context_size = xhci.context_64 ? 64U : 32U;
    memset(input, 0, NPK_PAGE_SIZE);
    uint32_t *control = (uint32_t *)input;
    control[1] = (1U << 0) | (1U << endpoint_id); /* Add Slot and selected endpoint. */

    uint32_t *input_slot = (uint32_t *)(input + context_size);
    uint32_t *output_slot = (uint32_t *)(output + context_size);
    memcpy(input_slot, output_slot, context_size);
    input_slot[0] = (input_slot[0] & ~(0x1fU << 27)) |
                    ((uint32_t)endpoint_id << 27);

    uint32_t *endpoint = (uint32_t *)(input + (size_t)(endpoint_id + 1U) * context_size);
    endpoint[0] = (uint32_t)interval_field << 16;
    endpoint[1] = (3U << 1) | (7U << 3) | ((uint32_t)max_packet_size << 16);
    endpoint[2] = (uint32_t)slot->interrupt_ring_phys | 1U;
    endpoint[3] = (uint32_t)(slot->interrupt_ring_phys >> 32);
    endpoint[4] = 8U | ((uint32_t)max_packet_size << 16);
    slot->interrupt_endpoint_id = endpoint_id;
    slot->interrupt_packet_size = max_packet_size;
    slot->interrupt_cycle = 1U;
    slot->interrupt_enqueue_index = 0U;
    write_link_trb(slot->interrupt_ring, slot->interrupt_ring_phys,
                   slot->interrupt_cycle);

    uint8_t completed_slot = 0;
    if (!issue_command(XHCI_TRB_TYPE_CONFIGURE_ENDPOINT, slot->input_context_phys,
                       0U, 0U, slot_id, &completed_slot) || completed_slot != slot_id) {
        log_message(LOG_WARN, "xhci", "Configure Endpoint command failed for HID keyboard");
        return false;
    }
    slot->interrupt_configured = true;
    LOG_INFOF("xhci", "configured USB interrupt-IN endpoint DCI", endpoint_id);
    LOG_INFOF("xhci", "USB interrupt-IN endpoint max packet", max_packet_size);
    LOG_INFOF("xhci", "USB interrupt-IN endpoint interval field", interval_field);
    return true;
}

static bool enqueue_keyboard_report(xhci_slot_t *slot) {
    if (!slot || !slot->interrupt_started || slot->interrupt_pending ||
        !slot->interrupt_ring || !slot->report_buffer ||
        slot->interrupt_enqueue_index >= XHCI_TRB_COUNT - 1U) return false;
    uint8_t index = slot->interrupt_enqueue_index;
    volatile xhci_trb_t *trb = &slot->interrupt_ring[index];
    slot->pending_transfer_phys = slot->interrupt_ring_phys +
                                  (uint64_t)index * sizeof(xhci_trb_t);
    trb->control = 0;
    trb->parameter = slot->report_buffer_phys;
    trb->status = 8U;
    compiler_barrier();
    trb->control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                   (slot->interrupt_cycle ? XHCI_TRB_CYCLE : 0U);
    ++slot->interrupt_enqueue_index;
    if (slot->interrupt_enqueue_index == XHCI_TRB_COUNT - 1U) {
        slot->interrupt_enqueue_index = 0U;
        write_link_trb(slot->interrupt_ring, slot->interrupt_ring_phys,
                       slot->interrupt_cycle);
        slot->interrupt_cycle ^= 1U;
    }
    compiler_barrier();
    uint32_t doorbell = xhci.doorbell_offset + (uint32_t)slot->slot_id * 4U;
    if (!write_reg32(doorbell, slot->interrupt_endpoint_id)) return false;
    slot->interrupt_pending = true;
    return true;
}

bool xhci_start_interrupt_in(uint8_t slot_id) {
    if (!xhci.active || !slot_id || slot_id > XHCI_MAX_SLOTS) return false;
    xhci_slot_t *slot = &xhci.slots[slot_id];
    if (!slot->active || !slot->interrupt_configured || slot->interrupt_started) return false;
    slot->interrupt_started = true;
    if (!enqueue_keyboard_report(slot)) {
        slot->interrupt_started = false;
        return false;
    }
    return true;
}

void xhci_poll_keyboard_inputs(void) {
    if (!xhci.active) return;
    xhci_trb_t event;
    while (next_event(&event)) {
        uint8_t event_type = (uint8_t)((event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3fU);
        if (event_type == XHCI_TRB_TYPE_HOST_EVENT) {
            log_message(LOG_ERROR, "xhci", "host controller event while polling HID input");
            continue;
        }
        if (event_type != XHCI_TRB_TYPE_TRANSFER_EVENT) continue;
        uint8_t slot_id = (uint8_t)(event.control >> 24);
        uint8_t endpoint_id = (uint8_t)((event.control >> 16) & 0x1fU);
        if (!slot_id || slot_id > XHCI_MAX_SLOTS) continue;
        xhci_slot_t *slot = &xhci.slots[slot_id];
        if (!slot->interrupt_pending || endpoint_id != slot->interrupt_endpoint_id ||
            event.parameter != slot->pending_transfer_phys) continue;

        slot->interrupt_pending = false;
        uint8_t completion = (uint8_t)(event.status >> 24);
        uint32_t residual = event.status & 0x00ffffffU;
        if ((completion == XHCI_CC_SUCCESS || completion == XHCI_CC_SHORT_PACKET) &&
            residual <= 8U) {
            size_t actual = 8U - residual;
            if (actual == 8U)
                usb_generic_keyboard_receive_report(slot_id, slot->report_buffer, actual);
            else
                LOG_WARNF("xhci", "short HID boot report bytes", actual);
        } else {
            LOG_ERRORF("xhci", "HID interrupt transfer completion code", completion);
            slot->interrupt_started = false;
            continue;
        }
        if (slot->interrupt_started && !enqueue_keyboard_report(slot)) {
            slot->interrupt_started = false;
            LOG_ERRORF("xhci", "HID report transfer could not be requeued", slot_id);
        }
    }
}

static int xhci_probe(npk_device_t *device);
