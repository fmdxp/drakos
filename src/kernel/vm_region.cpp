#include "kernel/vm_region.hpp"

#ifndef DRAKOS_HOST_TEST
#include "cpu.hpp"
#endif
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "cpu.hpp"

VmRegionTable::VmRegionTable() : m_regions{}, m_count(0), m_lock(false) {}

VmRegionTable::~VmRegionTable() { clear(); }

uint64_t VmRegionTable::lock() {
#ifdef DRAKOS_HOST_TEST
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE)) {}
    return 0;
#else
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE)) asm volatile("pause");
    return flags;
#endif
}

void VmRegionTable::unlock(uint64_t flags) {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

VmRegionResult VmRegionTable::insert(const VmRegion& region) {
    constexpr uintptr_t USER_LIMIT = 1ULL << 47;
    if (region.start >= region.end || (region.start & 0xFFF) || (region.end & 0xFFF) ||
        region.end > USER_LIMIT || !region.permissions || (region.permissions & ~7u))
        return VmRegionResult::InvalidRange;
    if (region.type == VmRegionType::File &&
        (!region.backing || region.backing->type() != KernelObjectType::File))
        return VmRegionResult::InvalidBacking;
    if ((region.backing && !region.backing->retain())) return VmRegionResult::InvalidBacking;

    uint64_t flags = lock();
    for (uint32_t i = 0; i < m_count; ++i) {
        if (region.start < m_regions[i].end && m_regions[i].start < region.end) {
            unlock(flags);
            if (region.backing) region.backing->release();
            return VmRegionResult::Overlap;
        }
    }
    if (m_count == Capacity) {
        unlock(flags);
        if (region.backing) region.backing->release();
        return VmRegionResult::CapacityExceeded;
    }
    m_regions[m_count++] = region;
    unlock(flags);
    return VmRegionResult::Success;
}

bool VmRegionTable::find(uintptr_t address, VmRegion* snapshot) {
    if (!snapshot) return false;
    uint64_t flags = lock();
    for (uint32_t i = 0; i < m_count; ++i) {
        VmRegion& region = m_regions[i];
        if (address < region.start || address >= region.end) continue;
        *snapshot = region;
        if (snapshot->backing && !snapshot->backing->retain()) {
            unlock(flags);
            return false;
        }
        unlock(flags);
        return true;
    }
    unlock(flags);
    return false;
}

VmFaultResult VmRegionTable::resolve_anonymous_fault(uintptr_t root,
                                                      uintptr_t address,
                                                      uint64_t error_code) {
    constexpr uint64_t PF_PRESENT = 1u << 0;
    constexpr uint64_t PF_WRITE = 1u << 1;
    constexpr uint64_t PF_USER = 1u << 2;
    constexpr uint64_t PF_RESERVED = 1u << 3;
    constexpr uint64_t PF_INSTRUCTION = 1u << 4;
    if (!root || (error_code & (PF_PRESENT | PF_RESERVED)) || !(error_code & PF_USER))
        return VmFaultResult::NotHandled;
    uintptr_t page = address & ~static_cast<uintptr_t>(PAGE_SIZE - 1);

    uint64_t lock_flags = lock();
    VmRegion* region = nullptr;
    for (uint32_t i = 0; i < m_count; ++i)
        if (address >= m_regions[i].start && address < m_regions[i].end) {
            region = &m_regions[i];
            break;
        }
    if (!region || (region->type != VmRegionType::Anonymous &&
                    region->type != VmRegionType::Stack)) {
        unlock(lock_flags);
        return VmFaultResult::NotHandled;
    }

    bool write = (error_code & PF_WRITE) != 0;
    bool execute = (error_code & PF_INSTRUCTION) != 0;
    bool read = !write && !execute;
    bool allowed = (!read || (region->permissions & VM_PERM_READ)) &&
                   (!write || (region->permissions & VM_PERM_WRITE)) &&
                   (!execute || (region->permissions & VM_PERM_EXECUTE));
    VmmMappingInfo existing{};
    if (vmm_query_mapping(page, &existing)) {
        bool mapped_access = (!write || (existing.flags & VMM_WRITE)) &&
                             (!execute || !(existing.flags & VMM_NX));
        unlock(lock_flags);
        return mapped_access ? VmFaultResult::Resolved : VmFaultResult::ProtectionFault;
    }

    uintptr_t frame = pmm_alloc_page();
    if (!frame) {
        unlock(lock_flags);
        return VmFaultResult::OutOfMemory;
    }
    auto* bytes = reinterpret_cast<volatile uint8_t*>(frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;

    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (region->permissions & VM_PERM_WRITE) flags |= VMM_WRITE;
    if (!(region->permissions & VM_PERM_EXECUTE) && nx_enabled()) flags |= VMM_NX;
    VmmMapResult mapped = vmm_map_owned_page(root, page, frame, flags);
    if (mapped != VmmMapResult::Success) {
        pmm_free_page(frame);
        if (mapped == VmmMapResult::MappingExists && vmm_query_mapping(page, &existing)) {
            bool mapped_access = (!write || (existing.flags & VMM_WRITE)) &&
                                 (!execute || !(existing.flags & VMM_NX));
            unlock(lock_flags);
            return mapped_access ? VmFaultResult::Resolved : VmFaultResult::ProtectionFault;
        }
        unlock(lock_flags);
        return mapped == VmmMapResult::PageTableOutOfMemory
            ? VmFaultResult::OutOfMemory : VmFaultResult::NotHandled;
    }
    unlock(lock_flags);
    return allowed ? VmFaultResult::Resolved : VmFaultResult::ProtectionFault;
}

bool VmRegionTable::covers(uintptr_t start, uintptr_t end) {
    if (start >= end) return false;
    uint64_t flags = lock();
    uintptr_t cursor = start;
    while (cursor < end) {
        uintptr_t furthest = cursor;
        for (uint32_t i = 0; i < m_count; ++i)
            if (m_regions[i].start <= cursor && m_regions[i].end > furthest)
                furthest = m_regions[i].end;
        if (furthest == cursor) {
            unlock(flags);
            return false;
        }
        cursor = furthest < end ? furthest : end;
    }
    unlock(flags);
    return true;
}

VmRegionResult VmRegionTable::remove(uintptr_t start, uintptr_t end) {
    if (start >= end || (start & 0xFFF) || (end & 0xFFF)) return VmRegionResult::InvalidRange;
    uint64_t flags = lock();
    uintptr_t cursor = start;
    while (cursor < end) {
        uintptr_t furthest = cursor;
        for (uint32_t i = 0; i < m_count; ++i)
            if (m_regions[i].start <= cursor && m_regions[i].end > furthest)
                furthest = m_regions[i].end;
        if (furthest == cursor) {
            unlock(flags);
            return VmRegionResult::NotMapped;
        }
        cursor = furthest < end ? furthest : end;
    }

    uint32_t splits = 0;
    for (uint32_t i = 0; i < m_count; ++i)
        if (m_regions[i].start < start && end < m_regions[i].end) ++splits;
    if (splits > Capacity - m_count) {
        unlock(flags);
        return VmRegionResult::CapacityExceeded;
    }

    for (uint32_t i = 0; i < m_count; ++i) {
        const VmRegion& region = m_regions[i];
        if (region.start >= start || end >= region.end || !region.backing) continue;
        if (!region.backing->retain()) {
            for (uint32_t j = 0; j < i; ++j) {
                const VmRegion& prior = m_regions[j];
                if (prior.start < start && end < prior.end && prior.backing)
                    prior.backing->release();
            }
            unlock(flags);
            return VmRegionResult::InvalidBacking;
        }
    }

    KernelObject* released[Capacity] = {};
    uint32_t released_count = 0;
    for (uint32_t i = 0; i < m_count;) {
        VmRegion& region = m_regions[i];
        if (region.start >= end || region.end <= start) { ++i; continue; }
        bool keep_left = region.start < start;
        bool keep_right = end < region.end;
        if (keep_left && keep_right) {
            VmRegion right = region;
            right.start = end;
            right.file_offset += end - region.start;
            region.end = start;
            m_regions[m_count++] = right;
            ++i;
        } else if (keep_left) {
            region.end = start;
            ++i;
        } else if (keep_right) {
            region.file_offset += end - region.start;
            region.start = end;
            ++i;
        } else {
            if (region.backing) released[released_count++] = region.backing;
            m_regions[i] = m_regions[--m_count];
        }
    }
    unlock(flags);
    for (uint32_t i = 0; i < released_count; ++i) released[i]->release();
    return VmRegionResult::Success;
}

void VmRegionTable::clear() {
    KernelObject* released[Capacity] = {};
    uint32_t count = 0;
    uint64_t flags = lock();
    for (uint32_t i = 0; i < m_count; ++i)
        if (m_regions[i].backing) released[count++] = m_regions[i].backing;
    m_count = 0;
    unlock(flags);
    for (uint32_t i = 0; i < count; ++i) released[i]->release();
}

uint32_t VmRegionTable::count() {
    uint64_t flags = lock();
    uint32_t result = m_count;
    unlock(flags);
    return result;
}

void VmRegionTable::release_snapshot(VmRegion& region) {
    if (region.backing) region.backing->release();
    region.backing = nullptr;
}
