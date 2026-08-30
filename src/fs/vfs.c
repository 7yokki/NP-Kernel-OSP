#include <npk/boot.h>
#include <npk/block.h>
#include <npk/heap.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/keyboard.h>
#include <npk/procfs.h>
#include <npk/string.h>
#include <npk/vfs.h>

#define MAX_INITRD_FILES 64U
#define MAX_OPEN_DESCRIPTORS 64U
#define OPEN_DESCRIPTOR_BASE MAX_INITRD_FILES
#define PROC_BUFFER_SIZE 4096U
#define PIPE_CAPACITY 4096U
#define VFS_NAME_CAPACITY 256U
#define EPOLL_MAX_WATCHES 32U
#define SHM_MAX_PAGES 256U
#define SHM_MAX_BYTES (SHM_MAX_PAGES * NPK_PAGE_SIZE)
#define PERSIST_MAGIC 0x4e504b46U /* NPKF */
#define PERSIST_VERSION 2U
#define PERSIST_MAX_FILES 64U
#define PERSIST_NAME_CAPACITY 48U
#define PERSIST_DATA_SECTORS 128U
/* Sector 1/10 are dual superblocks, 2-9/12-19 are dual inode copies,
 * sector 11 is the commit marker, and data begins after both metadata slots. */
#define PERSIST_SUPER_PRIMARY 1U
#define PERSIST_INODE_PRIMARY 2U
#define PERSIST_SUPER_BACKUP 10U
#define PERSIST_TXN_SECTOR 11U
#define PERSIST_INODE_BACKUP 12U
#define PERSIST_INODE_SECTORS 8U
#define PERSIST_DATA_START 32U
#define PERSIST_FILE_BYTES (PERSIST_DATA_SECTORS * NPK_SECTOR_SIZE)
#define PERSIST_TXN_MAGIC 0x4e504b54U /* NPKT */
#define PERSIST_TXN_PREPARE 1U
#define PERSIST_TXN_CLEAR 0U
#define PERSIST_CRC24_MASK 0x00ffffffU
#define PERSIST_MAX_SECTOR 0xffffffffU

typedef struct pipe_object {
    uint8_t buffer[PIPE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    uint32_t readers;
    uint32_t writers;
} pipe_object_t;

typedef struct {
    size_t size;
    size_t page_count;
    paddr_t pages[SHM_MAX_PAGES];
} shm_object_t;

typedef struct {
    bool used;
    int fd;
    uint32_t events;
    uint64_t data;
} epoll_watch_t;

typedef struct {
    epoll_watch_t watches[EPOLL_MAX_WATCHES];
} epoll_object_t;

typedef struct {
    uint8_t used;
    uint8_t reserved[3];
    uint32_t size;
    uint32_t start_sector;
    uint32_t mode;
    char name[PERSIST_NAME_CAPACITY];
} persistent_inode_disk_t;

_Static_assert(sizeof(persistent_inode_disk_t) == 64, "persistent inode layout");

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t next_sector;
    uint32_t generation;
    uint32_t inode_checksum;
    uint32_t checksum;
    uint8_t reserved[480];
} persistent_superblock_t;

_Static_assert(sizeof(persistent_superblock_t) == NPK_SECTOR_SIZE, "persistent superblock layout");

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t target_generation;
    uint32_t inode_checksum;
    uint32_t checksum;
    uint8_t reserved[488];
} persistent_txn_t;

_Static_assert(sizeof(persistent_txn_t) == NPK_SECTOR_SIZE, "persistent transaction layout");

typedef struct {
    initrd_file_t file;
    size_t offset;
    uint32_t refs;
    bool used;
    bool pseudo;
    bool directory;
    bool readable;
    bool writable;
    bool is_pipe;
    bool is_epoll;
    bool is_shm;
    bool initrd_directory;
    bool persistent;
    bool persistent_directory;
    persistent_inode_disk_t *persistent_inode;
    uint8_t *persistent_data;
    pipe_object_t *pipe;
    epoll_object_t *epoll;
    shm_object_t *shm;
    char name_storage[VFS_NAME_CAPACITY];
    uint8_t proc_buffer[PROC_BUFFER_SIZE];
} descriptor_t;

typedef struct {
    uint64_t inode;
    int64_t offset;
    uint16_t record_length;
    uint8_t type;
} dirent64_header_t;

static descriptor_t files[MAX_INITRD_FILES + MAX_OPEN_DESCRIPTORS];
static unsigned file_count;
static persistent_superblock_t persistent_super;
static persistent_inode_disk_t persistent_inodes[PERSIST_MAX_FILES] __attribute__((aligned(2)));
static persistent_inode_disk_t persistent_backup_inodes[PERSIST_MAX_FILES] __attribute__((aligned(2)));
static bool persistent_ready;
static bool persistent_active_slot;
static uint8_t persistent_zero_data[PERSIST_FILE_BYTES] __attribute__((aligned(2)));

static bool add_overflow(size_t a, size_t b) { return b > SIZE_MAX - a; }
static bool align4_checked(size_t value, size_t *result) {
    if (value > SIZE_MAX - 3) return false;
    *result = (value + 3) & ~3ULL;
    return true;
}

static int allocate_descriptor(void) {
    for (unsigned i = OPEN_DESCRIPTOR_BASE; i < OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS; ++i) {
        if (files[i].used) continue;
        memset(&files[i], 0, sizeof(files[i]));
        files[i].used = true;
        files[i].refs = 1;
        return (int)i;
    }
    return -24;
}

static bool copy_name(char *destination, size_t capacity, const char *source) {
    if (!destination || !source || capacity == 0) return false;
    size_t length = strlen(source);
    if (length + 1 > capacity) return false;
    memcpy(destination, source, length + 1);
    return true;
}

void vfs_register_file(const char *name, const uint8_t *data, size_t size) {
    if (file_count >= MAX_INITRD_FILES || !name || (!data && size != 0)) return;
    files[file_count].file.name = name;
    files[file_count].file.data = data;
    files[file_count].file.size = size;
    files[file_count].used = true;
    files[file_count].readable = true;
    ++file_count;
}

