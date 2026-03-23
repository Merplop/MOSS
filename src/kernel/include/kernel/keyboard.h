#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_INFO_ADDRESS 0x1600

#pragma once

enum {
	LSHIFT_MAKE  = 0x2A,
	LSHIFT_BREAK = 0xAA,
	RSHIFT_MAKE  = 0x36,
	RSHIFT_BREAK = 0xB6,
	LCTRL_MAKE   = 0x1D,
	LCTRL_BREAK  = 0x9D,
};

uint8_t get_key(void);

#endif
