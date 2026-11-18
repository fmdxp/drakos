# ELF executable loading

`elf_load_executable` is a reusable loader operation in
`include/kernel/elf_loader.hpp`. It reads a bounded byte range from an existing
`VFS::FileObject`, loads it into a caller supplied user address-space root, and
records the resulting segments in a caller supplied `VmRegionTable`. The
caller continues to own the file, address space, and region table. The loader
does not create a `Process` or `Thread`, enqueue work with the scheduler, or
publish an executable to userspace.

The caller provides the byte range so an ELF image can live in a larger file
container. No DRK package parsing or process-launch integration is performed
by this API. On success, `ElfImageInfo` reports the entry point, page-aligned
image bounds, number of loadable segments, and total segment memory size.
`ElfLoadResult` distinguishes validation, VFS read, memory mapping, region
registration, and rollback failures; it also preserves the underlying VMM or
region-table error where applicable.

## Accepted format

The loader accepts little-endian ELF64 `ET_EXEC` images for x86-64. It checks
the ELF magic, class, data encoding, identification and header versions, type,
machine, header and program-header sizes, program-header table bounds, and
program-header count. It validates each `PT_LOAD` segment's file range,
file and memory sizes, power-of-two alignment and offset/address congruence,
user virtual-address bounds, page-rounded overlap, and permissions. Segments
must be readable; writable and executable flags cannot be combined. The entry
point must lie inside an executable load segment.
Dynamic, interpreter, shared-library, and TLS program headers are rejected
because this milestone does not implement dynamic linking, interpreters, or
thread-local storage. An executable-stack request is also rejected.

All file and address ranges use overflow-checked arithmetic before mapping.
The loader uses `FileObject::read_at` and completes positive short reads; an
error or premature end fails the load. Each mapped page is zero-filled before
file contents are copied, so partial final pages and BSS are initialized.
Mappings are user accessible and carry the segment's write permission. NX is
set on non-executable segments when the CPU reports NX support.

## Failure and rollback

The loader validates all ELF headers and segments before allocating any image
pages. It then tracks each successfully mapped page. If allocation, mapping,
file reading, or VM-region registration fails, it removes any regions inserted
by the operation and unmaps its pages in reverse order. Owned data pages are
released by the existing VMM unmap path. The address-space root remains owned
by the caller. `rollback_error` reports if cleanup itself could not complete;
callers must treat that result as a serious address-space cleanup failure.
`ElfImageInfo` is cleared unless loading succeeds.

## Current limitations

- Only static, fixed-address `ET_EXEC` ELF64 x86-64 images are supported.
- `ET_DYN`, PIE, dynamic linking, relocations, interpreters, and 32-bit ELF are
  unsupported.
- Program-header tables are limited to 128 entries and loadable segments to
  the VM-region table capacity.
- VFS `read_at` currently limits file offsets to the 32-bit filesystem API
  range.
- This operation does not create a process or initial thread and is not wired
  into boot, SSM, or a spawn API.
- NX depends on hardware support; W^X is enforced by rejecting segments that
  request both write and execute permissions.
