#include <npk/arch.h>
#include <npk/block.h>
#include <npk/log.h>
#include <npk/string.h>

#define ATA_DATA 0x1f0
#define ATA_ERROR 0x1f1
#define ATA_SECTOR_COUNT 0x1f2
#define ATA_LBA_LOW 0x1f3
#define ATA_LBA_MID 0x1f4
#define ATA_LBA_HIGH 0x1f5
#define ATA_DRIVE 0x1f6
#define ATA_STATUS 0x1f7
#define ATA_COMMAND 0x1f7
#define ATA_ALT_STATUS 0x3f6
#define ATA_CONTROL 0x3f6

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_CACHE_FLUSH 0xe7
#define ATA_CMD_IDENTIFY 0xec
#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF 0x20
#define ATA_SR_BSY 0x80
#define ATA_LBA28_MAX 0x10000000ULL
#define ATA_TIMEOUT 1000000U

static ata_device_info_t primary;

static void io_wait(void) { (void)inb(0x80); (void)inb(0x80); (void)inb(0x80); (void)inb(0x80); }
static uint8_t status(void) { return inb(ATA_STATUS); }

static int wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t value = status();
        if (!(value & ATA_SR_BSY)) return (value & (ATA_SR_ERR | ATA_SR_DF)) ? -5 : 0;
    }
    return -110;
}

static int wait_drq(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t value = status();
        if (value & ATA_SR_ERR || value & ATA_SR_DF) return -5;
        if (!(value & ATA_SR_BSY) && (value & ATA_SR_DRQ)) return 0;
    }
    return -110;
}

static int identify(void) {
    outb(ATA_DRIVE, 0xa0);
    io_wait();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();
    if (status() == 0) return -19;
    if (wait_not_busy() != 0) return -5;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return -19;
    if (wait_drq() != 0) return -5;
    uint16_t identify_data[256];
    for (unsigned i = 0; i < 256; ++i) identify_data[i] = inw(ATA_DATA);
    uint64_t sectors = (uint64_t)identify_data[60] | ((uint64_t)identify_data[61] << 16);
    if ((identify_data[83] & (1U << 10)) != 0) {
        uint32_t high = ((uint32_t)identify_data[103] << 16) | identify_data[102];
        sectors = ((uint64_t)high << 32) | ((uint32_t)identify_data[101] << 16) | identify_data[100];
    }
    primary.present = true;
    primary.writable = true;
    primary.sectors = sectors;
    return 0;
}

void ata_init(void) {
    memset(&primary, 0, sizeof(primary));
    int result = identify();
    if (result == 0) LOG_INFOF("ata", "primary sectors", primary.sectors);
    else LOG_INFOF("ata", "primary device unavailable", (uint64_t)(uint32_t)(-result));
}

const ata_device_info_t *ata_primary_info(void) { return &primary; }

static int check_request(uint64_t lba, uint32_t count, const void *buffer) {
    if (!primary.present || count == 0 || count > 255 || buffer == NULL ||
        lba >= ATA_LBA28_MAX || (uint64_t)count > ATA_LBA28_MAX - lba ||
        lba + count > primary.sectors) return -22;
    if (((uintptr_t)buffer & 1U) != 0) return -22;
    return 0;
}

int ata_read_sectors(uint64_t lba, uint32_t count, void *buffer) {
    int check = check_request(lba, count, buffer);
    if (check != 0) return check;
    uint16_t *destination = (uint16_t *)buffer;
    for (uint32_t sector = 0; sector < count; ++sector) {
        if (wait_not_busy() != 0) return -5;
        uint32_t current = (uint32_t)(lba + sector);
        outb(ATA_DRIVE, 0xe0 | ((current >> 24) & 0x0f));
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW, current & 0xff);
        outb(ATA_LBA_MID, (current >> 8) & 0xff);
        outb(ATA_LBA_HIGH, (current >> 16) & 0xff);
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);
        if (wait_drq() != 0) return -5;
        for (unsigned word = 0; word < 256; ++word) destination[sector * 256 + word] = inw(ATA_DATA);
    }
    return 0;
}

int ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer) {
    int check = check_request(lba, count, buffer);
    if (check != 0 || !primary.writable) return check != 0 ? check : -30;
    const uint16_t *source = (const uint16_t *)buffer;
    for (uint32_t sector = 0; sector < count; ++sector) {
        if (wait_not_busy() != 0) return -5;
        uint32_t current = (uint32_t)(lba + sector);
        outb(ATA_DRIVE, 0xe0 | ((current >> 24) & 0x0f));
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW, current & 0xff);
        outb(ATA_LBA_MID, (current >> 8) & 0xff);
        outb(ATA_LBA_HIGH, (current >> 16) & 0xff);
        outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);
        if (wait_drq() != 0) return -5;
        for (unsigned word = 0; word < 256; ++word) {
            uint16_t value = source[sector * 256 + word];
            outw(ATA_DATA, value);
        }
        outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
        if (wait_not_busy() != 0) return -5;
    }
    return 0;
}