static uint32_t hex_field(const char *s, size_t length) {
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t c = (uint8_t)s[i];
        if (c >= '0' && c <= '9') value = (value << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') value = (value << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = (value << 4) | (c - 'A' + 10);
        else return 0;
    }
    return value;
}

static void parse_newc(const uint8_t *archive, size_t size) {
    size_t offset = 0;
    while (offset <= size && size - offset >= 110) {
        const char *header = (const char *)(archive + offset);
        if (strncmp(header, "070701", 6) != 0) break;
        uint32_t mode = hex_field(header + 14, 8);
        uint32_t file_size = hex_field(header + 54, 8);
        uint32_t name_size = hex_field(header + 94, 8);
        if (name_size == 0 || name_size > size - offset - 110) break;
        size_t name_offset = offset + 110;
        size_t name_end;
        if (add_overflow(name_offset, name_size) || !align4_checked(name_offset + name_size, &name_end) || name_end > size) break;
        if (strncmp((const char *)(archive + name_offset), "TRAILER!!!", 10) == 0) break;
        size_t data_end;
        if (add_overflow(name_end, file_size) || !align4_checked(name_end + file_size, &data_end) || data_end > size) break;
        if ((mode & 0170000) == 0100000 && archive[name_offset + name_size - 1] == '\0')
            vfs_register_file((const char *)(archive + name_offset), archive + name_end, file_size);
        offset = data_end;
    }
}

