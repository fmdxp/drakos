# drakos System Service Manager

## Current status

The current implementation is a userspace-consumable, freestanding C++ policy
core. It parses service definitions, validates dependencies, computes a
deterministic startup order, gates startup on required dependencies, validates
lifecycle transitions, and calculates bounded restart decisions and delays.
It does not start or supervise processes. No kernel policy has been added.

The runtime blockers found during the architecture review are:

- The syscall interface has no userspace process spawn or exec operation. The
  existing ELF loader is called from kernel code and is not a userspace API.
- The normal boot path does not create an initial userspace process. The
  kernel-side `DrkLoader` path is not enabled in `make_threads_and_processes()`.
- Process termination does not preserve an exit status for a parent to collect;
  there is no wait/reap or child-exit notification API.
- There is no userspace process termination request API for controlled stop or
  shutdown timeouts.
- No IPC channel or service-discovery interface is available to userspace.
- Filesystem availability depends on device initialization and mounts; there is
  no defined guaranteed service-configuration location at boot.

As a result, configuration, ordering, and policy tests are host tests. They do
not establish process management or service runtime behavior in the kernel.

## Version 1 configuration

Configuration is line based. Blank lines and lines whose first non-space
character is `#` are ignored. Values may be unquoted or double-quoted; quoted
values support `\\` and `\"` escapes. The parser has fixed capacities
documented in `userspace/ssm/service_manager.hpp` and rejects unknown keys,
unsupported versions, malformed values, missing required services, and
over-capacity definitions.

```ini
version = 1

[service storage]
name = "Storage Service"
exec = /system/services/storage.drk
restart = on-failure
restart-limit = 5
restart-backoff-ms = 250
restart-backoff-max-ms = 30000
startup-timeout-ms = 5000
shutdown-timeout-ms = 3000

[service logger]
name = "System Logger"
exec = /system/services/logger.drk
arg = --system
env = LOG_LEVEL=info
restart = always

[service shell]
name = "System Shell"
exec = /system/services/shell.drk
depends = !storage
depends = ?logger
optional = true
restart = never
```

`depends = !id` declares a required dependency; `depends = ?id` declares an
optional dependency. Required dependencies must exist in the configuration and
must be in `Running` state before the dependent service is eligible to start.
A required dependency in `Failed` or `Stopped` state blocks the dependent.
Optional dependencies do not gate startup; present optional dependencies are
still ordered before the dependent in the static startup plan. A dependency
cycle is rejected. The plan is deterministic and preserves registration order
among services whose dependencies permit them to start.

Restart policies are `never`, `on-failure`, and `always`. Intentional stops do
not request a restart. `restart-limit` bounds retries and the delay doubles
from `restart-backoff-ms` up to `restart-backoff-max-ms`. The policy helpers
only calculate decisions; a runtime must provide timers and process exit
events before these rules can supervise a real process.

## Lifecycle policy

Allowed transitions are validated by the policy core:

```text
Registered -> WaitingForDependencies -> Starting -> Running
      |                 |                |          |
      +-> Stopped       +-> Failed       +-> Failed  +-> Stopping
                                                        |
                                                     Stopped
```

`Failed` and `Stopped` records may return to `Starting` for an explicit retry.
Illegal transitions are rejected. This state model currently has no event loop,
timeouts, process references, or asynchronous shutdown implementation.

## Build and tests

Run the host-side parser and policy tests with:

```sh
make test-ssm
```

The tests cover parsing, duplicate identifiers, missing required dependencies,
dependency ordering and cycles, required-dependency startup gating, lifecycle
transitions, restart policy, retry limits, and capped backoff. They do not
simulate or claim kernel process execution. The SSM source can also be compiled
with the project's freestanding cross-compiler flags; it is not linked into a
bootable SSM executable yet.

## Incremental runtime prerequisites

The safe implementation order is:

1. Add a kernel-supported initial userspace process and a userspace spawn API
   with validated executable path, arguments, and environment.
2. Retain process exit status and add wait/reap plus child-exit notification;
   define process references and cleanup ownership.
3. Add controlled process termination and timeout semantics.
4. Establish a boot-time service configuration source that is available before
   services depending on mounted storage are launched.
5. Add a minimal userspace IPC primitive and service discovery/status protocol.
6. Build the SSM executable on the policy core, integrate it as the initial
   userspace process, then add QEMU tests for multiple processes, dependencies,
   failure detection/restart, shutdown, and resource cleanup.

No ADR or architecture-document directory existed when this milestone began;
this document records the current SSM design and blockers. The roadmap tracks
the remaining kernel and runtime work.
