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


#include <stdint.h>
#include <stddef.h>
#include <cpuid.h>

#include "limine.h"
#include "module.hpp"
#include "limine_requests.hpp"
#include "vga.hpp"
#include "panic.hpp"
#include "serial.hpp"
#include "usb.hpp"
#include "gamepad.hpp"
#include "thread.hpp"
#include "fs/vfs.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

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

// ACPI RSDP: gives us the pointer to the ACPI tables
volatile struct limine_rsdp_request g_rsdp_request =
{
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// Enable AVX/AVX512 and XSAVE support dynamically based on CPUID
static void enable_fpu_and_avx() {
    uint32_t eax, ebx, ecx, edx;
    
    // Check XSAVE support (CPUID.1:ECX.XSAVE[bit 26])
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1 << 26))) {
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9);  // OSFXSR (SSE support)
        cr4 |= (1 << 10); // OSXMMEXCPT (SSE exceptions)
        cr4 |= (1 << 18); // OSXSAVE (XSAVE support)
        asm volatile("mov %0, %%cr4" : : "r"(cr4));

        // Base XCR0: x87 (bit 0) and SSE (bit 1)
        uint64_t xcr0 = 1 | (1 << 1);

        // Check AVX support (CPUID.1:ECX.AVX[bit 28])
        if (ecx & (1 << 28)) {
            xcr0 |= (1 << 2); // Enable AVX in XCR0
        }

        // Check AVX512F support (CPUID.7.0:EBX.AVX512F[bit 16])
        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1 << 16))) {
            xcr0 |= (1 << 5) | (1 << 6) | (1 << 7); // Opmask, ZMM_Hi256, Hi16_ZMM
        }

        uint32_t xcr0_low = xcr0 & 0xFFFFFFFF;
        uint32_t xcr0_high = xcr0 >> 32;
        asm volatile("xsetbv" : : "a"(xcr0_low), "d"(xcr0_high), "c"(0));
    } 
    else {
        // Fallback for very old CPUs: Just enable standard SSE
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9);  // OSFXSR
        cr4 |= (1 << 10); // OSXMMEXCPT
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }
}

// Write to MSR
static void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Read from MSR
static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

extern "C" void syscall_entry();

static void enable_syscalls() {
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1;
    wrmsr(0xC0000080, efer);

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x200); 
}

extern "C" void syscall_handler(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t sys_num) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (sys_num == 0) {
        // dummy debug syscall
    }
}


void usb_thread_main() {
    while (1) {
        if (g_usb_manager && g_usb_manager->has_pending_tasks()) g_usb_manager->update();
        else scheduler_block_current_thread();
    }
}

Thread* g_usb_thread = nullptr;
Thread* g_kernel_thread = nullptr;

void test_vfs() {
    if (g_vga) g_vga->write("\n--- Testing VFS ---\n");
    
    // Test SATA read
    int fd1 = vfs_open("/sata/HELLO.TXT");
    if (fd1 >= 0) {
        uint8_t* fbuf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
        vmm_map((uintptr_t)fbuf & ~0xFFFULL, (uintptr_t)fbuf - pmm_hhdm_offset(), VMM_MMIO);
        
        int n = vfs_read(fd1, fbuf, 4095);
        if (n > 0) {
            fbuf[n] = 0;
            if (g_vga) { g_vga->write("VFS SATA Read: "); g_vga->write((const char*)fbuf); g_vga->write("\n"); }
        }
        vfs_close(fd1);
    } 
    else if (g_vga) g_vga->write("VFS SATA: HELLO.TXT not found\n");
    

    // Note: USB testing will only work after USB enumeration completes in the background thread.
    // The user might see the output interlaced.
}

