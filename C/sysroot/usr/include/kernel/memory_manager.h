#include<string.h>

#pragma once

#define BLOCK_SIZE 4096
#define BLOCKS_PER_BYTE 8

static uint32_t *memory_map = 0;
static uint32_t max_blocks = 0;
static uint32_t used_blocks = 0;

void set_block(uint32_t bit) {
    memory_map[bit/32] |= (1 << (bit % 32));
}

void unset_block(uint32_t bit) {
    memory_map[bit/32] &= ~(1 << (bit % 32));
}

uint8_t check_block(uint32_t bit) {
    return memory_map[bit/32] & (1 << (bit % 32));
}

int32_t find_first_free_blocks(uint32_t num_blocks) {
    if (num_blocks == 0) {return -1;}
    for (uint32_t i = 0; i < max_blocks / 32; i++) {
        if (memory_map[i] != 0xFFFFFFFF) {
            for (int32_t j = 0; j < 32; j++) {
                int32_t bit = 1 << j;
                if (!(memory_map[i] & j)) {
                    int32_t start_bit = i*32 + bit;
                    uint32_t free_blocks = 0;

                    for (uint32_t count = 0; count <= num_blocks; count++) {
                        if(!check_block(start_bit + count)) {free_blocks++;}
                        if(free_blocks == num_blocks) {
                            return i*32 + j;
                        }
                    }
                }
            }
        }
    }
    return -1;
}

void initialize_memory_manager(uint64_t start_address, uint64_t size) {
    memory_map = (uint32_t *)start_address;
    max_blocks = size / BLOCK_SIZE;
    used_blocks = max_blocks;

    memset(memory_map, 0xFF, max_blocks / BLOCKS_PER_BYTE);
}

void initialize_memory_region(uint64_t base_address, uint64_t size) {
    int32_t align = base_address / BLOCK_SIZE;
    int32_t num_blocks = size / BLOCK_SIZE;

    for (; num_blocks > 0; num_blocks--) {
        unset_block(align++);
        used_blocks--;
    }

    set_block(0);
}

void deinitialize_memory_region(uint32_t base_address, uint32_t size) {
    int32_t align = base_address / BLOCK_SIZE;
    int32_t num_blocks = size / BLOCK_SIZE;

    for (; num_blocks > 0; num_blocks--) {
        set_block(align++);
        used_blocks--;
    }
}

uint32_t *allocate_blocks(uint32_t num_blocks) {
    if ((max_blocks - used_blocks) <= num_blocks) {return 0;}
    int32_t starting_block = find_first_free_blocks(num_blocks);
    if (starting_block == -1) {return 0;}
    for (uint32_t i = 0; i < num_blocks; i++) {
        set_block(starting_block + i);
    }
    used_blocks += num_blocks;

    uint32_t address = starting_block * BLOCK_SIZE;

    return (uint32_t *)address;
}

void free_blocks(uint32_t *address, uint32_t num_blocks) {
    int32_t starting_block = (uint32_t)address / BLOCK_SIZE;
    for (uint32_t i = 0; i < num_blocks; i++) {
        unset_block(starting_block + i);
    }

    used_blocks -= num_blocks;
}