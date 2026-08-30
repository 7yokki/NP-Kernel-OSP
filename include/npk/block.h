#ifndef NPK_BLOCK_H
#define NPK_BLOCK_H

#include "types.h"

#define NPK_SECTOR_SIZE 512U

typedef struct {
    bool present;
    bool writable;
    uint64_t sectors;
} ata_device_info_t;

void ata_init(void);
const ata_device_info_t *ata_primary_info(void);
int ata_read_sectors(uint64_t lba, uint32_t count, void *buffer);
int ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer);

#endif
