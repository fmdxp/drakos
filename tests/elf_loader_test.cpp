#include "kernel/elf_loader.hpp"
#include "kernel/elf.hpp"
#include "fs/vfs.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>

void vmm_test_set_nx_enabled(bool enabled);

namespace {
constexpr uint64_t kPayloadBase = 0x80;
constexpr uint64_t kPayloadSize = 0x5000;
constexpr uintptr_t kCodeAddress = 0x00400000;
constexpr uintptr_t kDataAddress = 0x00800000;
constexpr uintptr_t kReadOnlyAddress = 0x01000000;

struct SegmentSpec {
    uint64_t offset;
    uint64_t address;
    uint64_t file_size;
    uint64_t memory_size;
    uint32_t flags;
    uint64_t alignment;
};

class MemoryFs final : public VFS::FileSystem {
public:
    std::vector<uint8_t> contents;
    uint64_t fail_reads_at = UINT64_MAX;
    uint64_t eof_reads_at = UINT64_MAX;
    uint32_t max_read = UINT32_MAX;

    VFS::Node* lookup(const char*) override { return nullptr; }
    VFS::Node* create(const char*, bool) override { return nullptr; }
    int read(VFS::Node*, void* output, uint32_t offset, uint32_t size) override {
        if (offset >= fail_reads_at) return -1;
        if (offset >= eof_reads_at) return 0;
        if (offset >= contents.size()) return 0;
        uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(size, contents.size() - offset));
        count = std::min(count, max_read);
        if (offset < fail_reads_at) count = static_cast<uint32_t>(
            std::min<uint64_t>(count, fail_reads_at - offset));
        if (offset < eof_reads_at) count = static_cast<uint32_t>(
            std::min<uint64_t>(count, eof_reads_at - offset));
        if (!count) return -1;
        std::memcpy(output, contents.data() + offset, count);
        return static_cast<int>(count);
    }
    int write(VFS::Node*, const void*, uint32_t, uint32_t) override { return -1; }
    bool listdir(VFS::Node*, VFS::DirEntry*, uint32_t, uint32_t& count) override {
        count = 0;
        return true;
    }
    void release(VFS::Node* node) override { delete node; }
};

static bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: ELF loader: %s\n", message);
    return condition;
}

static std::vector<uint8_t> make_image(const SegmentSpec* segments, uint16_t count) {
    std::vector<uint8_t> bytes(kPayloadBase + kPayloadSize, 0xCC);
    auto* header = reinterpret_cast<Elf64_Ehdr*>(bytes.data() + kPayloadBase);
    header->e_ident[0] = 0x7F;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_EXEC;
    header->e_machine = EM_X86_64;
    header->e_version = EV_CURRENT;
    header->e_entry = kCodeAddress;
    header->e_flags = 0;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_ehsize = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = count;

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(
        bytes.data() + kPayloadBase + sizeof(Elf64_Ehdr));
    for (uint16_t i = 0; i < count; ++i) {
        phdrs[i].p_type = PT_LOAD;
        phdrs[i].p_flags = segments[i].flags;
        phdrs[i].p_offset = segments[i].offset;
        phdrs[i].p_vaddr = segments[i].address;
        phdrs[i].p_filesz = segments[i].file_size;
        phdrs[i].p_memsz = segments[i].memory_size;
        phdrs[i].p_align = segments[i].alignment;
        for (uint64_t j = 0; j < segments[i].file_size; ++j)
            bytes[kPayloadBase + segments[i].offset + j] = static_cast<uint8_t>(0x30 + i + j);
    }
    return bytes;
}

static void set_image(MemoryFs& fs, const SegmentSpec* segments, uint16_t count) {
    fs.contents = make_image(segments, count);
    fs.fail_reads_at = UINT64_MAX;
    fs.eof_reads_at = UINT64_MAX;
    fs.max_read = UINT32_MAX;
}

static ElfLoadResult attempt(MemoryFs& fs, uint64_t range_size, uintptr_t root,
                             VmRegionTable& regions, ElfImageInfo* info) {
    auto* node = new VFS::Node{};
    node->type = VFS::NodeType::FILE;
    node->size = fs.contents.size();
    node->fs = &fs;
    VFS::FileObject file(node, VFS_O_READ);
    return elf_load_executable(file, kPayloadBase, range_size, root, regions, info);
}

