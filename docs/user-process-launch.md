# Initial user-thread preparation

`user_process_create_from_elf` in `include/kernel/user_process.hpp` connects
the process lifecycle, ELF loader, user stack, thread context, and scheduler.
The caller supplies an already-open `VFS::FileObject` and an executable byte
range. The helper keeps the file caller-owned and returns a `UserProcessImage`
containing the process's creator reference, entry point, initial stack pointer,
and image metadata.

The preparation order is:

1. Create an unpublished user `Process` with its own address space.
2. Load and validate the ELF image into that address space and register its
   VM regions.
3. Allocate the initial user thread, kernel stack, and one-page user stack.
4. Initialize an empty System V AMD64 process-entry stack with 16-byte aligned
   RSP, `argc = 0`, null `argv` and `envp` terminators, and a null auxiliary
   vector terminator. Stack memory is zeroed before writing the initial words.
5. Validate the initial context and publish the fully initialized thread to
   the scheduler.

The thread context uses the existing `iretq` path with user CS `0x23`, user SS
`0x1b`, IF enabled, and IOPL clear. Its RIP is the validated ELF entry point.
The scheduler API now returns `SchedulerPublishResult`; it retains the
address-space and process thread references before adding a thread to the
shared run queue. User-thread initialization reports a `ThreadInitResult`,
including separate stack allocation, map, and VM-region failures.

On failure, no thread is published. The helper destroys the unpublished
process, which reclaims ELF mappings, registered image regions, and any stack
resource already attached. The supplied file remains caller-owned. Cleanup
failures are treated as kernel errors instead of being silently ignored.

The QEMU address-space test constructs a minimal ELF image in a test-only VFS
file, launches it through this same API, and verifies ring-3 execution. The
user program checks `argc`, saves its stack pointer, calls `getpid` through
`SYSCALL`, stores the returned PID, and raises a controlled invalid-opcode
exception. The kernel test checks the exception's CPL, RIP, RSP, process
association, stack marker, and PID, then verifies scheduler retirement and
deferred process cleanup. Failure injection covers ELF validation, thread
allocation, user-stack allocation and mapping, thread initialization, and
scheduler publication.

## Limits

- This is a kernel-side preparation API; it is not exposed as a syscall and is
  not invoked during normal boot.
- No arguments, environment variables, startup runtime, or initial userspace
  process are provided.
- The test program terminates through the existing fault-retirement test path;
  normal process exit semantics remain future work.
- ELF format limits are documented in [elf-loading.md](elf-loading.md).
