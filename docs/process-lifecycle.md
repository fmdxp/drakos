# Process lifecycle and creation ownership

## Scope

This documents user-process creation, execution, and process-wide exit. It does
not provide wait/reap, parent-child relationships, external process termination,
or userspace spawning.

## Lifecycle

`ProcessState` transitions are atomic compare-and-swap operations:

```text
Created -> Initializing -> Ready -> Running -> Terminating -> Exited
                            |                     ^
                            +---------------------+
```

- **Created:** private user-process allocation before resource initialization.
- **Initializing:** the process transaction is acquiring its user address
  space. It is not returned to the caller or eligible for scheduler enrollment.
- **Ready:** process metadata and its owned user address space are initialized.
  A Ready process can exist with zero threads.
- **Running:** the scheduler has selected one of its threads.
- **Terminating:** an explicit userspace exit has requested process-wide
  termination, its final enrolled thread has been retired, or its final
  external reference is being released while it is still empty and Ready.
- **Exited:** owned address space, handles, and VM-region metadata have been
  cleaned. The process object can remain alive while references remain.

There is no Reaped state or process registry yet. Exited is terminal; wait and
reap semantics belong to a later milestone. Kernel worker processes use the
kernel address space as a borrowed root and start Ready.

## Process exit

The `exit` syscall stores its low 32-bit argument as a signed `int32_t` process
exit status. The first exit request wins if multiple threads call `exit`
concurrently. Processes terminated without an explicit status retain the
default status `0`; `has_explicit_exit_status()` distinguishes that default
from an explicitly supplied zero. The status remains available while the
Exited Process object is retained.

The scheduler serializes process exit with thread publication and selection
under its run-queue lock. It changes Ready/Running to Terminating, marks every
enrolled thread in that process dead, and prevents future publication into the
terminating process. It sends reschedule IPIs to other CPUs currently running
its threads. Threads already inside a syscall are stopped before returning to
userspace by the syscall return check; a thread already in userspace is
preempted by its reschedule IPI. Other processes continue to schedule.

The final retired thread queues deferred process cleanup. Only after no thread
can be running in the address space does the scheduler destroy process-owned
address-space resources, close handles, clear VM-region metadata, transition
Terminating to Exited, and release its cleanup reference. Thread objects and
the Process remain protected by their existing references until deferred
reclamation finishes. No parent notification or status collection syscall is
provided yet.

## Creation transaction

`Process::create_user` is the creation transaction. It allocates an unpublished
Process object, enters Initializing, creates a VMM address space, attaches the
owned root, initializes its process-owned handle and VM-region tables, and only
then transitions to Ready and returns the creator reference. Process-owned
tables are fixed-capacity members, so their constructors do not allocate or
fail. The address-space allocation is the current fallible resource step.

On allocation failure, the output remains null and the Process object is
released. The VMM address-space creator rolls back its root page if registry
registration fails. A failure while committing the Ready state destroys the
new root before releasing the object; failure to roll back is fatal rather
than silently leaking the root.

The default `Process` constructor is for kernel processes and borrows the
kernel page-table root. User processes must use `Process::create_user`; a
thread constructor no longer creates a process address space implicitly.

## Ownership and cleanup

`Process::create_user` returns one creator reference. `retain` and `release`
manage intrusive atomic references. Each `Thread` retains its containing
Process for its entire object lifetime. Since the scheduler owns an enrolled
Thread until it has safely left every CPU's current/pending context, this
Thread reference keeps the Process alive across scheduling and deferred thread
reclamation. The scheduler also takes an explicit reference when queuing
terminal process cleanup.

The scheduler thread count is protected by the scheduler lock. A process is
not destroyed while that count is nonzero. Each enrolled user thread also
retains its VMM address space; the scheduler releases that VMM reference only
after the thread is no longer executing. When the last thread is retired, the
scheduler transitions the Process to Terminating and tries to destroy its
owned root. Busy or active-root failures are retried through the existing
deferred cleanup queue. Once the root is destroyed, the process closes its
handle table, clears VM-region references, and becomes Exited. The kernel root
is never freed by Process cleanup.

The final `release` uses a temporary destroying reference value to prevent
concurrent retains while it cleans an empty Ready process. If cleanup fails,
the reference is restored and an explicit error is returned so the caller can
retry. A Process object is deleted only after it is Exited and its last
reference is released.

## Thread and scheduler integration

User-thread construction now requires a Ready or Running user Process with an
owned address space. It separately allocates its kernel stack and one user
stack page, reserves a non-overlapping user stack slot atomically, maps the
page, and registers the VM region. A failed mapping or metadata insertion
leaves the thread unschedulable; rollback failure is fatal. Scheduler
enrollment checks process readiness while holding the run-queue lock and
rejects terminal processes. The scheduler marks a process Running only when
selecting one of its threads.

No allocation or blocking wait was added to scheduler critical sections.
Reference changes are atomic; VMM destruction and handle/VM-region cleanup
remain outside the run-queue lock.

## Testing and limitations

Host tests use the actual Process factory and VMM implementation to verify
empty-process creation/destruction, lifecycle transitions, reference behavior,
address-space ownership, and rollback after injected address-space registry
allocation failure. The existing four-CPU QEMU tests continue to exercise
synthetic user threads, address-space isolation, deferred cleanup, and SMP
scheduling; they do not load ELF processes.

The Process record retains exit status but has no waitable parent relationship,
PID lookup/registry, or reaping API. Direct `scheduler_terminate_thread` still
terminates one thread only; the userspace `exit` syscall is process-wide.
Dynamic ELF loading, command-line/environment construction, userspace spawn,
and SSM integration remain later milestones.
