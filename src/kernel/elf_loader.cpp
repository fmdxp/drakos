#include "kernel/elf_loader.hpp"

#include "elf.hpp"
#include "cpu.hpp"
#include "fs/vfs.hpp"
#include "memory/pmm.hpp"

namespace {
constexpr uint64_t kUserLimit = 1ULL << 47;
constexpr uint64_t kPageMask = PAGE_SIZE - 1;
constexpr uint16_t kMaxProgramHeaders = 128;
constexpr uint16_t kMaxLoadSegments = VmRegionTable::Capacity;

struct LoadSegment {
    uint64_t virtual_address;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t page_start;
    uint64_t page_end;
    uint32_t flags;
};

bool add_overflow(uint64_t left, uint64_t right, uint64_t* result) {
    if (right > UINT64_MAX - left) return true;
    *result = left + right;
    return false;
}

bool range_fits(uint64_t offset, uint64_t length, uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

ElfLoadResult failure(ElfLoadError error) {
    return {error, ElfLoadError::Success, VmmMapResult::Success, VmRegionResult::Success};
}

bool read_exact(VFS::FileObject& file, uint64_t offset, void* buffer,
                uint32_t size, ElfLoadResult* result) {
    auto* output = static_cast<uint8_t*>(buffer);
    uint32_t completed = 0;
    while (completed < size) {
        int count = file.read_at(offset + completed, output + completed, size - completed);
        if (count < 0) {
            result->error = ElfLoadError::ReadError;
            return false;
        }
        if (count == 0) {
            result->error = ElfLoadError::ShortRead;
            return false;
        }
        completed += static_cast<uint32_t>(count);
    }
    return true;
}

bool rollback(VmRegionTable& regions, uintptr_t root,
              const LoadSegment* segments, uint16_t segment_count,
              const uint32_t* mapped_pages, uint16_t regions_added,
              ElfLoadResult* result) {
    bool clean = true;
    for (uint16_t i = regions_added; i > 0; --i) {
        const LoadSegment& segment = segments[i - 1];
        if (regions.remove(static_cast<uintptr_t>(segment.page_start),
                           static_cast<uintptr_t>(segment.page_end)) != VmRegionResult::Success)
            clean = false;
    }
    for (uint16_t i = segment_count; i > 0; --i) {
        const LoadSegment& segment = segments[i - 1];
        uint64_t page_count = (segment.page_end - segment.page_start) / PAGE_SIZE;
        uint32_t count = mapped_pages[i - 1];
        if (count > page_count) count = static_cast<uint32_t>(page_count);
        for (uint32_t page = count; page > 0; --page) {
            uintptr_t address = static_cast<uintptr_t>(segment.page_start +
                                                       uint64_t(page - 1) * PAGE_SIZE);
            if (vmm_unmap_page(root, address) != VmmUnmapResult::Success) clean = false;
        }
    }
    if (!clean) result->rollback_error = ElfLoadError::RollbackFailed;
    return clean;
}
}

ElfLoadResult elf_load_executable(VFS::FileObject& file,
                                  uint64_t file_offset,
                                  uint64_t file_size,
                                  uintptr_t address_space_root,
                                  VmRegionTable& regions,
                                  ElfImageInfo* image_info) {
    if (image_info) *image_info = {};
    ElfLoadResult result = failure(ElfLoadError::InvalidArgument);
    if (!image_info || !address_space_root || file_size < sizeof(Elf64_Ehdr)) return result;

    uint64_t file_end = 0;
    if (add_overflow(file_offset, file_size, &file_end) ||
        file_end > file.size() || file_end > UINT32_MAX)
        return failure(ElfLoadError::FileRangeInvalid);

    if (vmm_retain_address_space(address_space_root) != VmmAddressSpaceResult::Success)
        return failure(ElfLoadError::InvalidArgument);
    if (vmm_release_address_space(address_space_root) != VmmAddressSpaceResult::Success)
        return failure(ElfLoadError::RollbackFailed);

    Elf64_Ehdr header{};
    result = failure(ElfLoadError::Success);
    if (!read_exact(file, file_offset, &header, sizeof(header), &result)) return result;
    if (header.e_ident[0] != 0x7F || header.e_ident[1] != 'E' ||
        header.e_ident[2] != 'L' || header.e_ident[3] != 'F')
        return failure(ElfLoadError::InvalidMagic);
    if (header.e_ident[EI_CLASS] != ELFCLASS64)
        return failure(ElfLoadError::UnsupportedClass);
    if (header.e_ident[EI_DATA] != ELFDATA2LSB)
        return failure(ElfLoadError::UnsupportedEndianness);
    if (header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT)
        return failure(ElfLoadError::UnsupportedVersion);
    if (header.e_type != ET_EXEC) return failure(ElfLoadError::UnsupportedType);
    if (header.e_machine != EM_X86_64) return failure(ElfLoadError::UnsupportedMachine);
    if (header.e_flags != 0) return failure(ElfLoadError::InvalidHeader);
    if (header.e_ehsize != sizeof(Elf64_Ehdr) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum ||
        header.e_phnum > kMaxProgramHeaders || header.e_phoff < sizeof(Elf64_Ehdr))
        return failure(ElfLoadError::InvalidHeader);

    uint64_t program_table_size = uint64_t(header.e_phnum) * sizeof(Elf64_Phdr);
    if (!range_fits(header.e_phoff, program_table_size, file_size))
        return failure(ElfLoadError::InvalidProgramHeaders);

    LoadSegment segments[kMaxLoadSegments]{};
    uint16_t segment_count = 0;
    bool entry_is_executable = false;
    uint64_t image_start = UINT64_MAX;
    uint64_t image_end = 0;
    uint64_t total_memory = 0;

    for (uint16_t i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr program{};
        uint64_t relative = header.e_phoff + uint64_t(i) * sizeof(program);
        if (!read_exact(file, file_offset + relative, &program, sizeof(program), &result))
            return result;
        if (program.p_type == PT_DYNAMIC || program.p_type == PT_INTERP ||
            program.p_type == PT_SHLIB || program.p_type == PT_TLS)
            return failure(ElfLoadError::UnsupportedProgramHeader);
        if (program.p_type == PT_GNU_STACK && (program.p_flags & PF_X))
            return failure(ElfLoadError::UnsupportedProgramHeader);
        if (program.p_type != PT_LOAD) continue;
        if (segment_count == kMaxLoadSegments) return failure(ElfLoadError::TooManySegments);
        if (program.p_flags & ~(PF_R | PF_W | PF_X) || !(program.p_flags & PF_R) ||
            ((program.p_flags & PF_W) && (program.p_flags & PF_X)) ||
            program.p_filesz > program.p_memsz)
            return failure(ElfLoadError::InvalidSegment);
        if (!program.p_memsz) {
            if (program.p_filesz) return failure(ElfLoadError::InvalidSegment);
            continue;
        }
        if (!range_fits(program.p_offset, program.p_filesz, file_size))
            return failure(ElfLoadError::InvalidSegment);
        if (program.p_align > 1 &&
            ((program.p_align & (program.p_align - 1)) ||
             (program.p_vaddr & (program.p_align - 1)) !=
                 (program.p_offset & (program.p_align - 1))))
            return failure(ElfLoadError::InvalidAlignment);

        uint64_t memory_end = 0;
        uint64_t page_end_unrounded = 0;
        if (add_overflow(program.p_vaddr, program.p_memsz, &memory_end) ||
            add_overflow(memory_end, kPageMask, &page_end_unrounded))
            return failure(ElfLoadError::InvalidAddress);
        uint64_t page_start = program.p_vaddr & ~kPageMask;
        uint64_t page_end = page_end_unrounded & ~kPageMask;
        if (page_start < PAGE_SIZE || page_end <= page_start || page_end > kUserLimit)
            return failure(ElfLoadError::InvalidAddress);
        if (program.p_offset > UINT32_MAX - file_offset ||
            add_overflow(total_memory, program.p_memsz, &total_memory))
            return failure(ElfLoadError::InvalidSegment);

        for (uint16_t prior = 0; prior < segment_count; ++prior)
            if (page_start < segments[prior].page_end &&
                segments[prior].page_start < page_end)
                return failure(ElfLoadError::OverlappingSegments);

        LoadSegment& segment = segments[segment_count++];
        segment.virtual_address = program.p_vaddr;
        segment.file_offset = program.p_offset;
        segment.file_size = program.p_filesz;
        segment.memory_size = program.p_memsz;
        segment.page_start = page_start;
        segment.page_end = page_end;
        segment.flags = program.p_flags;
        if (page_start < image_start) image_start = page_start;
        if (page_end > image_end) image_end = page_end;
        if ((program.p_flags & PF_X) && header.e_entry >= program.p_vaddr &&
            header.e_entry < memory_end)
            entry_is_executable = true;
    }

    if (!segment_count || !entry_is_executable) return failure(ElfLoadError::InvalidEntryPoint);

    uint32_t mapped_pages[kMaxLoadSegments]{};
    for (uint16_t i = 0; i < segment_count; ++i) {
        const LoadSegment& segment = segments[i];
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (segment.flags & PF_W) flags |= VMM_WRITE;
        if (!(segment.flags & PF_X) && nx_enabled()) flags |= VMM_NX;
        uint64_t page_count = (segment.page_end - segment.page_start) / PAGE_SIZE;
        if (page_count > UINT32_MAX) {
            result.error = ElfLoadError::OutOfMemory;
            rollback(regions, address_space_root, segments, segment_count, mapped_pages, 0, &result);
            return result;
        }
        for (uint64_t page = 0; page < page_count; ++page) {
            uintptr_t physical = pmm_alloc_page();
            if (!physical) {
                result.error = ElfLoadError::OutOfMemory;
                rollback(regions, address_space_root, segments, segment_count, mapped_pages, 0, &result);
                return result;
            }
            uintptr_t virtual_page = static_cast<uintptr_t>(segment.page_start + page * PAGE_SIZE);
            VmmMapResult mapped = vmm_map_owned_page(address_space_root, virtual_page,
                                                     physical, flags);
            if (mapped != VmmMapResult::Success) {
                pmm_free_page(physical);
                result.error = ElfLoadError::MappingFailed;
                result.mapping_error = mapped;
                rollback(regions, address_space_root, segments, segment_count, mapped_pages, 0, &result);
                return result;
            }
            ++mapped_pages[i];
            auto* bytes = reinterpret_cast<uint8_t*>(physical + pmm_hhdm_offset());
            for (size_t byte = 0; byte < PAGE_SIZE; ++byte) bytes[byte] = 0;

            uint64_t page_begin = virtual_page;
            uint64_t page_end = page_begin + PAGE_SIZE;
            uint64_t copy_begin = segment.virtual_address > page_begin
                ? segment.virtual_address : page_begin;
            uint64_t segment_file_end = segment.virtual_address + segment.file_size;
            uint64_t copy_end = segment_file_end < page_end ? segment_file_end : page_end;
            if (copy_begin < copy_end) {
                uint64_t source_offset = segment.file_offset + (copy_begin - segment.virtual_address);
                uint32_t copy_size = static_cast<uint32_t>(copy_end - copy_begin);
                if (!read_exact(file, file_offset + source_offset,
                                bytes + (copy_begin - page_begin), copy_size, &result)) {
                    rollback(regions, address_space_root, segments, segment_count,
                             mapped_pages, 0, &result);
                    return result;
                }
            }
        }
    }

    uint16_t regions_added = 0;
    for (uint16_t i = 0; i < segment_count; ++i) {
        const LoadSegment& segment = segments[i];
        uint8_t permissions = VM_PERM_READ;
        if (segment.flags & PF_W) permissions |= VM_PERM_WRITE;
        if (segment.flags & PF_X) permissions |= VM_PERM_EXECUTE;
        VmRegion region{permissions, VmRegionType::Anonymous, VmRegionSharing::Private,
                        0, static_cast<uintptr_t>(segment.page_start),
                        static_cast<uintptr_t>(segment.page_end), 0, nullptr};
        VmRegionResult inserted = regions.insert(region);
        if (inserted != VmRegionResult::Success) {
            result.error = ElfLoadError::RegionFailed;
            result.region_error = inserted;
            rollback(regions, address_space_root, segments, segment_count,
                     mapped_pages, regions_added, &result);
            return result;
        }
        ++regions_added;
    }

    image_info->entry_point = static_cast<uintptr_t>(header.e_entry);
    image_info->image_start = static_cast<uintptr_t>(image_start);
    image_info->image_end = static_cast<uintptr_t>(image_end);
    image_info->load_segment_count = segment_count;
    image_info->mapped_memory_size = total_memory;
    result.error = ElfLoadError::Success;
    return result;
}
