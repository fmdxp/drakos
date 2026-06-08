#pragma once

#include "limine.h"

// These Limine requests are defined in kernel.cpp.
// Other subsystems (PMM, VMM) include this header to read them.

extern volatile struct limine_memmap_request g_memmap_request;
extern volatile struct limine_hhdm_request   g_hhdm_request;
extern volatile struct limine_framebuffer_request g_framebuffer_request;
extern volatile struct limine_rsdp_request   g_rsdp_request;
