#ifndef KERNEL_MEMORY_MANAGER_H
#define KERNEL_MEMORY_MANAGER_H

#include <stdint.h>

#define BLOCK_SIZE 4096
#define BLOCKS_PER_BYTE 8

void set_block(uint32_t bit);
void unset_block(uint32_t bit);
uint8_t check_block(uint32_t bit);
int32_t find_first_free_blocks(uint32_t num_blocks);
void initialize_memory_manager(uint32_t start_address, uint32_t size);
void initialize_memory_region(uint32_t base_address, uint32_t size);
void deinitialize_memory_region(uint32_t base_address, uint32_t size);
uint32_t *allocate_blocks(uint32_t num_blocks);
void free_blocks(uint32_t *address, uint32_t num_blocks);

#endif /* KERNEL_MEMORY_MANAGER_H */
