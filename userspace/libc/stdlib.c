#include "stdlib.h"
#include "syscall.h"

void exit(int status) {
    syscall1(SYS_exit, (uint64_t)status);
    while (1) {}
}

static uint8_t* heap_start = NULL;
static size_t heap_offset = 0;
static size_t heap_size = 0;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    
    // align to 16 bytes
    size = (size + 15) & ~15;

    if (heap_start == NULL || heap_offset + size > heap_size) {
        // Allocate a new chunk (2 MB or larger)
        size_t alloc_size = (size > 0x200000) ? size : 0x200000;
        uint64_t addr = syscall6(SYS_mmap, 0, (uint64_t)alloc_size, 3, 0x22, 0, 0);
        if (addr < 4096) return NULL;
        
        heap_start = (uint8_t*)addr;
        heap_offset = 0;
        heap_size = alloc_size;
    }

    void* ptr = heap_start + heap_offset;
    heap_offset += size;
    return ptr;
}

void free(void *ptr) {
    // Stub
    (void)ptr;
}
