# Process lifecycle and creation ownership

## Scope

This documents user-process creation, execution, process-wide exit,
parent-child relationships, and wait/reap. External process termination,
userspace spawning, and an initial userspace process are not provided.

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

There is no Reaped state or global process registry. Exited is terminal; a
successful wait removes the parent-child relationship while the Process object
may remain alive for other outstanding references. Kernel worker processes use
the kernel address space as a borrowed root and start Ready.

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
reclamation finishes. Publishing Exited notifies an attached parent and wakes
its waiters.

## Parent-child ownership and wait/reap

`process_attach_child(parent, child)` establishes a relationship only between
distinct user processes in valid lifecycle states. A parent owns each child
link, and each link holds one child reference so an exited child remains
available for status collection. The child's back-reference to its parent is
weak; this avoids a reference cycle. Relationship mutation and lookup are
serialized by the process relationship lock. PIDs increase monotonically and
the allocator stops rather than wrapping, so a relation cannot accidentally
refer to a later process after PID reuse.

`user_process_create_from_elf` accepts an optional parent Process. The caller
still receives its creator reference. Kernel-created processes may omit a
parent. This milestone does not add a spawn syscall.

The userspace syscall ABI assigns number 61 to `waitpid(pid, status*)`:

- A positive PID waits for that direct child.
- `-1` waits for any direct child.
- A null status pointer is allowed; otherwise it must reference writable
  userspace memory.
- Success returns the reaped child PID and, when requested, stores its signed
  exit status in an `int32_t`.
- Errors return negative `EINVAL` for PID 0 or values below -1, `ECHILD` when
  there is no matching unreaped child, `EFAULT` for an invalid status pointer,
  `EPERM` outside a user process, and `EAGAIN` if the current thread cannot be
  blocked by the scheduler.

The kernel checks child state and prepares the current thread to block while
holding the relationship lock. A scheduler handoff state latches a wake that
arrives before switch-out completes, closing the condition-check/sleep race
without polling. Child exit and successful reaping wake all threads waiting for
that parent; each waiter rechecks the child list under lock. Only one waiter
can unlink and collect a child. Reaping takes a temporary child reference
before dropping the parent's link reference, then releases it after copying
out PID and status. The status pointer is checked before waiting and checked
again while holding the process mapping-operation lock before copyout; mmap and
munmap use that same lock so a sibling thread cannot remove the output mapping
during the final copy.

If a parent reaches Exited before its children, cleanup clears their weak
parent back-references and releases the parent's child-link references. The
children become orphans and continue running; no userspace init/reaper adopts
them. They do not leak solely because of an abandoned relationship, although
other owners (including a process-creation caller reference) must still release
their references. A future init process may add adoption policy.

The relationship lock serializes concurrent exits, waiters, and reaps on SMP
and is never held while a parent blocks. This does not add wait options,
signaling, global PID lookup, or process spawning.

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
terminal process cleanup. A parent relationship adds one child reference; the
weak parent pointer adds none. Successful wait or parent orphan cleanup
releases that relationship reference.

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

Host tests use the Process factory and VMM implementation to verify relationship
registration, invalid/duplicate links, exit-before-wait collection, status
preservation, one-time reaping, and orphan detachment. Four-CPU QEMU tests
exercise the userspace wait syscall, invalid status pointers, concurrent parent
waiters, blocked wakeup, multiple child exits, single reaping, status copyout,
and a parent exiting while a live orphan continues to run.

Direct `scheduler_terminate_thread` still terminates one thread only; the
userspace `exit` syscall is process-wide. Dynamic ELF loading and
command-line/environment construction exist, while userspace spawn and SSM
integration remain later milestones.