static uint32_t persistent_checksum_bytes(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static uint32_t persistent_inode_checksum(const persistent_inode_disk_t *inodes) {
    return persistent_checksum_bytes(inodes, sizeof(persistent_inode_disk_t) * PERSIST_MAX_FILES);
}

static uint32_t persistent_super_checksum(const persistent_superblock_t *super) {
    persistent_superblock_t copy = *super;
    copy.checksum = 0;
    return persistent_checksum_bytes(&copy, sizeof(copy));
}

static uint32_t persistent_txn_checksum(const persistent_txn_t *txn) {
    persistent_txn_t copy = *txn;
    copy.checksum = 0;
    return persistent_checksum_bytes(&copy, sizeof(copy));
}

static int persistent_write_metadata(void) {
    if (!persistent_ready) return -19;
    if (persistent_super.next_sector < PERSIST_DATA_START) return -5;
    const ata_device_info_t *device = ata_primary_info();
    if (!device || persistent_super.next_sector > device->sectors) return -5;

    persistent_superblock_t next_super = persistent_super;
    if (next_super.generation == UINT32_MAX) return -5;
    ++next_super.generation;
    next_super.inode_count = 0;
    for (unsigned i = 0; i < PERSIST_MAX_FILES; ++i)
        if (persistent_inodes[i].used) ++next_super.inode_count;
    next_super.inode_checksum = persistent_inode_checksum(persistent_inodes);
    next_super.checksum = 0;
    next_super.checksum = persistent_super_checksum(&next_super);

    persistent_txn_t txn = {0};
    txn.magic = PERSIST_TXN_MAGIC;
    txn.version = PERSIST_VERSION;
    txn.state = PERSIST_TXN_PREPARE;
    txn.target_generation = next_super.generation;
    txn.inode_checksum = next_super.inode_checksum;
    txn.checksum = persistent_txn_checksum(&txn);
    if (ata_write_sectors(PERSIST_TXN_SECTOR, 1, &txn) != 0) return -5;

    if (ata_write_sectors(PERSIST_INODE_BACKUP, PERSIST_INODE_SECTORS, persistent_inodes) != 0)
        return -5;
    if (ata_write_sectors(PERSIST_SUPER_BACKUP, 1, &next_super) != 0) return -5;
    if (ata_write_sectors(PERSIST_INODE_PRIMARY, PERSIST_INODE_SECTORS, persistent_inodes) != 0)
        return -5;
    if (ata_write_sectors(PERSIST_SUPER_PRIMARY, 1, &next_super) != 0) return -5;

    persistent_txn_t clear = {0};
    clear.magic = PERSIST_TXN_MAGIC;
    clear.version = PERSIST_VERSION;
    clear.state = PERSIST_TXN_CLEAR;
    clear.checksum = persistent_txn_checksum(&clear);
    if (ata_write_sectors(PERSIST_TXN_SECTOR, 1, &clear) != 0) return -5;
    persistent_super = next_super;
    persistent_active_slot = false;
    return 0;
}

static bool persistent_inode_valid(const persistent_inode_disk_t *inode,
                                   uint64_t sectors) {
    if (!inode || !inode->used || inode->name[0] == '\0' ||
        inode->name[PERSIST_NAME_CAPACITY - 1] != '\0' || inode->size > PERSIST_FILE_BYTES ||
        inode->start_sector < PERSIST_DATA_START ||
        inode->start_sector > sectors || PERSIST_DATA_SECTORS > sectors - inode->start_sector)
        return false;
    return true;
}

static bool persistent_super_slot_valid(const persistent_superblock_t *super,
                                        const persistent_inode_disk_t *inodes,
                                        uint64_t sectors) {
    if (!super || !inodes || super->magic != PERSIST_MAGIC ||
        super->version != PERSIST_VERSION || super->data_start != PERSIST_DATA_START ||
        super->inode_count > PERSIST_MAX_FILES || super->next_sector < PERSIST_DATA_START ||
        super->next_sector > sectors || super->generation == 0 ||
        super->checksum != persistent_super_checksum(super) ||
        super->inode_checksum != persistent_inode_checksum(inodes)) return false;
    unsigned count = 0;
    for (unsigned i = 0; i < PERSIST_MAX_FILES; ++i) {
        if (!inodes[i].used) continue;
        if (!persistent_inode_valid(&inodes[i], sectors)) return false;
        ++count;
    }
    return count == super->inode_count;
}

static bool persistent_txn_valid(const persistent_txn_t *txn) {
    return txn && txn->magic == PERSIST_TXN_MAGIC && txn->version == PERSIST_VERSION &&
           (txn->state == PERSIST_TXN_PREPARE || txn->state == PERSIST_TXN_CLEAR) &&
           txn->checksum == persistent_txn_checksum(txn);
}

static void persistent_format(void) {
    memset(&persistent_super, 0, sizeof(persistent_super));
    memset(persistent_inodes, 0, sizeof(persistent_inodes));
    memset(persistent_backup_inodes, 0, sizeof(persistent_backup_inodes));
    persistent_super.magic = PERSIST_MAGIC;
    persistent_super.version = PERSIST_VERSION;
    persistent_super.data_start = PERSIST_DATA_START;
    persistent_super.next_sector = PERSIST_DATA_START;
    persistent_super.generation = 0;
    persistent_ready = true;
    persistent_active_slot = false;
    (void)persistent_write_metadata();
}

static void persistent_init(void) {
    persistent_ready = false;
    persistent_active_slot = false;
    const ata_device_info_t *device = ata_primary_info();
    if (!device || !device->present || !device->writable || device->sectors < PERSIST_DATA_START + PERSIST_DATA_SECTORS) {
        log_message(LOG_WARN, "vfs", "persistent NPKFS unavailable; ATA disk is absent or too small");
        return;
    }

    persistent_superblock_t primary_super __attribute__((aligned(2)));
    persistent_superblock_t backup_super __attribute__((aligned(2)));
    persistent_txn_t txn __attribute__((aligned(2)));
    memset(&primary_super, 0, sizeof(primary_super));
    memset(&backup_super, 0, sizeof(backup_super));
    memset(&txn, 0, sizeof(txn));
    bool primary_read = ata_read_sectors(PERSIST_SUPER_PRIMARY, 1, &primary_super) == 0 &&
                        ata_read_sectors(PERSIST_INODE_PRIMARY, PERSIST_INODE_SECTORS, persistent_inodes) == 0;
    bool backup_read = ata_read_sectors(PERSIST_SUPER_BACKUP, 1, &backup_super) == 0 &&
                       ata_read_sectors(PERSIST_INODE_BACKUP, PERSIST_INODE_SECTORS, persistent_backup_inodes) == 0;
    bool txn_read = ata_read_sectors(PERSIST_TXN_SECTOR, 1, &txn) == 0;
    bool primary_valid = primary_read && persistent_super_slot_valid(&primary_super, persistent_inodes, device->sectors);
    bool backup_valid = backup_read && persistent_super_slot_valid(&backup_super, persistent_backup_inodes, device->sectors);

    if (!primary_valid && !backup_valid) {
        persistent_format();
        LOG_INFOF("vfs", "persistent NPKFS formatted after invalid metadata", PERSIST_DATA_START);
        return;
    }
    if (backup_valid && (!primary_valid || backup_super.generation > primary_super.generation)) {
        persistent_super = backup_super;
        memcpy(persistent_inodes, persistent_backup_inodes, sizeof(persistent_inodes));
        persistent_active_slot = true;
    } else {
        persistent_super = primary_super;
        persistent_active_slot = false;
    }
    persistent_ready = true;

    if (txn_read && persistent_txn_valid(&txn) && txn.state == PERSIST_TXN_PREPARE) {
        /* A valid slot is authoritative after a power loss. Clearing only the
         * marker prevents replay of an already committed generation. */
        persistent_txn_t clear = {0};
        clear.magic = PERSIST_TXN_MAGIC;
        clear.version = PERSIST_VERSION;
        clear.state = PERSIST_TXN_CLEAR;
        clear.checksum = persistent_txn_checksum(&clear);
        (void)ata_write_sectors(PERSIST_TXN_SECTOR, 1, &clear);
        LOG_INFOF("vfs", "persistent NPKFS recovered transaction", persistent_super.generation);
    }
    LOG_INFOF("vfs", "persistent NPKFS files", persistent_super.inode_count);
}

static bool persistent_extract_name(const char *path, char *name, size_t capacity) {
    const char *prefix = "/persist/";
    if (!path || !name || capacity == 0 || strncmp(path, prefix, 9) != 0) return false;
    const char *source = path + 9;
    size_t length = strlen(source);
    if (length == 0 || length >= capacity) return false;
    for (size_t i = 0; i < length; ++i) if (source[i] == '/') return false;
    memcpy(name, source, length + 1);
    return true;
}

static persistent_inode_disk_t *persistent_find(const char *name) {
    if (!name) return NULL;
    for (unsigned i = 0; i < PERSIST_MAX_FILES; ++i)
        if (persistent_inodes[i].used && strcmp(persistent_inodes[i].name, name) == 0) return &persistent_inodes[i];
    return NULL;
}

static persistent_inode_disk_t *persistent_create(const char *name) {
    if (!persistent_ready || !name || strlen(name) >= PERSIST_NAME_CAPACITY ||
        persistent_super.inode_count >= PERSIST_MAX_FILES) return NULL;
    const ata_device_info_t *device = ata_primary_info();
    if (!device || persistent_super.next_sector > device->sectors ||
        PERSIST_DATA_SECTORS > device->sectors - persistent_super.next_sector) return NULL;
    persistent_inode_disk_t *inode = NULL;
    for (unsigned i = 0; i < PERSIST_MAX_FILES; ++i) {
        if (!persistent_inodes[i].used) { inode = &persistent_inodes[i]; break; }
    }
    if (!inode) return NULL;
    memset(inode, 0, sizeof(*inode));
    inode->used = 1;
    inode->size = 0;
    inode->start_sector = persistent_super.next_sector;
    inode->mode = 0100666U;
    memcpy(inode->name, name, strlen(name) + 1);
    persistent_super.next_sector += PERSIST_DATA_SECTORS;
    ++persistent_super.inode_count;
    memset(persistent_zero_data, 0, sizeof(persistent_zero_data));
    if (ata_write_sectors(inode->start_sector, PERSIST_DATA_SECTORS, persistent_zero_data) != 0 || persistent_write_metadata() != 0) {
        memset(inode, 0, sizeof(*inode));
        --persistent_super.inode_count;
        persistent_super.next_sector -= PERSIST_DATA_SECTORS;
        (void)persistent_write_metadata();
        return NULL;
    }
    return inode;
}

static int persistent_sync_descriptor(descriptor_t *descriptor) {
    if (!descriptor || !descriptor->persistent || !descriptor->persistent_inode || !descriptor->persistent_data)
        return -9;
    persistent_inode_disk_t *inode = descriptor->persistent_inode;
    inode->size = (uint32_t)descriptor->file.size;
    if (ata_write_sectors(inode->start_sector, PERSIST_DATA_SECTORS, descriptor->persistent_data) != 0)
        return -5;
    return persistent_write_metadata();
}

void vfs_init(void) {
    memset(files, 0, sizeof(files));
    file_count = 0;
    memset(&persistent_super, 0, sizeof(persistent_super));
    memset(persistent_inodes, 0, sizeof(persistent_inodes));
    persistent_init();
    if (g_boot_info.modules != NULL) {
        for (uint64_t i = 0; i < g_boot_info.modules->module_count; ++i) {
            struct limine_file *module = g_boot_info.modules->modules[i];
            if (module && module->address && module->size >= 6 && strncmp((const char *)module->address, "070701", 6) == 0) {
                parse_newc((const uint8_t *)module->address, module->size);
                LOG_INFOF("vfs", "initramfs files", file_count);
                break;
            }
        }
    }
    if (file_count == 0) log_message(LOG_WARN, "vfs", "no CPIO initramfs module; read-only VFS is empty");
}

static bool canonicalize_path(const char *path, char *canonical, size_t capacity) {
    if (!path || !canonical || capacity < 2 || path[0] != '/') return false;
    size_t input = 1;
    size_t output = 0;
    while (path[input] != '\0') {
        while (path[input] == '/') ++input;
        if (path[input] == '\0') break;
        size_t start = input;
        while (path[input] != '\0' && path[input] != '/') ++input;
        size_t length = input - start;
        if (length == 1 && path[start] == '.') continue;
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') return false;
        if (output + 1 + length >= capacity) return false;
        canonical[output++] = '/';
        memcpy(canonical + output, path + start, length);
        output += length;
    }
    if (output == 0) canonical[output++] = '/';
    canonical[output] = '\0';
    return true;
}

static bool initrd_name_matches(const char *archive_name, const char *canonical) {
    if (!archive_name || !canonical) return false;
    if (archive_name[0] == '/') return strcmp(archive_name, canonical) == 0;
    return canonical[0] == '/' && strcmp(archive_name, canonical + 1) == 0;
}

static bool initrd_directory_exists(const char *canonical) {
    if (!canonical) return false;
    if (strcmp(canonical, "/") == 0) return true;
    size_t directory_length = strlen(canonical) - 1;
    if (directory_length == 0) return true;
    for (unsigned i = 0; i < file_count; ++i) {
        const char *name = files[i].file.name;
        if (name[0] == '/') ++name;
        if (strlen(name) > directory_length &&
            strncmp(name, canonical + 1, directory_length) == 0 &&
            name[directory_length] == '/') return true;
    }
    return false;
}

int vfs_open(const char *path) {
    return vfs_open_flags(path, 0, 0);
}

int vfs_open_flags(const char *path, uint32_t flags, uint32_t mode) {
    (void)mode;
    if (path == NULL) return -2;
    char canonical[VFS_NAME_CAPACITY];
    if (!canonicalize_path(path, canonical, sizeof(canonical))) return -22;
    bool write_requested = (flags & (NPK_O_WRONLY | NPK_O_RDWR)) != 0;
    if (strcmp(canonical, "/persist") == 0) {
        if (write_requested) return -21;
        int fd = allocate_descriptor();
        if (fd < 0) return fd;
        descriptor_t *descriptor = &files[fd];
        descriptor->persistent_directory = true;
        descriptor->directory = true;
        descriptor->readable = true;
        if (!copy_name(descriptor->name_storage, sizeof(descriptor->name_storage), "/persist")) {
            memset(descriptor, 0, sizeof(*descriptor));
            return -36;
        }
        descriptor->file.name = descriptor->name_storage;
        return fd;
    }
    char persistent_name[PERSIST_NAME_CAPACITY];
    if (persistent_extract_name(canonical, persistent_name, sizeof(persistent_name))) {
        if (!persistent_ready) return -19;
        persistent_inode_disk_t *inode = persistent_find(persistent_name);
        if (!inode && (flags & NPK_O_CREAT) != 0) inode = persistent_create(persistent_name);
        if (!inode) return -2;
        if (write_requested && (inode->mode & 0200U) == 0) return -13;
        int fd = allocate_descriptor();
        if (fd < 0) return fd;
        descriptor_t *descriptor = &files[fd];
        descriptor->persistent = true;
        descriptor->persistent_inode = inode;
        descriptor->persistent_data = (uint8_t *)kmalloc(PERSIST_FILE_BYTES);
        if (!descriptor->persistent_data || ata_read_sectors(inode->start_sector, PERSIST_DATA_SECTORS, descriptor->persistent_data) != 0) {
            if (descriptor->persistent_data) kfree(descriptor->persistent_data);
            memset(descriptor, 0, sizeof(*descriptor));
            return -5;
        }
        descriptor->file.size = inode->size;
        descriptor->file.data = descriptor->persistent_data;
        descriptor->readable = (flags & NPK_O_WRONLY) == 0;
        descriptor->writable = write_requested;
        if ((flags & NPK_O_TRUNC) != 0 && descriptor->writable) {
            memset(descriptor->persistent_data, 0, PERSIST_FILE_BYTES);
            descriptor->file.size = 0;
            if (persistent_sync_descriptor(descriptor) != 0) {
                kfree(descriptor->persistent_data);
                memset(descriptor, 0, sizeof(*descriptor));
                return -5;
            }
        }
        if ((flags & NPK_O_APPEND) != 0) descriptor->offset = descriptor->file.size;
        copy_name(descriptor->name_storage, sizeof(descriptor->name_storage), canonical);
        descriptor->file.name = descriptor->name_storage;
        return fd;
    }
    if (write_requested) return -30;
    if (procfs_is_path(canonical)) {
        int fd = allocate_descriptor();
        if (fd < 0) return fd;
        descriptor_t *descriptor = &files[fd];
        ssize_t length = procfs_snapshot(canonical, (char *)descriptor->proc_buffer, sizeof(descriptor->proc_buffer));
        if (length < 0 || !copy_name(descriptor->name_storage, sizeof(descriptor->name_storage), canonical)) {
            memset(descriptor, 0, sizeof(*descriptor));
            return length < 0 ? (int)length : -36;
        }
        descriptor->file.name = descriptor->name_storage;
        descriptor->file.data = descriptor->proc_buffer;
        descriptor->file.size = (size_t)length;
        descriptor->pseudo = true;
        descriptor->readable = true;
        return fd;
    }
    if (strcmp(canonical, "/") == 0 || initrd_directory_exists(canonical)) {
        int fd = allocate_descriptor();
        if (fd < 0) return fd;
        descriptor_t *descriptor = &files[fd];
        descriptor->pseudo = strcmp(canonical, "/") == 0;
        descriptor->initrd_directory = !descriptor->pseudo;
        descriptor->directory = true;
        descriptor->readable = true;
        if (!copy_name(descriptor->name_storage, sizeof(descriptor->name_storage), canonical)) {
            memset(descriptor, 0, sizeof(*descriptor));
            return -36;
        }
        descriptor->file.name = descriptor->name_storage;
        return fd;
    }
    for (unsigned i = 0; i < file_count; ++i) {
        if (!initrd_name_matches(files[i].file.name, canonical)) continue;
        int fd = allocate_descriptor();
        if (fd < 0) return fd;
        descriptor_t *descriptor = &files[fd];
        descriptor->file = files[i].file;
        descriptor->readable = true;
        return fd;
    }
    return -2;
}

int vfs_shm_create(size_t size) {
    if (size == 0 || size > SHM_MAX_BYTES || size > SIZE_MAX - (NPK_PAGE_SIZE - 1)) return -22;
    size_t page_count = (size + NPK_PAGE_SIZE - 1) / NPK_PAGE_SIZE;
    if (page_count == 0 || page_count > SHM_MAX_PAGES) return -22;
    int fd = allocate_descriptor();
    if (fd < 0) return fd;
    shm_object_t *object = (shm_object_t *)kcalloc(1, sizeof(*object));
    if (!object) {
        memset(&files[fd], 0, sizeof(files[fd]));
        return -12;
    }
    object->size = size;
    object->page_count = page_count;
    for (size_t i = 0; i < page_count; ++i) {
        object->pages[i] = pmm_alloc_page();
        if (!object->pages[i]) {
            for (size_t j = 0; j < i; ++j) pmm_free_page(object->pages[j]);
            kfree(object);
            memset(&files[fd], 0, sizeof(files[fd]));
            return -12;
        }
        memset(phys_to_virt(object->pages[i]), 0, NPK_PAGE_SIZE);
    }
    descriptor_t *descriptor = &files[fd];
    descriptor->is_shm = true;
    descriptor->readable = true;
    descriptor->writable = true;
    descriptor->file.name = "npk-shm";
    descriptor->file.size = size;
    descriptor->shm = object;
    return fd;
}

bool vfs_is_shared_memory(int fd) {
    return fd >= (int)OPEN_DESCRIPTOR_BASE &&
           (unsigned)fd < OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS &&
           files[fd].used && files[fd].is_shm && files[fd].shm != NULL;
}

paddr_t vfs_shm_page(int fd, size_t page_index) {
    if (!vfs_is_shared_memory(fd) || page_index >= files[fd].shm->page_count) return 0;
    return files[fd].shm->pages[page_index];
}

size_t vfs_shm_page_count(int fd) {
    return vfs_is_shared_memory(fd) ? files[fd].shm->page_count : 0;
}

int vfs_retain(int fd) {
    if (fd == 0 || fd == 1 || fd == 2) return 0;
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || files[fd].refs == UINT32_MAX) return -9;
    ++files[fd].refs;
    if (files[fd].is_pipe && files[fd].pipe) {
        if (files[fd].readable) ++files[fd].pipe->readers;
        if (files[fd].writable) ++files[fd].pipe->writers;
    }
    return 0;
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || !files[fd].readable) return -9;
    if (count == 0) return 0;
    if (!buffer) return -14;
    descriptor_t *descriptor = &files[fd];
    if (descriptor->is_pipe) {
        pipe_object_t *pipe = descriptor->pipe;
        if (!pipe || pipe->count == 0) return pipe && pipe->writers != 0 ? -11 : 0;
        size_t amount = count < pipe->count ? count : pipe->count;
        for (size_t i = 0; i < amount; ++i) {
            ((uint8_t *)buffer)[i] = pipe->buffer[pipe->head];
            pipe->head = (pipe->head + 1) % PIPE_CAPACITY;
        }
        pipe->count -= amount;
        return (ssize_t)amount;
    }
    if (descriptor->directory) return -21;
    if (descriptor->is_shm) return -22;
    if (descriptor->offset >= descriptor->file.size) return 0;
    size_t remaining = descriptor->file.size - descriptor->offset;
    if (count > remaining) count = remaining;
    memcpy(buffer, descriptor->file.data + descriptor->offset, count);
    descriptor->offset += count;
    return (ssize_t)count;
}

