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
4. Validate argument/environment arrays and bounded strings, then initialize a
   System V AMD64 process-entry stack with copied strings, pointer arrays,
   null terminators, a null auxiliary vector terminator, and 16-byte aligned
   RSP. The strings and pointers are inside the process's one-page user stack.
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

`user_process_create_from_elf` accepts `UserProcessArguments` with kernel-owned
pointer arrays. It rejects missing arrays or strings, more than 32 arguments
or environment entries, strings longer than 255 bytes plus NUL, and aggregate
string storage above 2048 bytes. Pointers are copied into the user stack; no
kernel pointer is exposed. The caller must provide readable kernel pointers
that remain stable for the duration of the call. Invalid input is rejected
before process/address-space allocation, and stack-construction failures
prevent scheduler publication.

At ELF entry, RSP points to `argc`, followed by `argv[0..argc-1]`,
`argv[argc] == NULL`, `envp[0..envc-1]`, `envp[envc] == NULL`, and the
`AT_NULL, 0` auxiliary-vector terminator. RSP is 16-byte aligned. Argument,
environment, and terminator pointers are user virtual addresses in the stack
page. `userspace/libc/crt0.S` passes these values as `(argc, argv, envp)` to
`main`; existing applications can ignore them. The libc does not yet parse
auxiliary-vector entries or provide a richer process startup runtime.

The built `userspace/args_env_test.elf` validates the entry stack. The QEMU
address-space test uses the same assembly test program in a minimal test ELF,
launches it through this API with two arguments and one environment entry, and
verifies ring-3 code checks, pointer/string contents, stack alignment, a
`getpid` syscall, and controlled invalid-opcode retirement. The kernel test
also checks the exception's CPL, RIP, RSP, process association, and stack
marker. Failure injection still covers ELF validation, thread allocation,
user-stack allocation and mapping, thread initialization, and scheduler
publication.

## Limits

- This is a kernel-side preparation API; it is not exposed as a syscall and is
  not invoked during normal boot.
- The launcher remains a kernel API; normal boot does not start a userspace
  process, and no syscall exposes process creation.
- The one-page initial stack and fixed count/string/storage limits are
  intentionally bounded. Auxiliary-vector data currently contains only
  `AT_NULL`.
- The test program terminates through the existing fault-retirement test path;
  normal process exit semantics remain future work.
- ELF format limits are documented in [elf-loading.md](elf-loading.md).
