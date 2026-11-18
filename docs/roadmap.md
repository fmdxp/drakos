# drakos development roadmap

This roadmap records implementation dependencies observed in the current tree.
It does not imply that listed future capabilities already work.

## Userspace process-launching foundation

- **Milestone 1 complete:** process lifecycle, creator/thread/deferred-cleanup
  references, explicit user address-space creation, and transactional process
  initialization are documented in
  [process-lifecycle.md](process-lifecycle.md).
- **Milestone 2 complete:** reusable ELF64 `ET_EXEC` validation and loading
  into a caller-owned address space is implemented and host-tested. The loader
  does not create processes or threads; see [elf-loading.md](elf-loading.md).
- **Milestone 3 complete:** `user_process_create_from_elf` prepares a process,
  image, initial ABI stack, and ring-3 thread before scheduler publication.
  QEMU verifies user instructions, stack state, a `getpid` syscall, failure
  rollback, and deferred cleanup; normal boot still starts no user process.
  See [user-process-launch.md](user-process-launch.md).
- **Milestone 4 complete:** bounded arguments and environment are copied to a
  System V AMD64 initial stack; libc startup passes `(argc, argv, envp)` to
  `main`; host and QEMU tests check the values. See
  [user-process-launch.md](user-process-launch.md).
- **Milestones 5–9 not started:** exit and wait/reap semantics,
  parent-child management, initial userspace
  boot, SSM runtime integration, and later launch features remain unimplemented.

## System services

The SSM host-side policy core is implemented in `userspace/ssm/`; service
configuration and dependency/lifecycle/restart policy have focused tests via
`make test-ssm`. A running SSM is not complete. The following work is required
in order:

1. Start a first userspace process during boot and provide a safe userspace
   process creation API with arguments and environment.
2. Add process references, wait/reap, retained exit status, and child exit
   notification so userspace can supervise child processes.
3. Add controlled termination and shutdown timeout behavior.
4. Define a boot-available service configuration source.
5. Add userspace IPC and service discovery/status messages.
6. Integrate the SSM executable and verify multiple independent services,
   dependency startup/failure propagation, restart policy, controlled shutdown,
   and cleanup in QEMU.

See [system-service-manager.md](system-service-manager.md) for the current
architecture, config format, test scope, and explicit limitations.

## Other documented memory and userspace limits

- `mprotect`, file-backed VM fault resolution, and Copy-on-Write are not
  implemented.
- A permission-controlled shared-memory API is not implemented.
- NX enforcement is unavailable on CPUs without NX support.
- The current user isolation boundary is an early kernel foundation, not a
  complete security model.
