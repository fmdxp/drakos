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

#include "kernel.hpp"
#include "module.hpp"

#include "limine_requests.hpp"
#include "limine.h"

#include "vga.hpp"
#include "serial.hpp"
#include "panic.hpp"
#include "usb.hpp"
#include "gamepad.hpp"

#include "thread.hpp"
#include "vfs.hpp"

#include "pmm.hpp"
#include "vmm.hpp"

#include "syscalls.hpp"
#include "cpu.hpp"


static void test_vfs() {
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
    
    // Test SATA write back
    fd1 = vfs_open("/sata/NEW.TXT", VFS_O_CREATE | VFS_O_WRITE);
    if (fd1 >= 0) {
        const char* msg = "This file was created by Drakos!\n";
        g_vga->write("VFS SATA Write: ");
        int w = vfs_write(fd1, msg, 33);
        if (w > 0) g_vga->write("Success!\n");
        else g_vga->write("Failed.\n");
        vfs_close(fd1);

        // Read it back
        int fd2 = vfs_open("/sata/NEW.TXT", VFS_O_READ);
        if (fd2 >= 0) {
            uint8_t* rbuf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
            vmm_map((uintptr_t)rbuf & ~0xFFFULL, (uintptr_t)rbuf - pmm_hhdm_offset(), VMM_MMIO);
            int n = vfs_read(fd2, rbuf, 4095);
            if (n > 0) {
                rbuf[n] = 0;
                if (g_vga) { g_vga->write("VFS SATA Read back: "); g_vga->write((const char*)rbuf); g_vga->write("\n"); }
            }
            vfs_close(fd2);
        }
    }

    // Test NVMe write back
    int fd_nvme = vfs_open("/nvme/NVME_OUT.TXT", VFS_O_CREATE | VFS_O_WRITE);
    if (fd_nvme >= 0) {
        const char* msg = "Hello from NVMe Driver!\n";
        g_vga->write("VFS NVMe Write: ");
        int w = vfs_write(fd_nvme, msg, 24);
        if (w > 0) g_vga->write("Success!\n");
        else g_vga->write("Failed.\n");
        vfs_close(fd_nvme);

        // Read it back
        int fd2_nvme = vfs_open("/nvme/NVME_OUT.TXT", VFS_O_READ);
        if (fd2_nvme >= 0) {
            uint8_t* rbuf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
            int n = vfs_read(fd2_nvme, rbuf, 4095);
            if (n > 0) {
                rbuf[n] = 0;
                if (g_vga) { g_vga->write("VFS NVMe Read back: "); g_vga->write((const char*)rbuf); }
            } else {
                if (g_vga) { g_vga->write("VFS NVMe Read back: 0 bytes read!\n"); }
            }
            vfs_close(fd2_nvme);
        } else {
            if (g_vga) { g_vga->write("VFS NVMe: Failed to open for reading!\n"); }
        }
    }
    

    // Test USB read

    if (g_vga) g_vga->write("Waiting for USB stick to mount...\n");
    
    // Wait for USB to mount by checking if we can open a root file or just yield some time
    bool usb_ready = false;
    for (int i = 0; i < 200; i++) {
        int test_fd = vfs_open("/usb/USB.TXT", VFS_O_CREATE | VFS_O_WRITE);
        if (test_fd >= 0) {
            usb_ready = true;
            
            const char* msg = "Hello from USB Driver!\n";
            g_vga->write("VFS USB Write: ");
            int w = vfs_write(test_fd, msg, 23);
            if (w > 0) g_vga->write("Success!\n");
            else g_vga->write("Failed.\n");
            
            vfs_close(test_fd);
            break;
        }
        scheduler_yield();
    }

    if (usb_ready) {
        int fd2 = vfs_open("/usb/USB.TXT", VFS_O_READ);
        if (fd2 >= 0) {
            uint8_t* fbuf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
            
            int n = vfs_read(fd2, fbuf, 4095);
            if (n > 0) {
                fbuf[n] = 0;
                if (g_vga) { g_vga->write("VFS USB Read back: "); g_vga->write((const char*)fbuf); }
            } else if (g_vga) { g_vga->write("VFS USB Read back: 0 bytes read!\n"); }
            
            vfs_close(fd2);
        } else if (g_vga) { g_vga->write("VFS USB: Failed to open for reading!\n"); }
        
    } 
    else if (g_vga) g_vga->write("VFS USB: USB enumeration timed out\n");
    
}

static void make_threads_and_processes()
{
    Process* usb_proc = new Process();
    g_usb_thread = new Thread(usb_proc, usb_thread_main, false, "USB Controller");
    scheduler_add_thread(g_usb_thread);


    Process* gamepad_proc = new Process();
    g_gamepad_thread = new Thread(gamepad_proc, gamepad_thread_main, false, "Gamepad Debugger");
    scheduler_add_thread(g_gamepad_thread);

    g_kernel_thread = scheduler_get_current_thread();
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
    
    // 5. Make all the kernel threads and processes
    make_threads_and_processes();


    // If framebuffer or VGA are not available, halt
    if (g_framebuffer_request.response == NULL || g_framebuffer_request.response->framebuffer_count < 1) panic("Broken framebuffer");    // This will be printed on the serial instead.
    if (!g_vga) panic("VGA did not init");


    // Kernel is now initialized!
    g_vga->write("Welcome to drakos!\n");
    test_vfs();

    
    

    // Main Kernel Loop
    while (1) {
        scheduler_block_current_thread();
    }
}