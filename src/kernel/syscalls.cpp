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
#include "kernel/smp.hpp"
#include "cpu.hpp"
#include <stddef.h>

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

static Process* current_user_process() {
    Thread* current = scheduler_get_current_thread();
    if (!current || !current->parent_process || !current->parent_process->is_user)
        return nullptr;
    return current->parent_process;
}

class UserMappingGuard {
public:
    explicit UserMappingGuard(Process* process) : process_(process) {
        if (process_) process_->lock_user_mappings();
    }
    ~UserMappingGuard() {
        if (process_) process_->unlock_user_mappings();
    }
    UserMappingGuard(const UserMappingGuard&) = delete;
    UserMappingGuard& operator=(const UserMappingGuard&) = delete;
private:
    Process* process_;
};

static VFS::FileObject* acquire_user_file(uint64_t handle, uint32_t rights) {
    if (handle > INT32_MAX) return nullptr;
    Process* process = current_user_process();
    if (!process) return nullptr;
    KernelObject* object = process->handles.acquire(static_cast<int32_t>(handle),
        KernelObjectType::File, rights);
    return static_cast<VFS::FileObject*>(object);
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
    smp_note_syscall();
    int64_t ret = -ENOSYS;
    
    switch (frame->num) {
        case 0: // read (fd, buf, count)
            if (frame->rdx > UINT32_MAX || !user_range_access(frame->rsi, frame->rdx, true)) ret = -EFAULT;
            else if (frame->rdi == 0) ret = 0; // EOF on stdin
            else if (frame->rdi > 2) {
                VFS::FileObject* file = acquire_user_file(frame->rdi, HANDLE_RIGHT_READ);
                if (!file) ret = -EBADF;
                else { ret = file->read((void*)frame->rsi, (uint32_t)frame->rdx); file->release(); }
            } else ret = -EBADF;
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
                VFS::FileObject* file = acquire_user_file(frame->rdi, HANDLE_RIGHT_WRITE);
                if (!file) ret = -EBADF;
                else { ret = file->write((const void*)frame->rsi, (uint32_t)frame->rdx); file->release(); }
            } else {
                ret = -EBADF;
            }
            break;

        case 2: // open (filename, flags, mode)
        {
            char filename[256];
            if (!copy_user_string(frame->rdi, filename, sizeof(filename))) { ret = -EFAULT; break; }
            if (frame->rsi > INT32_MAX) { ret = -EINVAL; break; }
            Process* process = current_user_process();
            if (!process) { ret = -EPERM; break; }
            constexpr uint64_t O_ACCMODE = 3;
            constexpr uint64_t O_CREAT = 0x40;
            uint64_t raw_flags = frame->rsi;
            uint64_t supported = O_ACCMODE | O_CREAT;
            if (raw_flags & ~supported) { ret = -ENOSYS; break; }
            uint64_t access = raw_flags & O_ACCMODE;
            if (access == O_ACCMODE) { ret = -EINVAL; break; }
            int open_flags = access == 0 ? VFS_O_READ :
                             access == 1 ? VFS_O_WRITE : VFS_O_RDWR;
            if (raw_flags & O_CREAT) open_flags |= VFS_O_CREATE;
            VFS::FileObject* file = vfs_open_file(filename, open_flags);
            if (!file) { ret = -ENOENT; break; }
            uint32_t rights = HANDLE_RIGHT_DUPLICATE;
            if (file->open_flags() & VFS_O_READ) rights |= HANDLE_RIGHT_READ;
            if (file->open_flags() & VFS_O_WRITE) rights |= HANDLE_RIGHT_WRITE;
            ret = process->handles.install(file, rights);
            file->release();
            if (ret < 0) ret = -EMFILE;
            break;
        }

        case 3: // close (fd)
            if (frame->rdi > 2 && frame->rdi <= INT32_MAX) {
                Process* process = current_user_process();
                ret = process && process->handles.close(static_cast<int32_t>(frame->rdi)) ? 0 : -EBADF;
            }
            else if (frame->rdi > INT32_MAX) ret = -EBADF;
            else ret = 0; // close stdout/stderr does nothing for now
            break;

        case 8: // lseek (fd, offset, whence)
            if (frame->rdi > 2) {
                VFS::FileObject* file = acquire_user_file(frame->rdi, 0);
                if (!file) ret = -EBADF;
                else {
                    int64_t offset = static_cast<int64_t>(frame->rsi);
                    if (offset < INT32_MIN || offset > INT32_MAX || frame->rdx > INT32_MAX)
                        ret = -EINVAL;
                    else ret = file->seek(static_cast<int32_t>(offset), static_cast<int>(frame->rdx));
                    file->release();
                }
            }
            else ret = -ESPIPE;
            break;

        case 9: { // mmap (addr, length, prot, flags, fd, offset)
            uint64_t addr = frame->rdi;
            uint64_t length = frame->rsi;
            uint64_t prot = frame->rdx;
            uint64_t flags = frame->r10;

            // Only support MAP_ANONYMOUS | MAP_PRIVATE (typically 0x22 in Linux) for now
            if ((flags & 0x20) == 0 || (flags & ~0x22ULL) != 0) {
                ret = -ENOSYS; // File mapping not supported yet
                break;
            }
            if ((frame->r8 != UINT64_MAX && frame->r8 != UINT32_MAX) || frame->r9 != 0) {
                ret = -EINVAL;
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
            if (rounded_length >= USER_LIMIT) { ret = -EINVAL; break; }
            Process* process = current_user_process();
            if (!process) { ret = -EPERM; break; }
            if (addr == 0) {
                uintptr_t next = __atomic_load_n(&process->next_mmap_address, __ATOMIC_RELAXED);
                for (;;) {
                    if (next > USER_LIMIT - rounded_length) { ret = -ENOMEM; break; }
                    uintptr_t following = next + rounded_length;
                    if (__atomic_compare_exchange_n(&process->next_mmap_address, &next, following,
                                                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                        addr = next;
                        break;
                    }
                }
                if (ret == -ENOMEM) break;
            }
            if ((addr & (PAGE_SIZE - 1)) || addr >= USER_LIMIT ||
                rounded_length > USER_LIMIT - addr) {
                ret = -EINVAL;
                break;
            }

            uint8_t permissions = 0;
            if (prot & 0x1) permissions |= VM_PERM_READ;
            if (prot & 0x2) permissions |= VM_PERM_READ | VM_PERM_WRITE;
            if (prot & 0x4) permissions |= VM_PERM_EXECUTE;
            VmRegion region{permissions, VmRegionType::Anonymous, VmRegionSharing::Private, 0,
                            static_cast<uintptr_t>(addr), static_cast<uintptr_t>(addr + rounded_length),
                            0, nullptr};
            UserMappingGuard mapping_guard(process);
            VmRegionResult added = process->vm_regions.insert(region);
            ret = added == VmRegionResult::Success ? static_cast<int64_t>(addr) :
                  (added == VmRegionResult::CapacityExceeded ? -ENOMEM : -EINVAL);
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
            Process* process = current_user_process();
            if (!process) {
                ret = -EPERM;
                break;
            }
            UserMappingGuard mapping_guard(process);
            uintptr_t end = addr + pages * PAGE_SIZE;
            if (!process->vm_regions.covers(addr, end) ||
                process->vm_regions.remove(addr, end) != VmRegionResult::Success) {
                ret = -EINVAL;
                break;
            }
            ret = 0;
            for (uint64_t i = 0; i < pages; ++i) {
                uintptr_t page = addr + i * PAGE_SIZE;
                VmmMappingInfo mapping = {};
                if (!vmm_query_mapping(page, &mapping)) continue;
                if (!(mapping.flags & VMM_USER) ||
                    mapping.ownership != VmmMappingInfo::Ownership::Owned) {
                    panic("anonymous munmap found a non-owned page");
                }
                VmmUnmapResult result = vmm_unmap_page(process->page_table_phys, page);
                if (result != VmmUnmapResult::Success) {
                    panic("munmap failed after region removal");
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
                if (frame->num == 5 && frame->rdi > 2 && frame->rdi <= INT32_MAX) {
                    VFS::FileObject* file = acquire_user_file(frame->rdi, 0);
                    if (!file) { ret = -EBADF; break; }
                    *st_size = file->size();
                    file->release();
                } else if (frame->num == 5 && frame->rdi > INT32_MAX) {
                    ret = -EBADF;
                    break;
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

        case 60: { // exit (status)
            ProcessExitRequestResult result = scheduler_exit_current_process(
                static_cast<int32_t>(static_cast<uint32_t>(frame->rdi)));
            if (result == ProcessExitRequestResult::Success ||
                result == ProcessExitRequestResult::AlreadyTerminating) {
                scheduler_yield();
                panic("Process exit syscall returned to an exiting thread");
            }
            ret = -EPERM;
            break;
        }

        case 61: { // waitpid (pid, status*) -- pid -1 waits for any child
            int64_t requested_pid = static_cast<int64_t>(frame->rdi);
            if (frame->rsi && !user_range_access(frame->rsi, sizeof(int32_t), true)) {
                ret = -EFAULT;
                break;
            }
            Process* parent = current_user_process();
            if (!parent) { ret = -EPERM; break; }
            uint64_t child_pid = 0;
            int32_t status = 0;
            ProcessWaitResult result = process_wait_child(parent, requested_pid,
                &child_pid, frame->rsi ? &status : nullptr);
            switch (result) {
                case ProcessWaitResult::Success:
                    if (frame->rsi) {
                        UserMappingGuard mapping_guard(parent);
                        if (!user_range_access(frame->rsi, sizeof(int32_t), true)) ret = -EFAULT;
                        else {
                            *reinterpret_cast<int32_t*>(frame->rsi) = status;
                            ret = static_cast<int64_t>(child_pid);
                        }
                    } else ret = static_cast<int64_t>(child_pid);
                    break;
                case ProcessWaitResult::NoChild:
                    ret = -ECHILD;
                    break;
                case ProcessWaitResult::InvalidPid:
                    ret = -EINVAL;
                    break;
                case ProcessWaitResult::InvalidParent:
                    ret = -EPERM;
                    break;
                case ProcessWaitResult::SchedulerUnavailable:
                    ret = -EAGAIN;
                    break;
            }
            break;
        }

        case 62: { // terminate (direct-child pid, requested exit status)
            Process* requester = current_user_process();
            if (!requester) { ret = -EPERM; break; }
            ProcessTerminationRequestResult result = process_terminate_child(requester,
                frame->rdi, static_cast<int32_t>(static_cast<uint32_t>(frame->rsi)));
            switch (result) {
                case ProcessTerminationRequestResult::Success:
                case ProcessTerminationRequestResult::AlreadyTerminating:
                    ret = 0;
                    break;
                case ProcessTerminationRequestResult::InvalidPid:
                    ret = -EINVAL;
                    break;
                case ProcessTerminationRequestResult::NoSuchProcess:
                case ProcessTerminationRequestResult::InvalidState:
                    ret = -ESRCH;
                    break;
                case ProcessTerminationRequestResult::InvalidProcess:
                    ret = -EPERM;
                    break;
            }
            break;
        }

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
        case 33: // dup2
            ret = -ENOSYS; // Not implemented yet
            break;

        case 32: { // dup
            Process* process = current_user_process();
            if (!process || frame->rdi > INT32_MAX) ret = -EBADF;
            else {
                ret = process->handles.duplicate(static_cast<int32_t>(frame->rdi));
                if (ret < 0) ret = -EBADF;
            }
            break;
        }

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
    
    Thread* current = scheduler_get_current_thread();
    if (current && current->parent_process && current->parent_process->is_user &&
        (current->parent_process->state() == ProcessState::Terminating ||
         current->parent_process->state() == ProcessState::Exited)) {
        scheduler_terminate_thread(current);
        scheduler_yield();
        panic("Terminating process thread returned from a syscall");
    }
    frame->num = (uint64_t)ret;
}