static bool state_is_empty(uintptr_t root, VmRegionTable& regions,
                           const SegmentSpec* segments, uint16_t count) {
    bool ok = regions.count() == 0;
    uintptr_t kernel_root = vmm_get_kernel_pml4();
    ok &= vmm_switch_address_space(root) == VmmAddressSpaceResult::Success;
    for (uint16_t i = 0; i < count; ++i) {
        if (!segments[i].memory_size) continue;
        uintptr_t page = static_cast<uintptr_t>(segments[i].address & ~(PAGE_SIZE - 1));
        uint64_t end = segments[i].address + segments[i].memory_size;
        uint64_t page_end = (end + PAGE_SIZE - 1) & ~(uint64_t(PAGE_SIZE - 1));
        for (; page < page_end; page += PAGE_SIZE) {
            VmmMappingInfo mapping{};
            ok &= !vmm_query_mapping(page, &mapping);
        }
    }
    ok &= vmm_switch_address_space(kernel_root) == VmmAddressSpaceResult::Success;
    return ok;
}
}

bool run_elf_loader_test() {
    const SegmentSpec valid_segments[] = {
        {0x1000, kCodeAddress, 16, PAGE_SIZE, PF_R | PF_X, PAGE_SIZE},
        {0x2000, kDataAddress, 8, PAGE_SIZE, PF_R | PF_W, PAGE_SIZE},
        {0x3000, kReadOnlyAddress, 8, PAGE_SIZE, PF_R, PAGE_SIZE},
    };
    MemoryFs fs;
    set_image(fs, valid_segments, 3);
    uintptr_t root = vmm_create_address_space();
    if (!root) return false;
    VmRegionTable regions;
    ElfImageInfo info{};
    bool ok = true;
    vmm_test_set_nx_enabled(true);
    ElfLoadResult loaded = attempt(fs, kPayloadSize, root, regions, &info);
    ok &= check(loaded.error == ElfLoadError::Success, "valid multi-segment image loads");
    if (loaded.error != ElfLoadError::Success) {
        vmm_switch_address_space(vmm_get_kernel_pml4());
        vmm_destroy_address_space(root);
        vmm_test_set_nx_enabled(false);
        return false;
    }
    ok &= check(info.entry_point == kCodeAddress && info.load_segment_count == 3 &&
                info.image_start == kCodeAddress && info.image_end == kReadOnlyAddress + PAGE_SIZE,
                "image metadata describes mapped executable segments");
    ok &= check(info.mapped_memory_size == 3 * PAGE_SIZE && regions.count() == 3,
                "image regions and memory size are reported");
    uintptr_t kernel_root = vmm_get_kernel_pml4();
    ok &= vmm_switch_address_space(root) == VmmAddressSpaceResult::Success;
    VmmMappingInfo code{}, data{}, read_only{};
    ok &= check(vmm_query_mapping(kCodeAddress, &code) &&
                !(code.flags & VMM_WRITE) && !(code.flags & VMM_NX),
                "RX segment is mapped read-only and executable");
    ok &= check(vmm_query_mapping(kDataAddress, &data) &&
                (data.flags & VMM_WRITE) && (data.flags & VMM_NX),
                "RW segment is writable and NX when supported");
    ok &= check(vmm_query_mapping(kReadOnlyAddress, &read_only) &&
                !(read_only.flags & VMM_WRITE) && (read_only.flags & VMM_NX),
                "read-only segment permissions are preserved");
    auto* code_bytes = reinterpret_cast<uint8_t*>(code.physical_page + pmm_hhdm_offset());
    auto* bss_bytes = reinterpret_cast<uint8_t*>(data.physical_page + pmm_hhdm_offset());
    ok &= check(code_bytes[0] == 0x30 && code_bytes[15] == 0x3F && code_bytes[16] == 0,
                "file bytes are copied and trailing memory is zero-filled");
    ok &= check(bss_bytes[0] == 0x31 && bss_bytes[8] == 0,
                "writable segment content and BSS are initialized");
    VmRegion region{};
    ok &= check(regions.find(kDataAddress, &region) &&
                (region.permissions & VM_PERM_WRITE) &&
                !(region.permissions & VM_PERM_EXECUTE),
                "region metadata matches data-segment permissions");
    if (regions.find(kDataAddress, &region)) VmRegionTable::release_snapshot(region);
    ok &= vmm_switch_address_space(kernel_root) == VmmAddressSpaceResult::Success;
    vmm_test_set_nx_enabled(false);

    ok &= check(vmm_destroy_address_space(root) == VmmAddressSpaceResult::Success,
                "caller can destroy the loaded address space");

    const SegmentSpec minimal_segment[] = {valid_segments[0]};
    root = vmm_create_address_space();
    if (!root) return false;
    VmRegionTable minimal_regions;
    set_image(fs, minimal_segment, 1);
    ElfImageInfo minimal_info{};
    ok &= check(attempt(fs, kPayloadSize, root, minimal_regions, &minimal_info).error ==
                ElfLoadError::Success && minimal_info.load_segment_count == 1,
                "minimal single-segment executable loads");
    ok &= check(vmm_destroy_address_space(root) == VmmAddressSpaceResult::Success,
                "minimal image address space is released");

    root = vmm_create_address_space();
    if (!root) return false;
    VmRegionTable invalid_regions;
    auto expect_invalid = [&](const char* label, auto mutate, ElfLoadError expected,
                              uint64_t range_size = kPayloadSize) {
        set_image(fs, valid_segments, 3);
        mutate(fs.contents.data() + kPayloadBase);
        ElfImageInfo failed_info{1, 1, 1, 1, 1};
        ElfLoadResult result = attempt(fs, range_size, root, invalid_regions, &failed_info);
        bool clean = state_is_empty(root, invalid_regions, valid_segments, 3);
        ok &= check(result.error == expected && clean && failed_info.entry_point == 0,
                    label);
    };
    expect_invalid("invalid magic is rejected without mappings", [](uint8_t* p) { p[0] = 0; },
                   ElfLoadError::InvalidMagic);
    expect_invalid("unsupported ELF class is rejected", [](uint8_t* p) { p[EI_CLASS] = 1; },
                   ElfLoadError::UnsupportedClass);
    expect_invalid("unsupported byte order is rejected", [](uint8_t* p) { p[EI_DATA] = 2; },
                   ElfLoadError::UnsupportedEndianness);
    expect_invalid("unsupported ELF version is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_version = 0;
    }, ElfLoadError::UnsupportedVersion);
    expect_invalid("unsupported identification version is rejected", [](uint8_t* p) {
        p[EI_VERSION] = 0;
    }, ElfLoadError::UnsupportedVersion);
    expect_invalid("unsupported ELF type is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_type = ET_DYN;
    }, ElfLoadError::UnsupportedType);
    expect_invalid("unsupported architecture is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_machine = 3;
    }, ElfLoadError::UnsupportedMachine);
    expect_invalid("invalid header size is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_ehsize = 1;
    }, ElfLoadError::InvalidHeader);
    expect_invalid("invalid program-header entry size is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_phentsize = 1;
    }, ElfLoadError::InvalidHeader);
    expect_invalid("program-header table outside file is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_phoff = UINT64_MAX - 10;
    }, ElfLoadError::InvalidProgramHeaders);
    expect_invalid("segment file range outside image is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_offset = 0x4FFF;
    }, ElfLoadError::InvalidSegment);
    expect_invalid("overflowing segment file offset is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_offset = UINT64_MAX - 3;
    }, ElfLoadError::InvalidSegment);
    expect_invalid("file size larger than memory size is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_memsz = 1;
    }, ElfLoadError::InvalidSegment);
    expect_invalid("non-power-of-two segment alignment is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_align = 3;
    }, ElfLoadError::InvalidAlignment);
    expect_invalid("overflowing user address range is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_vaddr = UINT64_MAX - 7;
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_align = 1;
    }, ElfLoadError::InvalidAddress);
    expect_invalid("entry outside executable segments is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Ehdr*>(p)->e_entry = 0x500000;
    }, ElfLoadError::InvalidEntryPoint);
    expect_invalid("truncated ELF payload is rejected", [](uint8_t*) {},
                   ElfLoadError::InvalidProgramHeaders, sizeof(Elf64_Ehdr));
    expect_invalid("write-execute segment is rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[0].p_flags |= PF_W;
    }, ElfLoadError::InvalidSegment);
    expect_invalid("dynamic ELF loading requirements are rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[1].p_type = PT_DYNAMIC;
    }, ElfLoadError::UnsupportedProgramHeader);
    expect_invalid("executable stack requirements are rejected", [](uint8_t* p) {
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[1].p_type = PT_GNU_STACK;
        reinterpret_cast<Elf64_Phdr*>(p + sizeof(Elf64_Ehdr))[1].p_flags = PF_R | PF_X;
    }, ElfLoadError::UnsupportedProgramHeader);

    set_image(fs, valid_segments, 3);
    fs.max_read = 7;
    ElfImageInfo short_read_info{};
    ok &= check(attempt(fs, kPayloadSize, root, invalid_regions, &short_read_info).error ==
                ElfLoadError::Success,
                "positive short reads are completed through read_at");
    ok &= check(vmm_destroy_address_space(root) == VmmAddressSpaceResult::Success,
                "address space with short-read-loaded image is releasable");

    root = vmm_create_address_space();
    if (!root) return false;
    VmRegionTable failure_regions;
    set_image(fs, valid_segments, 3);
    fs.fail_reads_at = kPayloadBase + valid_segments[0].offset + 4;
    ElfImageInfo failure_info{};
    ElfLoadResult read_failure = attempt(fs, kPayloadSize, root, failure_regions, &failure_info);
    ok &= check(read_failure.error == ElfLoadError::ReadError &&
                state_is_empty(root, failure_regions, valid_segments, 3),
                "read failure rolls back already mapped image pages");

    fs.fail_reads_at = UINT64_MAX;
    fs.eof_reads_at = kPayloadBase + valid_segments[0].offset + 4;
    ElfLoadResult short_read_failure = attempt(fs, kPayloadSize, root,
                                                failure_regions, &failure_info);
    ok &= check(short_read_failure.error == ElfLoadError::ShortRead &&
                state_is_empty(root, failure_regions, valid_segments, 3),
                "premature end after a positive short read rolls back mapped pages");

    fs.fail_reads_at = UINT64_MAX;
    fs.eof_reads_at = UINT64_MAX;
    vmm_test_fail_page_table_allocation_after(0);
    ElfLoadResult allocation_failure = attempt(fs, kPayloadSize, root, failure_regions, &failure_info);
    vmm_test_fail_page_table_allocation_after(-1);
    ok &= check(allocation_failure.error == ElfLoadError::MappingFailed &&
                allocation_failure.mapping_error == VmmMapResult::PageTableOutOfMemory &&
                state_is_empty(root, failure_regions, valid_segments, 3),
                "page-table allocation failure leaves no mappings or regions");

    vmm_test_fail_page_table_allocation_after(3);
    ElfLoadResult partial_allocation_failure = attempt(fs, kPayloadSize, root,
                                                        failure_regions, &failure_info);
    vmm_test_fail_page_table_allocation_after(-1);
    ok &= check(partial_allocation_failure.error == ElfLoadError::MappingFailed &&
                partial_allocation_failure.mapping_error == VmmMapResult::PageTableOutOfMemory &&
                state_is_empty(root, failure_regions, valid_segments, 3),
                "later page-table allocation failure rolls back earlier segment pages");

    VmRegion existing_region{VM_PERM_READ, VmRegionType::Anonymous,
        VmRegionSharing::Private, 0, kCodeAddress, kCodeAddress + PAGE_SIZE, 0, nullptr};
    ok &= check(failure_regions.insert(existing_region) == VmRegionResult::Success,
                "test installs caller-owned region before a load attempt");
    ElfLoadResult region_failure = attempt(fs, kPayloadSize, root, failure_regions, &failure_info);
    uintptr_t kernel_root_after_failure = vmm_get_kernel_pml4();
    bool no_new_mapping = vmm_switch_address_space(root) == VmmAddressSpaceResult::Success;
    VmmMappingInfo overlapping_mapping{};
    no_new_mapping &= !vmm_query_mapping(kCodeAddress, &overlapping_mapping);
    no_new_mapping &= vmm_switch_address_space(kernel_root_after_failure) ==
                      VmmAddressSpaceResult::Success;
    ok &= check(region_failure.error == ElfLoadError::RegionFailed &&
                region_failure.region_error == VmRegionResult::Overlap &&
                failure_regions.count() == 1 && no_new_mapping,
                "region insertion failure rolls back mappings and preserves caller metadata");
    failure_regions.clear();

    ok &= check(vmm_destroy_address_space(root) == VmmAddressSpaceResult::Success,
                "failed loads leave caller-owned address spaces destroyable");
    if (!ok) return false;
    std::puts("ELF loader validation, permissions, short reads, and rollback checks passed.");
    return true;
}
