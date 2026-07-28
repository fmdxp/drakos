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


#include "syscalls.hpp"
#include "msr.hpp"
#include "vfs.hpp"
#include "vga.hpp"
#include "thread.hpp"
#include "vmm.hpp"
#include "pmm.hpp"

uint64_t g_kernel_rsp = 0;
uint64_t g_user_rsp   = 0;

extern volatile uint64_t g_system_ticks;

void enable_syscalls() {
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1;
    wrmsr(0xC0000080, efer);

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x200); 
}

extern "C" void syscall_handler(SyscallFrame* frame) {
    int64_t ret = -ENOSYS;
    
    switch (frame->num) {
        case 0: // read (fd, buf, count)
            if (frame->rdi == 0) ret = 0; // EOF on stdin
            else if (frame->rdi > 2) ret = vfs_read((int)frame->rdi - 3, (void*)frame->rsi, (uint32_t)frame->rdx);
            else ret = -EBADF;
            break;

        case 1: // write (fd, buf, count)
            if (frame->rdi == 1 || frame->rdi == 2) {
                if (g_vga) {
                    const char* buf = (const char*)frame->rsi;
                    for (uint32_t i = 0; i < frame->rdx; i++) {
                        char c[2] = { buf[i], 0 };
                        g_vga->write(c);
                    }
                }
                ret = frame->rdx;
            } else if (frame->rdi > 2) {
                ret = vfs_write((int)frame->rdi - 3, (const void*)frame->rsi, (uint32_t)frame->rdx);
            } else {
                ret = -EBADF;
            }
            break;

        case 2: // open (filename, flags, mode)
            ret = vfs_open((const char*)frame->rdi, (int)frame->rsi);
            if (ret >= 0) ret += 3; // Shift fds to leave 0,1,2 free
            else ret = -ENOENT;
            break;

        case 3: // close (fd)
            if (frame->rdi > 2) ret = vfs_close((int)frame->rdi - 3);
            else ret = 0; // close stdout/stderr does nothing for now
            break;

        case 8: // lseek (fd, offset, whence)
            if (frame->rdi > 2) ret = vfs_seek((int)frame->rdi - 3, (int32_t)frame->rsi, (int)frame->rdx);
            else ret = -ESPIPE;
            break;

        case 9: { // mmap (addr, length, prot, flags, fd, offset)
            uint64_t addr = frame->rdi;
            uint64_t length = frame->rsi;
            // int prot = frame->rdx;
            int flags = frame->r10;
            // int fd = frame->r8;
            // int offset = frame->r9;

            // Only support MAP_ANONYMOUS | MAP_PRIVATE (typically 0x22 in Linux) for now
            if ((flags & 0x20) == 0) { 
                ret = -ENOSYS; // File mapping not supported yet
                break;
            }

            // Simple allocator: just pick an address if NULL
            static uintptr_t next_mmap = 0x100000000; // 4GB line
            if (addr == 0) {
                addr = next_mmap;
                next_mmap += (length + 0xFFF) & ~0xFFFULL;
            }

            uint64_t num_pages = (length + 0xFFF) / 0x1000;
            for (uint64_t i = 0; i < num_pages; i++) {
                uintptr_t phys = pmm_alloc_page();
                if (phys) {
                    vmm_map(addr + i * 0x1000, phys, VMM_PRESENT | VMM_WRITE | VMM_USER);
                } else {
                    ret = -ENOMEM;
                    break;
                }
            }

            if (ret == -ENOSYS) ret = addr;
            break;
        }

        case 10: // mprotect
            ret = 0; // Stub, always success for now
            break;

        case 11: // munmap
            ret = 0; // Stub, memory leaks for now
            break;

        case 12: // brk
            // If brk is 0, return current "break". For now just return 0 to force malloc to use mmap
            ret = 0;
            break;

        case 4: // stat
        case 5: { // fstat
            // struct stat *buf = frame->rsi;
            // Minimal stub to satisfy libc. 
            // 0x2180 is S_IFCHR, 0x8180 is S_IFREG.
            uint32_t* st_mode = (uint32_t*)(frame->rsi + 24); // approx offset for st_mode in linux stat
            uint64_t* st_size = (uint64_t*)(frame->rsi + 48); // approx offset for st_size
            
            if (frame->num == 5 && (frame->rdi == 1 || frame->rdi == 2)) {
                *st_mode = 0x21B6; // S_IFCHR | 0666
                *st_size = 0;
            } else {
                *st_mode = 0x81B6; // S_IFREG | 0666
                if (frame->num == 5 && frame->rdi > 2) {
                    *st_size = vfs_filesize((int)frame->rdi - 3);
                } else {
                    *st_size = 0;
                }
            }
            ret = 0;
            break;
        }

        case 21: // access
            ret = 0; // Stub: assume file exists and accessible
            break;

        case 24: // sched_yield
            scheduler_yield();
            ret = 0;
            break;

        case 35: { // nanosleep
            // struct timespec *req = frame->rdi
            if (frame->rdi) {
                uint64_t* req = (uint64_t*)frame->rdi;
                uint64_t sec = req[0];
                uint64_t nsec = req[1];
                
                // roughly 16ms per tick in QEMU APIC timer with 1000000 count divider 16
                uint64_t total_ms = (sec * 1000) + (nsec / 1000000);
                uint64_t target_ticks = g_system_ticks + (total_ms / 16) + 1;
                
                while (g_system_ticks < target_ticks) {
                    scheduler_yield();
                }
            }
            ret = 0;
            break;
        }

        case 39: // getpid
            if (scheduler_get_current_thread() && scheduler_get_current_thread()->parent_process) {
                ret = scheduler_get_current_thread()->parent_process->pid;
            } else {
                ret = 1;
            }
            break;

        case 60: // exit (status)
            if (scheduler_get_current_thread()) {
                scheduler_get_current_thread()->state = THREAD_DEAD;
                scheduler_yield(); // Will never return here
            }
            break;

        case 63: { // uname
            char* buf = (char*)frame->rdi;
            // struct utsname has 6 strings of 65 chars (sysname, nodename, release, version, machine, domainname)
            if (buf) {
                const char* sysname = "DrakOS";
                for (int i=0; i<65; i++) buf[i] = (i < 7) ? sysname[i] : 0;
                const char* nodename = "drakos-machine";
                for (int i=0; i<65; i++) buf[65 + i] = (i < 15) ? nodename[i] : 0;
                const char* release = "0.1";
                for (int i=0; i<65; i++) buf[130 + i] = (i < 4) ? release[i] : 0;
                const char* version = "1";
                for (int i=0; i<65; i++) buf[195 + i] = (i < 2) ? version[i] : 0;
                const char* machine = "x86_64";
                for (int i=0; i<65; i++) buf[260 + i] = (i < 7) ? machine[i] : 0;
            }
            ret = 0;
            break;
        }

        case 72: // fcntl
            ret = 0; // Stub
            break;

        case 79: { // getcwd
            char* buf = (char*)frame->rdi;
            if (buf) {
                buf[0] = '/';
                buf[1] = 0;
            }
            ret = 2; // bytes written including null
            break;
        }
        
        case 78: // getdents
        case 80: // chdir
        case 83: // mkdir
        case 84: // rmdir
        case 32: // dup
        case 33: // dup2
            ret = -ENOSYS; // Not implemented yet
            break;

        case 95: // umask
            ret = 022;
            break;

        case 96: // gettimeofday
            // struct timeval *tv
            if (frame->rdi) {
                uint64_t* tv_sec = (uint64_t*)frame->rdi;
                uint64_t* tv_usec = (uint64_t*)(frame->rdi + 8);
                *tv_sec = 0;
                *tv_usec = 0;
            }
            ret = 0;
            break;

        case 102: // getuid
        case 104: // getgid
        case 107: // geteuid
        case 108: // getegid
            ret = 0; // We are always root
            break;

        default:
            // Unknown syscall
            break;
    }
    
    frame->num = (uint64_t)ret;
}
