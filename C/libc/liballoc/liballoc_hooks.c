#include <liballoc.h>
#include <stdint.h>

/* allocate_blocks / free_blocks are defined in kernel via memory_manager.h */
extern uint32_t *allocate_blocks(uint32_t num_blocks);
extern void free_blocks(uint32_t *address, uint32_t num_blocks);

/** Lock the memory allocator by disabling interrupts.
 *  This is sufficient for a single-CPU kernel. */
int liballoc_lock() {
    asm volatile ("cli");
    return 0;
}

/** Unlock the memory allocator by re-enabling interrupts. */
int liballoc_unlock() {
    asm volatile ("sti");
    return 0;
}

/** Allocate contiguous physical pages for liballoc.
 *  @param pages  Number of 4096-byte pages requested.
 *  @return       Pointer to the allocated region, or NULL on failure. */
void *liballoc_alloc(int pages) {
    return (void *)allocate_blocks((uint32_t)pages);
}

/** Free previously allocated pages.
 *  @param ptr    Pointer returned by a prior liballoc_alloc call.
 *  @param pages  Number of pages to free.
 *  @return       0 on success. */
int liballoc_free(void *ptr, int pages) {
    free_blocks((uint32_t *)ptr, (uint32_t)pages);
    return 0;
}
