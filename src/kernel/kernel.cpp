#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "module.hpp"
#include "limine_requests.hpp"
#include "vga.hpp"

volatile struct limine_framebuffer_request g_framebuffer_request =
{
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// Memory map: tells us which physical memory regions are usable
volatile struct limine_memmap_request g_memmap_request =
{
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// Higher Half Direct Map: gives us the virtual offset to access physical memory
volatile struct limine_hhdm_request g_hhdm_request =
{
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

extern "C" [[noreturn]] void _start(void)
{
    // Execute all registered modules in order
    system_init_modules();

    // If framebuffer is not available, halt
    if (g_framebuffer_request.response == NULL || g_framebuffer_request.response->framebuffer_count < 1)
    {
        while (1) asm volatile ("hlt");
    }


    g_vga->write("Welcome to drakos!");
    
    
    while (1) asm volatile("hlt");
}