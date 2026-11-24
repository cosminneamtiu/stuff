// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap screens: Library, Convert, Saves, Log, plus dialogs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SEEDSWAP_VERSION "0.12.1"

void ui_init(void);
bool ui_input(uint64_t down);  // false = quit
void ui_draw(void);
void ui_exit(void);
