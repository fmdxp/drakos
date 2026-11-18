#pragma once

#include <stdint.h>
#include "kernel/handle.hpp"

enum class VmRegionType : uint8_t {
    Anonymous,
    File,
    Stack,
    Kernel,
    SharedMemory,
};

enum class VmRegionSharing : uint8_t { Private, Shared };

enum VmRegionPermission : uint8_t {
    VM_PERM_READ = 1u << 0,
    VM_PERM_WRITE = 1u << 1,
    VM_PERM_EXECUTE = 1u << 2,
};

struct VmRegion {
    uint8_t permissions;
    VmRegionType type;
    VmRegionSharing sharing;
    uint8_t reserved;
    uintptr_t start;
    uintptr_t end;
    uint64_t file_offset;
    KernelObject* backing;
};

enum class VmRegionResult : uint8_t {
    Success,
    InvalidRange,
    Overlap,
    NotMapped,
    CapacityExceeded,
    InvalidBacking,
};

enum class VmFaultResult : uint8_t {
    NotHandled,
    Resolved,
    ProtectionFault,
    OutOfMemory,
};

class VmRegionTable {
public:
    static constexpr uint32_t Capacity = 64;

    VmRegionTable();
    ~VmRegionTable();
    VmRegionTable(const VmRegionTable&) = delete;
    VmRegionTable& operator=(const VmRegionTable&) = delete;

    VmRegionResult insert(const VmRegion& region);
    bool find(uintptr_t address, VmRegion* retained_snapshot);
    VmFaultResult resolve_anonymous_fault(uintptr_t page_table_root,
                                          uintptr_t address,
                                          uint64_t error_code);
    VmRegionResult remove(uintptr_t start, uintptr_t end);
    bool covers(uintptr_t start, uintptr_t end);
    void clear();
    uint32_t count();

    static void release_snapshot(VmRegion& region);

private:
    VmRegion m_regions[Capacity];
    uint32_t m_count;
    volatile bool m_lock;

    uint64_t lock();
    void unlock(uint64_t flags);
};