ssize_t vfs_read_at(int fd, void *buffer, size_t count, uint64_t offset) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || !files[fd].readable || files[fd].is_pipe || files[fd].is_shm || files[fd].directory)
        return -9;
    if (count == 0) return 0;
    if (!buffer || offset > files[fd].file.size) return -14;
    size_t position = (size_t)offset;
    size_t remaining = files[fd].file.size - position;
    if (count > remaining) count = remaining;
    memcpy(buffer, files[fd].file.data + position, count);
    return (ssize_t)count;
}

bool vfs_file_backed(int fd) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || files[fd].pseudo || files[fd].persistent || files[fd].is_pipe ||
        files[fd].is_epoll || files[fd].directory || !files[fd].readable || files[fd].writable)
        return false;
    /* The first backend is immutable initramfs regular files. Mutable persistent
     * descriptors are intentionally excluded until page-cache/writeback semantics
     * exist, and procfs snapshots are not stable mapping sources. */
    return files[fd].file.data != NULL || files[fd].file.size == 0;
}

ssize_t vfs_write(int fd, const void *buffer, size_t count) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || !files[fd].writable) return -9;
    if (count == 0) return 0;
    if (!buffer) return -14;
    descriptor_t *descriptor = &files[fd];
    if (descriptor->is_shm) return -22;
    if (descriptor->persistent) {
        if (!descriptor->persistent_inode || !descriptor->persistent_data) return -5;
        if ((size_t)descriptor->offset > PERSIST_FILE_BYTES) return -27;
        size_t amount = count;
        if (amount > PERSIST_FILE_BYTES - descriptor->offset) amount = PERSIST_FILE_BYTES - descriptor->offset;
        if (amount == 0) return -27;
        size_t old_size = descriptor->file.size;
        memcpy(descriptor->persistent_data + descriptor->offset, buffer, amount);
        descriptor->offset += amount;
        if (descriptor->offset > descriptor->file.size) descriptor->file.size = descriptor->offset;
        if (persistent_sync_descriptor(descriptor) != 0) {
            (void)ata_read_sectors(descriptor->persistent_inode->start_sector, PERSIST_DATA_SECTORS,
                                   descriptor->persistent_data);
            descriptor->file.size = old_size;
            if (descriptor->offset > old_size) descriptor->offset = old_size;
            return -5;
        }
        return (ssize_t)amount;
    }
    pipe_object_t *pipe = descriptor->pipe;
    if (!descriptor->is_pipe || !pipe) return -9;
    if (pipe->readers == 0) return -32;
    size_t available = PIPE_CAPACITY - pipe->count;
    if (available == 0) return -11;
    size_t amount = count < available ? count : available;
    for (size_t i = 0; i < amount; ++i) {
        pipe->buffer[pipe->tail] = ((const uint8_t *)buffer)[i];
        pipe->tail = (pipe->tail + 1) % PIPE_CAPACITY;
    }
    pipe->count += amount;
    return (ssize_t)amount;
}

