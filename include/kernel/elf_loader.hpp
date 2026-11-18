#pragma once

#include <stdint.h>

#include "kernel/vm_region.hpp"
#include "memory/vmm.hpp"

namespace VFS { class FileObject; }

enum class ElfLoadError : uint8_t {
    Success,
    InvalidArgument,
    FileRangeInvalid,
    ReadError,
    ShortRead,
    InvalidMagic,
    UnsupportedClass,
    UnsupportedEndianness,
    UnsupportedVersion,
    UnsupportedType,
    UnsupportedMachine,
    InvalidHeader,
    InvalidProgramHeaders,
    UnsupportedProgramHeader,
    InvalidSegment,
    InvalidAlignment,
    InvalidAddress,
    InvalidEntryPoint,
    OverlappingSegments,
    TooManySegments,
    OutOfMemory,
    MappingFailed,
    RegionFailed,
    RollbackFailed,
};

struct ElfImageInfo {
    uintptr_t entry_point;
    uintptr_t image_start;
    uintptr_t image_end;
    uint32_t load_segment_count;
    uint64_t mapped_memory_size;
};

struct ElfLoadResult {
    ElfLoadError error;
    ElfLoadError rollback_error;
    VmmMapResult mapping_error;
    VmRegionResult region_error;
};

// Load an ELF64 little-endian x86-64 ET_EXEC image from the specified file
// range into an existing user address space. The caller retains ownership of
// both the FileObject and address space. On failure, mappings and VM regions
// created by this call are rolled back.
ElfLoadResult elf_load_executable(VFS::FileObject& file,
                                  uint64_t file_offset,
                                  uint64_t file_size,
                                  uintptr_t address_space_root,
                                  VmRegionTable& regions,
                                  ElfImageInfo* image_info);
