#include <npk/heap.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/string.h>

#define HEAP_MAGIC 0x4e504b4845415041ULL
#define HEAP_FREED 0x4e504b4652454544ULL

typedef struct heap_block {
    uint64_t magic;
    size_t size;
    uint8_t free;
    uint8_t reserved[7];
    struct heap_block *next;
    struct heap_block *prev;
} __attribute__((aligned(NPK_HEAP_ALIGNMENT))) heap_block_t;

static heap_block_t *heap_head;
static uint64_t heap_mapped;
static uint64_t heap_free;
static bool heap_ready;

static bool add_overflow_u64(uint64_t a, uint64_t b) { return b > UINT64_MAX - a; }

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    if (alignment == 0 || value > UINT64_MAX - (alignment - 1)) return 0;
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool pointer_in_heap(const void *pointer) {
    uint64_t address = (uint64_t)pointer;
    return address >= NPK_HEAP_BASE + sizeof(heap_block_t) && address < NPK_HEAP_BASE + heap_mapped;
}

static bool map_more(uint64_t required_bytes) {
    uint64_t required = align_up_u64(required_bytes, NPK_PAGE_SIZE);
    if (required == 0 || required > NPK_HEAP_LIMIT - NPK_HEAP_BASE) return false;
    while (heap_mapped < required) {
        paddr_t physical = pmm_alloc_page();
        if (physical == 0) return false;
        vaddr_t virtual_address = NPK_HEAP_BASE + heap_mapped;
        if (!vmm_map_page(virtual_address, physical, VM_WRITE | VM_NX)) {
            pmm_free_page(physical);
            return false;
        }
        memset((void *)virtual_address, 0, NPK_PAGE_SIZE);
        heap_mapped += NPK_PAGE_SIZE;
    }
    return true;
}

static heap_block_t *find_free(size_t size) {
    for (heap_block_t *block = heap_head; block != NULL; block = block->next)
        if (block->free && block->size >= size) return block;
    return NULL;
}

static bool split_block(heap_block_t *block, size_t size) {
    if (block->size < size + sizeof(heap_block_t) + NPK_HEAP_ALIGNMENT) return false;
    heap_block_t *tail = (heap_block_t *)((uint8_t *)(block + 1) + size);
    tail->magic = HEAP_MAGIC;
    tail->size = block->size - size - sizeof(heap_block_t);
    tail->free = 1;
    tail->next = block->next;
    tail->prev = block;
    if (tail->next) tail->next->prev = tail;
    block->next = tail;
    block->size = size;
    return true;
}

static void merge_with_next(heap_block_t *block) {
    heap_block_t *next = block->next;
    if (!next || !next->free) return;
    uint8_t *expected = (uint8_t *)(block + 1) + block->size;
    if ((uint8_t *)next != expected || next->magic != HEAP_MAGIC) return;
    block->size += sizeof(heap_block_t) + next->size;
    heap_free += sizeof(heap_block_t);
    block->next = next->next;
    if (block->next) block->next->prev = block;
    next->magic = HEAP_FREED;
    next->free = 1;
}

void kheap_init(void) {
    heap_head = NULL;
    heap_mapped = 0;
    heap_free = 0;
    heap_ready = false;
    if (!map_more(NPK_PAGE_SIZE)) {
        LOG_ERRORF("heap", "initial page unavailable", 0);
        return;
    }
    heap_head = (heap_block_t *)NPK_HEAP_BASE;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = NPK_PAGE_SIZE - sizeof(heap_block_t);
    heap_head->free = 1;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_free = heap_head->size;
    heap_ready = true;
    LOG_INFOF("heap", "initial heap bytes", heap_mapped);
}

void *kmalloc(size_t size) {
    if (!heap_ready || size == 0) return NULL;
    size_t aligned = (size_t)align_up_u64(size, NPK_HEAP_ALIGNMENT);
    if (aligned == 0) return NULL;
    heap_block_t *block = find_free(aligned);
    if (!block) {
        uint64_t old_end = NPK_HEAP_BASE + heap_mapped;
        uint64_t required;
        if (add_overflow_u64(heap_mapped, sizeof(heap_block_t) + aligned)) return NULL;
        required = heap_mapped + sizeof(heap_block_t) + aligned;
        if (!map_more(required)) return NULL;
        heap_block_t *tail = (heap_block_t *)old_end;
        tail->magic = HEAP_MAGIC;
        tail->size = (size_t)((NPK_HEAP_BASE + heap_mapped) - old_end - sizeof(heap_block_t));
        tail->free = 1;
        tail->next = NULL;
        tail->prev = NULL;
        if (heap_head == NULL) {
            heap_head = tail;
        } else {
            heap_block_t *last = heap_head;
            while (last->next) last = last->next;
            last->next = tail;
            tail->prev = last;
        }
        block = tail;
        heap_free += block->size;
    }
    size_t free_before = block->size;
    bool split = split_block(block, aligned);
    block->free = 0;
    heap_free -= split ? free_before - block->next->size : free_before;
    return (void *)(block + 1);
}

void *kcalloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void *memory = kmalloc(total);
    if (memory) memset(memory, 0, total);
    return memory;
}

void kfree(void *pointer) {
    if (pointer == NULL) return;
    if (!pointer_in_heap(pointer)) {
        LOG_ERRORF("heap", "kfree rejected pointer", (uint64_t)pointer);
        return;
    }
    heap_block_t *block = ((heap_block_t *)pointer) - 1;
    if (block->magic != HEAP_MAGIC || block->free || block->size == 0) {
        LOG_ERRORF("heap", "kfree rejected invalid or duplicate pointer", (uint64_t)pointer);
        return;
    }
    block->free = 1;
    heap_free += block->size;
    if (block->prev && block->prev->free) {
        merge_with_next(block->prev);
        block = block->prev;
    }
    merge_with_next(block);
}

bool kheap_validate(void) {
    if (!heap_ready) return false;
    uint64_t observed_free = 0;
    uint64_t address = NPK_HEAP_BASE;
    heap_block_t *previous = NULL;
    for (heap_block_t *block = heap_head; block != NULL; block = block->next) {
        if ((uint64_t)block != address || block->magic != HEAP_MAGIC || block->prev != previous || block->size == 0) return false;
        if (block->size > NPK_HEAP_LIMIT - NPK_HEAP_BASE || add_overflow_u64(address, sizeof(heap_block_t) + block->size)) return false;
        address += sizeof(heap_block_t) + block->size;
        if (block->free) observed_free += block->size;
        previous = block;
    }
    if (address != NPK_HEAP_BASE + heap_mapped) return false;
    return observed_free == heap_free;
}

uint64_t kheap_mapped_bytes(void) { return heap_mapped; }
uint64_t kheap_free_bytes(void) { return heap_free; }