int vfs_close(int fd) {
    if (fd == 0 || fd == 1 || fd == 2) return 0;
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used || files[fd].refs == 0) return -9;
    descriptor_t *descriptor = &files[fd];
    if (descriptor->is_pipe && descriptor->pipe) {
        if (descriptor->readable && descriptor->pipe->readers != 0) --descriptor->pipe->readers;
        if (descriptor->writable && descriptor->pipe->writers != 0) --descriptor->pipe->writers;
    }
    if (--descriptor->refs != 0) return 0;
    if (descriptor->is_epoll && descriptor->epoll) {
        for (unsigned i = 0; i < EPOLL_MAX_WATCHES; ++i) {
            if (descriptor->epoll->watches[i].used) {
                (void)vfs_close(descriptor->epoll->watches[i].fd);
                memset(&descriptor->epoll->watches[i], 0, sizeof(descriptor->epoll->watches[i]));
            }
        }
        kfree(descriptor->epoll);
        descriptor->epoll = NULL;
    }
    pipe_object_t *pipe = descriptor->pipe;
    if (descriptor->is_shm && descriptor->shm) {
        for (size_t i = 0; i < descriptor->shm->page_count; ++i)
            if (descriptor->shm->pages[i]) pmm_free_page(descriptor->shm->pages[i]);
        kfree(descriptor->shm);
        descriptor->shm = NULL;
    }
    if (descriptor->persistent && descriptor->persistent_data) {
        (void)persistent_sync_descriptor(descriptor);
        kfree(descriptor->persistent_data);
    }
    memset(descriptor, 0, sizeof(*descriptor));
    if (pipe && pipe->readers == 0 && pipe->writers == 0) kfree(pipe);
    return 0;
}