extern "C" [[noreturn]] void kernel_main(void)
{
    // 1. Initialize base hardware and VGA, so we can see output
    system_init_modules();
    
    // 2. Enable MSR Syscalls (yeah, ik we're not in ring 3)
    enable_syscalls();

    // 3. Enable XSAVE and AVX only AFTER checking CPUID
    enable_fpu_and_avx();
    
    // 4. Initialize Scheduler and create test threads
    scheduler_init();
    
    Process* usb_proc = new Process();
    g_usb_thread = new Thread(usb_proc, usb_thread_main, false, "USB Controller");
    scheduler_add_thread(g_usb_thread);

    g_kernel_thread = scheduler_get_current_thread();


    // If framebuffer or VGA are not available, halt
    if (g_framebuffer_request.response == NULL || g_framebuffer_request.response->framebuffer_count < 1) panic("Broken framebuffer");    // This will be printed on the serial instead.
    if (!g_vga) panic("VGA did not init");

    g_vga->write("Welcome to drakos!\n");
    
    test_vfs();
    
    // Main kernel loop: process deferred USB device initialization
    // and poll gamepad input. Runs with interrupts enabled (sti from GDT init).
    while (1) {
        // Process any USB devices that were registered during an interrupt
        
        // Print gamepad state changes
        static Input::GamepadState last_gp_state = {0};
        Input::GamepadState* gp = Input::GamepadManager::get_gamepad(0);
        if (gp && gp->connected) {
            bool changed = false;
            if (gp->btn_a != last_gp_state.btn_a) changed = true;
            if (gp->btn_b != last_gp_state.btn_b) changed = true;
            if (gp->btn_x != last_gp_state.btn_x) changed = true;
            if (gp->btn_y != last_gp_state.btn_y) changed = true;
            if (gp->dpad_up != last_gp_state.dpad_up) changed = true;
            if (gp->dpad_down != last_gp_state.dpad_down) changed = true;
            if (gp->dpad_left != last_gp_state.dpad_left) changed = true;
            if (gp->dpad_right != last_gp_state.dpad_right) changed = true;
            if (gp->btn_l1 != last_gp_state.btn_l1) changed = true;
            if (gp->btn_r1 != last_gp_state.btn_r1) changed = true;
            if (gp->btn_l3 != last_gp_state.btn_l3) changed = true;
            if (gp->btn_r3 != last_gp_state.btn_r3) changed = true;
            if (gp->btn_share != last_gp_state.btn_share) changed = true;
            if (gp->btn_options != last_gp_state.btn_options) changed = true;
            if (gp->btn_logo != last_gp_state.btn_logo) changed = true;
            
            // Analog sticks have jitter, so we use a deadzone of 2 to avoid infinite spam
            auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
            if (diff(gp->left_stick_x, last_gp_state.left_stick_x) > 2) changed = true;
            if (diff(gp->left_stick_y, last_gp_state.left_stick_y) > 2) changed = true;
            if (diff(gp->right_stick_x, last_gp_state.right_stick_x) > 2) changed = true;
            if (diff(gp->right_stick_y, last_gp_state.right_stick_y) > 2) changed = true;
            if (diff(gp->left_trigger, last_gp_state.left_trigger) > 2) changed = true;
            if (diff(gp->right_trigger, last_gp_state.right_trigger) > 2) changed = true;
            
            if (gp->touchpad_touching_1 != last_gp_state.touchpad_touching_1) changed = true;
            if (gp->touchpad_touching_1 && diff(gp->touchpad_x_1, last_gp_state.touchpad_x_1) > 2) changed = true;
            if (gp->touchpad_touching_1 && diff(gp->touchpad_y_1, last_gp_state.touchpad_y_1) > 2) changed = true;
            
            static uint32_t print_counter = 0;
            print_counter++;
            // Print every ~500 iterations depending on hlt
            if (print_counter > 500) {
                print_counter = 0;
                changed = true; // Force print periodically
            }

            if (changed) {
                g_vga->write("Gamepad 0: ");
                if (gp->btn_a) g_vga->write("X ");
                if (gp->btn_b) g_vga->write("O ");
                if (gp->btn_x) g_vga->write("SQ ");
                if (gp->btn_y) g_vga->write("TR ");
                if (gp->dpad_up) g_vga->write("UP ");
                if (gp->dpad_down) g_vga->write("DWN ");
                if (gp->dpad_left) g_vga->write("LFT ");
                if (gp->dpad_right) g_vga->write("RGT ");
                
                if (gp->btn_l1) g_vga->write("L1 ");
                if (gp->btn_r1) g_vga->write("R1 ");
                if (gp->btn_l3) g_vga->write("L3 ");
                if (gp->btn_r3) g_vga->write("R3 ");
                if (gp->btn_share) g_vga->write("SHR ");
                if (gp->btn_options) g_vga->write("OPT ");
                if (gp->btn_logo) g_vga->write("PS ");
                
                // Print analog values
                char a_buf[128];
                const char* hex = "0123456789ABCDEF";
                int i = 0;
                
                a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = 'X'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->left_stick_x >> 4) & 0xF];
                a_buf[i++] = hex[gp->left_stick_x & 0xF];
                a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->left_stick_y >> 4) & 0xF];
                a_buf[i++] = hex[gp->left_stick_y & 0xF];
                
                a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = 'X'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->right_stick_x >> 4) & 0xF];
                a_buf[i++] = hex[gp->right_stick_x & 0xF];
                a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->right_stick_y >> 4) & 0xF];
                a_buf[i++] = hex[gp->right_stick_y & 0xF];
                
                a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = '2'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->left_trigger >> 4) & 0xF];
                a_buf[i++] = hex[gp->left_trigger & 0xF];
                a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = '2'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->right_trigger >> 4) & 0xF];
                a_buf[i++] = hex[gp->right_trigger & 0xF];
                
                // Trackpad
                a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'P'; a_buf[i++] = ':';
                a_buf[i++] = gp->touchpad_touching_1 ? '1' : '0';
                a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'X'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->touchpad_x_1 >> 8) & 0xF];
                a_buf[i++] = hex[(gp->touchpad_x_1 >> 4) & 0xF];
                a_buf[i++] = hex[gp->touchpad_x_1 & 0xF];
                a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
                a_buf[i++] = hex[(gp->touchpad_y_1 >> 8) & 0xF];
                a_buf[i++] = hex[(gp->touchpad_y_1 >> 4) & 0xF];
                a_buf[i++] = hex[gp->touchpad_y_1 & 0xF];
                a_buf[i++] = '\0';
                
                g_vga->write(a_buf);
                g_vga->write("\n");
                
                // Copy state byte by byte to avoid operator= dependency if missing
                const uint8_t* src = reinterpret_cast<const uint8_t*>(gp);
                uint8_t* dst = reinterpret_cast<uint8_t*>(&last_gp_state);
                for (size_t i = 0; i < sizeof(Input::GamepadState); i++) {
                    dst[i] = src[i];
                }
            }
        }
        
        scheduler_block_current_thread();
    }
}