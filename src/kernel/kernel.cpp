#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "module.hpp"

static volatile struct limine_framebuffer_request framebuffer_request =
{
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr     // Needed to add this to suppress a warning
};

extern "C" [[noreturn]] void _start(void)
{
    // Execute all registered modules in order
    system_init_modules();

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1)
    {
        while (1) asm volatile ("hlt");
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    uint32_t *fb_ptr = (uint32_t*) fb->address;
    size_t total_pixels = fb->width * fb->height;

    for (size_t i = 0; i < total_pixels; i++) 
    {
        fb_ptr[i] = 0x0000FFFF;
    }

    while (1) asm volatile("hlt");
}