uint32_t vfs_poll_events(int fd, uint32_t requested) {
    if (fd == 1 || fd == 2) return requested & NPK_VFS_POLLOUT;
    if (fd == 0) return keyboard_has_data() ? (requested & NPK_VFS_POLLIN) : 0;
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[fd].used) return NPK_VFS_POLLERR;
    descriptor_t *descriptor = &files[fd];
    uint32_t ready = 0;
    if (descriptor->is_pipe && descriptor->pipe) {
        if (descriptor->readable && (descriptor->pipe->count != 0 || descriptor->pipe->writers == 0)) {
            ready |= descriptor->pipe->count != 0 ? NPK_VFS_POLLIN : NPK_VFS_POLLHUP;
        }
        if (descriptor->writable && descriptor->pipe->readers == 0) ready |= NPK_VFS_POLLERR | NPK_VFS_POLLHUP;
        else if (descriptor->writable && descriptor->pipe->count < PIPE_CAPACITY) ready |= NPK_VFS_POLLOUT;
    } else if (descriptor->is_epoll && descriptor->epoll) {
        for (unsigned i = 0; i < EPOLL_MAX_WATCHES; ++i) {
            epoll_watch_t *watch = &descriptor->epoll->watches[i];
            if (watch->used && vfs_poll_events(watch->fd, watch->events)) {
                ready |= NPK_VFS_POLLIN;
                break;
            }
        }
    } else if (descriptor->is_shm) {
        /* Shared memory has no byte-stream readiness semantics. */
        ready = 0;
    } else {
        /* Regular initramfs, procfs and directory descriptors are always
         * non-blocking: EOF and directory end are readable immediately. */
        if (descriptor->readable) ready |= NPK_VFS_POLLIN;
        if (descriptor->writable) ready |= NPK_VFS_POLLOUT;
    }
    return ready & (requested | NPK_VFS_POLLERR | NPK_VFS_POLLHUP);
}

