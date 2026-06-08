/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#pragma once

#include "limine.h"

// These Limine requests are defined in kernel.cpp.
// Other subsystems (PMM, VMM) include this header to read them.

extern volatile struct limine_memmap_request g_memmap_request;
extern volatile struct limine_hhdm_request   g_hhdm_request;
extern volatile struct limine_framebuffer_request g_framebuffer_request;
extern volatile struct limine_rsdp_request   g_rsdp_request;
