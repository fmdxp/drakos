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
#include "kernel/panic.hpp"
#include "cpu.hpp"
#include <stddef.h>

uint64_t g_kernel_rsp = 0;
uint64_t g_user_rsp   = 0;

extern volatile uint64_t g_system_ticks;

static bool user_range_access(uintptr_t address, uint64_t length, bool write) {
    if (!length) return true;
    constexpr uintptr_t USER_LIMIT = 1ULL << 47;
    if (!address || address >= USER_LIMIT || length > USER_LIMIT - address) return false;
    Thread* thread = scheduler_get_current_thread();
    if (!thread || !thread->parent_process || !thread->parent_process->is_user) return false;
    uint64_t end = address + length;
    uintptr_t page = address & ~(PAGE_SIZE - 1);
    while (page < end) {
        VmmMappingInfo mapping = {};
        if (!vmm_query_mapping(page, &mapping) || !(mapping.flags & VMM_USER) ||
            (write && !(mapping.flags & VMM_WRITE))) return false;
        if (page > UINTPTR_MAX - PAGE_SIZE) return false;
        page += PAGE_SIZE;
    }
    return true;
}

static bool copy_user_string(uintptr_t address, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    for (size_t i = 0; i < capacity; ++i) {
        if (address > UINTPTR_MAX - i || !user_range_access(address + i, 1, false)) return false;
        output[i] = *reinterpret_cast<const char*>(address + i);
        if (!output[i]) return true;
    }
    output[capacity - 1] = 0;
    return false;
}

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
            if (frame->rdx > UINT32_MAX || !user_range_access(frame->rsi, frame->rdx, true)) ret = -EFAULT;
            else if (frame->rdi == 0) ret = 0; // EOF on stdin
            else if (frame->rdi > 2) ret = vfs_read((int)frame->rdi - 3, (void*)frame->rsi, (uint32_t)frame->rdx);
            else ret = -EBADF;
            break;

        case 1: // write (fd, buf, count)
            if (frame->rdx > UINT32_MAX || !user_range_access(frame->rsi, frame->rdx, false)) ret = -EFAULT;
            else if (frame->rdi == 1 || frame->rdi == 2) {
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
        {
            char filename[256];
            if (!copy_user_string(frame->rdi, filename, sizeof(filename))) { ret = -EFAULT; break; }
            ret = vfs_open(filename, (int)frame->rsi);
            if (ret >= 0) ret += 3; // Shift fds to leave 0,1,2 free
            else ret = -ENOENT;
            break;
        }

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
            uint64_t prot = frame->rdx;
            int flags = frame->r10;
            // int fd = frame->r8;
            // int offset = frame->r9;

            // Only support MAP_ANONYMOUS | MAP_PRIVATE (typically 0x22 in Linux) for now
            if ((flags & 0x20) == 0 || (flags & ~0x22) != 0) {
                ret = -ENOSYS; // File mapping not supported yet
                break;
            }
            if ((prot & ~0x7) != 0 || prot == 0 || (prot & 0x6) == 0x6) {
                ret = -EINVAL;
                break;
            }

            if (!length || length > UINT64_MAX - (PAGE_SIZE - 1)) {
                ret = -EINVAL;
                break;
            }
            uint64_t rounded_length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            constexpr uintptr_t USER_LIMIT = 1ULL << 47;
            // Simple allocator: just pick an address if NULL
            static uintptr_t next_mmap = 0x100000000; // 4GB line
            if (addr == 0) {
                if (next_mmap > USER_LIMIT - rounded_length) { ret = -ENOMEM; break; }
                addr = next_mmap;
                next_mmap += rounded_length;
            }
            if ((addr & (PAGE_SIZE - 1)) || addr >= USER_LIMIT ||
                rounded_length > USER_LIMIT - addr) {
                ret = -EINVAL;
                break;
            }

            uint64_t num_pages = rounded_length / PAGE_SIZE;
            Thread* current = scheduler_get_current_thread();
            uintptr_t root = current && current->parent_process
                ? current->parent_process->page_table_phys : vmm_get_kernel_pml4();
            if (!current || !current->parent_process || !current->parent_process->is_user) {
                ret = -EPERM;
                break;
            }
            uint64_t map_flags = VMM_PRESENT | VMM_USER;
            if (prot & 0x2) map_flags |= VMM_WRITE;
            if (!(prot & 0x4) && nx_enabled()) map_flags |= VMM_NX;
            uint64_t mapped_pages = 0;
            for (; mapped_pages < num_pages; mapped_pages++) {
                uintptr_t phys = pmm_alloc_page();
                if (!phys) {
                    ret = -ENOMEM;
                    break;
                }
                auto* zero_page = reinterpret_cast<volatile uint64_t*>(phys + pmm_hhdm_offset());
                for (size_t word = 0; word < PAGE_SIZE / sizeof(uint64_t); ++word)
                    zero_page[word] = 0;
                VmmMapResult map_result = vmm_map_owned_page(root, addr + mapped_pages * 0x1000, phys,
                                                   map_flags);
                if (map_result != VmmMapResult::Success) {
                    pmm_free_page(phys);
                    ret = (map_result == VmmMapResult::PageTableOutOfMemory) ? -ENOMEM : -EINVAL;
                    break;
                }
            }

            if (mapped_pages == num_pages) {
                ret = addr;
            } else {
                while (mapped_pages > 0) {
                    --mapped_pages;
                    if (vmm_unmap_page(root, addr + mapped_pages * PAGE_SIZE) != VmmUnmapResult::Success)
                        panic("mmap rollback failed");
                }
            }
            break;
        }

        case 10: // mprotect
            ret = -ENOSYS;
            break;

        case 11: { // munmap
            uintptr_t addr = frame->rdi;
            uint64_t length = frame->rsi;
            if (!length || (addr & (PAGE_SIZE - 1)) || length > UINT64_MAX - (PAGE_SIZE - 1)) {
                ret = -EINVAL;
                break;
            }
            uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
            constexpr uintptr_t USER_LIMIT = 1ULL << 47;
            if (pages > (UINT64_MAX - addr) / PAGE_SIZE || addr >= USER_LIMIT ||
                pages > (USER_LIMIT - addr) / PAGE_SIZE) { ret = -EINVAL; break; }
            Thread* current = scheduler_get_current_thread();
            uintptr_t root = current && current->parent_process
                ? current->parent_process->page_table_phys : vmm_get_kernel_pml4();
            if (!current || !current->parent_process || !current->parent_process->is_user) {
                ret = -EPERM;
                break;
            }
            // Validate the full range first so an obviously invalid request does not
            // partially remove mappings. Large-page splitting can still fail later.
            bool valid = true;
            for (uint64_t i = 0; i < pages; ++i) {
                VmmMappingInfo mapping = {};
                uintptr_t page = addr + i * PAGE_SIZE;
                if (!vmm_query_mapping(page, &mapping) || !(mapping.flags & VMM_USER) ||
                    mapping.ownership != VmmMappingInfo::Ownership::Owned) {
                    valid = false;
                    break;
                }
            }
            if (!valid) { ret = -EINVAL; break; }
            ret = 0;
            for (uint64_t i = 0; i < pages; ++i) {
                VmmUnmapResult result = vmm_unmap_page(root, addr + i * PAGE_SIZE);
                if (result != VmmUnmapResult::Success) {
                    (void)result;
                    panic("munmap failed after range validation");
                }
            }
            break;
        }

        case 12: // brk
            // If brk is 0, return current "break". For now just return 0 to force malloc to use mmap
            ret = 0;
            break;

        case 4: // stat
        case 5: { // fstat
            // struct stat *buf = frame->rsi;
            // Minimal stub to satisfy libc. 
            // 0x2180 is S_IFCHR, 0x8180 is S_IFREG.
            if (!user_range_access(frame->rsi, 56, true)) { ret = -EFAULT; break; }
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
        {
            char filename[256];
            ret = copy_user_string(frame->rdi, filename, sizeof(filename)) ? 0 : -EFAULT;
            break;
        }

        case 24: // sched_yield
            scheduler_yield();
            ret = 0;
            break;

        case 35: { // nanosleep
            // struct timespec *req = frame->rdi
            if (frame->rdi) {
                if (!user_range_access(frame->rdi, 16, false)) { ret = -EFAULT; break; }
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
                if (!user_range_access(frame->rdi, 390, true)) { ret = -EFAULT; break; }
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
            if (!buf || frame->rsi < 2 || !user_range_access(frame->rdi, 2, true)) {
                ret = -EFAULT;
            } else {
                buf[0] = '/';
                buf[1] = 0;
                ret = 2;
            }
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
                if (!user_range_access(frame->rdi, 16, true)) { ret = -EFAULT; break; }
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