int vfs_epoll_create(void) {
    int fd = allocate_descriptor();
    if (fd < 0) return fd;
    epoll_object_t *object = (epoll_object_t *)kcalloc(1, sizeof(*object));
    if (!object) {
        memset(&files[fd], 0, sizeof(files[fd]));
        return -12;
    }
    descriptor_t *descriptor = &files[fd];
    descriptor->is_epoll = true;
    descriptor->epoll = object;
    descriptor->file.name = "epoll";
    return fd;
}

int vfs_epoll_ctl(int epfd, int operation, int fd, uint32_t events, uint64_t data) {
    if (epfd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)epfd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[epfd].used || !files[epfd].is_epoll || !files[epfd].epoll ||
        fd < 0 || (fd >= (int)OPEN_DESCRIPTOR_BASE + (int)MAX_OPEN_DESCRIPTORS && fd != 1 && fd != 2) || fd == epfd)
        return -9;
    if (fd >= (int)OPEN_DESCRIPTOR_BASE && !files[fd].used) return -9;
    descriptor_t *ep = &files[epfd];
    epoll_watch_t *found = NULL;
    epoll_watch_t *free_slot = NULL;
    for (unsigned i = 0; i < EPOLL_MAX_WATCHES; ++i) {
        epoll_watch_t *watch = &ep->epoll->watches[i];
        if (watch->used && watch->fd == fd) found = watch;
        if (!watch->used && !free_slot) free_slot = watch;
    }
    if (operation == 1) { /* EPOLL_CTL_ADD */
        if (found || !free_slot || vfs_retain(fd) != 0) return found ? -17 : -28;
        free_slot->used = true;
        free_slot->fd = fd;
        free_slot->events = events;
        free_slot->data = data;
        return 0;
    }
    if (operation == 2) { /* EPOLL_CTL_DEL */
        if (!found) return -2;
        (void)vfs_close(found->fd);
        memset(found, 0, sizeof(*found));
        return 0;
    }
    if (operation == 3) { /* EPOLL_CTL_MOD */
        if (!found) return -2;
        found->events = events;
        found->data = data;
        return 0;
    }
    return -22;
}

ssize_t vfs_epoll_wait(int epfd, vfs_epoll_event_t *events, size_t capacity) {
    if (epfd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)epfd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS ||
        !files[epfd].used || !files[epfd].is_epoll || !files[epfd].epoll) return -9;
    if (!events || capacity == 0) return 0;
    size_t written = 0;
    descriptor_t *ep = &files[epfd];
    for (unsigned i = 0; i < EPOLL_MAX_WATCHES && written < capacity; ++i) {
        epoll_watch_t *watch = &ep->epoll->watches[i];
        if (!watch->used) continue;
        uint32_t ready = vfs_poll_events(watch->fd, watch->events);
        if (!ready) continue;
        events[written].data = watch->data;
        events[written].events = ready;
        events[written].padding = 0;
        ++written;
    }
    return (ssize_t)written;
}

int vfs_sync_fd(int fd) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS || !files[fd].used) return -9;
    return files[fd].persistent ? persistent_sync_descriptor(&files[fd]) : 0;
}

int vfs_sync_all(void) {
    int result = 0;
    for (unsigned i = OPEN_DESCRIPTOR_BASE; i < OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS; ++i) {
        if (!files[i].used || !files[i].persistent) continue;
        int status = persistent_sync_descriptor(&files[i]);
        if (status < 0 && result == 0) result = status;
    }
    return result;
}

ssize_t vfs_size(int fd) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS || !files[fd].used) return -9;
    if (files[fd].is_pipe) return (ssize_t)files[fd].pipe->count;
    return files[fd].directory ? 0 : (ssize_t)files[fd].file.size;
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS || !files[fd].used) return -9;
    descriptor_t *descriptor = &files[fd];
    if (descriptor->directory || descriptor->is_pipe || descriptor->is_shm) return -29;
    int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)descriptor->offset : (whence == 2 ? (int64_t)descriptor->file.size : INT64_MIN));
    if (base == INT64_MIN || (offset > 0 && base > INT64_MAX - offset) || (offset < 0 && base < INT64_MIN - offset)) return -22;
    int64_t target = base + offset;
    if (target < 0) return -22;
    descriptor->offset = (size_t)target;
    return target;
}

int vfs_stat_fd(int fd, vfs_stat_t *status) {
    if (!status) return -14;
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS || !files[fd].used) return -9;
    const descriptor_t *descriptor = &files[fd];
    memset(status, 0, sizeof(*status));
    status->inode = (uint64_t)(fd + 1);
    status->mode = descriptor->is_pipe ? 0010666U : (descriptor->directory ? 0040555U :
                     (descriptor->is_shm ? 0100666U :
                     (descriptor->persistent ? descriptor->persistent_inode->mode : 0100444U)));
    status->uid = 0;
    status->gid = 0;
    status->size = descriptor->is_pipe ? descriptor->pipe->count : (descriptor->directory ? 0 : descriptor->file.size);
    status->blocks = (status->size + 511) / 512;
    status->is_directory = descriptor->directory ? 1U : 0U;
    return 0;
}

