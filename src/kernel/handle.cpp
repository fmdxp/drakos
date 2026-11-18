#include "kernel/handle.hpp"

#ifndef DRAKOS_HOST_TEST
#include "cpu.hpp"
#endif

KernelObject::KernelObject(KernelObjectType type)
    : m_type(type), m_references(1) {}

bool KernelObject::retain() {
    uint32_t current = __atomic_load_n(&m_references, __ATOMIC_ACQUIRE);
    while (current && current != UINT32_MAX) {
        if (__atomic_compare_exchange_n(&m_references, &current, current + 1, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

void KernelObject::release() {
    uint32_t current = __atomic_load_n(&m_references, __ATOMIC_ACQUIRE);
    while (current) {
        if (__atomic_compare_exchange_n(&m_references, &current, current - 1, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (current == 1) delete this;
            return;
        }
    }
}

uint32_t KernelObject::reference_count() const {
    return __atomic_load_n(&m_references, __ATOMIC_ACQUIRE);
}

KernelHandleTable::KernelHandleTable() : m_entries{}, m_lock(false) {}

KernelHandleTable::~KernelHandleTable() {
    close_all();
}

uint64_t KernelHandleTable::lock() {
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

void KernelHandleTable::unlock(uint64_t flags) {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

bool KernelHandleTable::decode(int32_t handle, uint32_t& index, uint32_t& generation) {
    if (handle < 0) return false;
    uint32_t value = static_cast<uint32_t>(handle);
    index = value & 0x3Fu;
    generation = value >> 6;
    return index >= 3 && index < Capacity;
}

int32_t KernelHandleTable::encode(uint32_t index, uint32_t generation) {
    constexpr uint32_t MAX_GENERATION = 0x01FFFFFFu;
    if (index >= Capacity || generation > MAX_GENERATION) return -1;
    return static_cast<int32_t>((generation << 6) | index);
}

void KernelHandleTable::advance_generation(Entry& entry) {
    constexpr uint32_t MAX_GENERATION = 0x01FFFFFFu;
    if (entry.generation == MAX_GENERATION) {
        entry.generation = MAX_GENERATION + 1;
        return;
    }
    ++entry.generation;
}

int32_t KernelHandleTable::install(KernelObject* object, uint32_t rights) {
    constexpr uint32_t supported_rights = HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE |
        HANDLE_RIGHT_MAP | HANDLE_RIGHT_DUPLICATE | HANDLE_RIGHT_ADMIN;
    if (rights & ~supported_rights) return -1;
    if (!object || !object->retain()) return -1;
    uint64_t flags = lock();
    for (uint32_t i = 3; i < Capacity; ++i) {
        Entry& entry = m_entries[i];
        if (entry.object || entry.generation > 0x01FFFFFFu) continue;
        entry.object = object;
        entry.rights = static_cast<uint8_t>(rights);
        int32_t handle = encode(i, entry.generation);
        unlock(flags);
        return handle;
    }
    unlock(flags);
    object->release();
    return -1;
}

KernelObject* KernelHandleTable::acquire(int32_t handle, KernelObjectType expected,
                                         uint32_t required_rights) {
    uint32_t index, generation;
    if (!decode(handle, index, generation)) return nullptr;
    uint64_t flags = lock();
    Entry& entry = m_entries[index];
    KernelObject* object = entry.object;
    bool valid = object && entry.generation == generation &&
        (expected == KernelObjectType::Any || object->type() == expected) &&
        (entry.rights & required_rights) == required_rights && object->retain();
    unlock(flags);
    return valid ? object : nullptr;
}

int32_t KernelHandleTable::duplicate(int32_t handle) {
    uint32_t index, generation;
    if (!decode(handle, index, generation)) return -1;
    uint64_t flags = lock();
    Entry& entry = m_entries[index];
    if (!entry.object || entry.generation != generation ||
        !(entry.rights & HANDLE_RIGHT_DUPLICATE)) {
        unlock(flags);
        return -1;
    }
    uint32_t rights = entry.rights;
    unlock(flags);
    return duplicate(handle, rights);
}

int32_t KernelHandleTable::duplicate(int32_t handle, uint32_t rights) {
    uint32_t source_index, generation;
    if (!decode(handle, source_index, generation)) return -1;
    uint64_t flags = lock();
    Entry& source = m_entries[source_index];
    if (!source.object || source.generation != generation ||
        !(source.rights & HANDLE_RIGHT_DUPLICATE) || (rights & ~source.rights) ||
        !source.object->retain()) {
        unlock(flags);
        return -1;
    }
    for (uint32_t i = 3; i < Capacity; ++i) {
        Entry& destination = m_entries[i];
        if (destination.object || destination.generation > 0x01FFFFFFu) continue;
        destination.object = source.object;
        destination.rights = static_cast<uint8_t>(rights);
        int32_t result = encode(i, destination.generation);
        unlock(flags);
        return result;
    }
    KernelObject* retained = source.object;
    unlock(flags);
    retained->release();
    return -1;
}

bool KernelHandleTable::close(int32_t handle) {
    uint32_t index, generation;
    if (!decode(handle, index, generation)) return false;
    uint64_t flags = lock();
    Entry& entry = m_entries[index];
    if (!entry.object || entry.generation != generation) {
        unlock(flags);
        return false;
    }
    KernelObject* object = entry.object;
    entry.object = nullptr;
    entry.rights = 0;
    advance_generation(entry);
    unlock(flags);
    object->release();
    return true;
}

void KernelHandleTable::close_all() {
    KernelObject* detached[Capacity] = {};
    uint64_t flags = lock();
    for (uint32_t i = 3; i < Capacity; ++i) {
        Entry& entry = m_entries[i];
        if (!entry.object) continue;
        detached[i] = entry.object;
        entry.object = nullptr;
        entry.rights = 0;
        advance_generation(entry);
    }
    unlock(flags);
    for (uint32_t i = 3; i < Capacity; ++i)
        if (detached[i]) detached[i]->release();
}