static bool initrd_child_from_name(const char *directory, const char *archive_name,
                                   char *child, size_t capacity, bool *is_directory) {
    if (!directory || !archive_name || !child || !is_directory || capacity == 0) return false;
    const char *name = archive_name;
    if (name[0] == '/') ++name;
    size_t directory_length = strcmp(directory, "/") == 0 ? 0 : strlen(directory) - 1;
    if (directory_length != 0 &&
        (strlen(name) <= directory_length || strncmp(name, directory + 1, directory_length) != 0 ||
         name[directory_length] != '/')) return false;
    const char *relative = name + directory_length + (directory_length != 0 ? 1 : 0);
    size_t length = 0;
    while (relative[length] != '\0' && relative[length] != '/') ++length;
    if (length == 0 || length + 1 > capacity) return false;
    memcpy(child, relative, length);
    child[length] = '\0';
    *is_directory = relative[length] == '/';
    return true;
}

static bool initrd_child_seen(const char *directory, unsigned limit, const char *child, bool is_directory) {
    char previous[VFS_NAME_CAPACITY];
    for (unsigned i = 0; i < limit; ++i) {
        bool previous_directory = false;
        if (!initrd_child_from_name(directory, files[i].file.name, previous, sizeof(previous), &previous_directory)) continue;
        if (previous_directory == is_directory && strcmp(previous, child) == 0) return true;
    }
    return false;
}

static bool initrd_child_at(const char *directory, size_t wanted, char *child,
                            size_t capacity, bool *is_directory) {
    size_t ordinal = 0;
    for (unsigned i = 0; i < file_count; ++i) {
        bool current_directory = false;
        if (!initrd_child_from_name(directory, files[i].file.name, child, capacity, &current_directory)) continue;
        if (initrd_child_seen(directory, i, child, current_directory)) continue;
        if (ordinal++ == wanted) {
            *is_directory = current_directory;
            return true;
        }
    }
    return false;
}

ssize_t vfs_getdents64(int fd, void *buffer, size_t capacity) {
    if (fd < (int)OPEN_DESCRIPTOR_BASE || (unsigned)fd >= OPEN_DESCRIPTOR_BASE + MAX_OPEN_DESCRIPTORS || !files[fd].used) return -9;
    if (!buffer) return -14;
    descriptor_t *descriptor = &files[fd];
    if (!descriptor->directory) return -20;
    size_t written = 0;
    if (descriptor->persistent_directory) {
        while (descriptor->offset < PERSIST_MAX_FILES) {
            size_t index = descriptor->offset++;
            if (!persistent_inodes[index].used) continue;
            const char *name = persistent_inodes[index].name;
            size_t name_length = strlen(name) + 1;
            size_t record_length = (sizeof(dirent64_header_t) + name_length + 7) & ~7ULL;
            if (record_length > UINT16_MAX) return -22;
            if (record_length > capacity - written) {
                if (written == 0) return -22;
                --descriptor->offset;
                break;
            }
            dirent64_header_t header = { .inode = index + 2, .offset = (int64_t)descriptor->offset,
                                         .record_length = (uint16_t)record_length, .type = 8 };
            uint8_t *destination = (uint8_t *)buffer + written;
            memset(destination, 0, record_length);
            memcpy(destination, &header, sizeof(header));
            memcpy(destination + sizeof(header), name, name_length);
            written += record_length;
        }
        return (ssize_t)written;
    }
    const char *directory = descriptor->file.name ? descriptor->file.name : "/";
    while (true) {
        size_t entry = descriptor->offset++;
        char child[VFS_NAME_CAPACITY];
        bool child_directory = false;
        const char *name = NULL;
        if (entry == 0) {
            name = ".";
        } else if (entry == 1) {
            name = "..";
        } else {
            size_t child_index = entry - 2;
            if (strcmp(directory, "/") == 0 && persistent_ready) {
                if (child_index == 0) {
                    name = "persist";
                    child_directory = true;
                } else {
                    if (!initrd_child_at(directory, child_index - 1, child, sizeof(child), &child_directory)) break;
                    name = child;
                }
            } else {
                if (!initrd_child_at(directory, child_index, child, sizeof(child), &child_directory)) break;
                name = child;
            }
        }
        size_t name_length = strlen(name) + 1;
        size_t record_length = (sizeof(dirent64_header_t) + name_length + 7) & ~7ULL;
        if (record_length > UINT16_MAX) return -22;
        if (record_length > capacity - written) {
            if (written == 0) return -22;
            --descriptor->offset;
            break;
        }
        dirent64_header_t header = {
            .inode = entry + 1,
            .offset = (int64_t)descriptor->offset,
            .record_length = (uint16_t)record_length,
            .type = entry < 2 || child_directory ? 4 : 8
        };
        uint8_t *destination = (uint8_t *)buffer + written;
        memset(destination, 0, record_length);
        memcpy(destination, &header, sizeof(header));
        memcpy(destination + sizeof(header), name, name_length);
        written += record_length;
    }
    return (ssize_t)written;
}

int vfs_pipe_create(int *read_fd, int *write_fd) {
    if (!read_fd || !write_fd) return -14;
    pipe_object_t *pipe = (pipe_object_t *)kcalloc(1, sizeof(*pipe));
    if (!pipe) return -12;
    int reader = allocate_descriptor();
    int writer = allocate_descriptor();
    if (reader < 0 || writer < 0) {
        if (reader >= 0) memset(&files[reader], 0, sizeof(files[reader]));
        if (writer >= 0) memset(&files[writer], 0, sizeof(files[writer]));
        kfree(pipe);
        return -24;
    }
    descriptor_t *r = &files[reader];
    descriptor_t *w = &files[writer];
    r->is_pipe = true; r->readable = true; r->pipe = pipe;
    w->is_pipe = true; w->writable = true; w->pipe = pipe;
    r->file.name = "pipe"; w->file.name = "pipe";
    pipe->readers = 1;
    pipe->writers = 1;
    *read_fd = reader;
    *write_fd = writer;
    return 0;
}
