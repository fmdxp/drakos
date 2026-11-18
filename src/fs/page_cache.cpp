#include "fs/page_cache.hpp"
#include "fs/vfs.hpp"
#include "memory/pmm.hpp"

#ifndef DRAKOS_HOST_TEST
#include "cpu.hpp"
#endif

CachedFilePage::CachedFilePage(VFS::FileObject* file, uint64_t identity,
                               uint64_t offset, uintptr_t physical)
    : KernelObject(KernelObjectType::File), m_file(file), m_identity(identity),
      m_offset(offset), m_physical(physical), m_last_use(0) {}

CachedFilePage::~CachedFilePage() {
    if (m_physical) pmm_free_page(m_physical);
    if (m_file) m_file->release();
}

const uint8_t* CachedFilePage::data() const {
    return reinterpret_cast<const uint8_t*>(m_physical + pmm_hhdm_offset());
}

PageCache::PageCache() : m_entries{}, m_clock(0), m_generation(0), m_lock(false) {}

static PageCache s_page_cache;
PageCache& PageCache::get() { return s_page_cache; }

uint64_t PageCache::lock() {
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

void PageCache::unlock(uint64_t flags) {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

PageCacheResult PageCache::acquire(VFS::FileObject* file, uint64_t offset,
                                   CachedFilePage** output) {
    if (output) *output = nullptr;
    if (!file || !output || (offset & (PAGE_SIZE - 1))) return PageCacheResult::InvalidFile;
    uint64_t identity = file->identity();
    uint64_t length = file->size();
    if (!identity) return PageCacheResult::InvalidFile;
    if (offset >= length) return PageCacheResult::EndOfFile;
    if (offset > UINT32_MAX) return PageCacheResult::InvalidFile;

    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        uint64_t flags = lock();
        uint64_t generation = m_generation;
        for (CachedFilePage* entry : m_entries) {
            if (entry && entry->m_identity == identity && entry->m_offset == offset && entry->retain()) {
                entry->m_last_use = ++m_clock;
                *output = entry;
                unlock(flags);
                return PageCacheResult::Success;
            }
        }
        unlock(flags);

        if (!file->retain()) return PageCacheResult::InvalidFile;
        uintptr_t physical = pmm_alloc_page();
        if (!physical) { file->release(); return PageCacheResult::OutOfMemory; }
        auto* bytes = reinterpret_cast<uint8_t*>(physical + pmm_hhdm_offset());
        for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;
        uint32_t wanted = static_cast<uint32_t>(length - offset);
        if (wanted > PAGE_SIZE) wanted = PAGE_SIZE;
        int read = file->read_at(offset, bytes, wanted);
        if (read < 0 || static_cast<uint32_t>(read) != wanted) {
            pmm_free_page(physical);
            file->release();
            return PageCacheResult::IoError;
        }
        CachedFilePage* candidate = new CachedFilePage(file, identity, offset, physical);
        if (!candidate) {
            pmm_free_page(physical);
            file->release();
            return PageCacheResult::OutOfMemory;
        }

        CachedFilePage* evicted = nullptr;
        flags = lock();
        if (generation != m_generation) {
            unlock(flags);
            candidate->release();
            continue;
        }
        int32_t free_slot = -1;
        int32_t oldest_slot = -1;
        uint64_t oldest_use = UINT64_MAX;
        for (uint32_t i = 0; i < Capacity; ++i) {
            CachedFilePage* entry = m_entries[i];
            if (!entry) { if (free_slot < 0) free_slot = static_cast<int32_t>(i); continue; }
            if (entry->m_identity == identity && entry->m_offset == offset && entry->retain()) {
                entry->m_last_use = ++m_clock;
                *output = entry;
                unlock(flags);
                candidate->release();
                return PageCacheResult::Success;
            }
            if (entry->reference_count() == 1 && entry->m_last_use < oldest_use) {
                oldest_use = entry->m_last_use;
                oldest_slot = static_cast<int32_t>(i);
            }
        }
        int32_t slot = free_slot >= 0 ? free_slot : oldest_slot;
        if (slot < 0) {
            unlock(flags);
            candidate->release();
            return PageCacheResult::OutOfMemory;
        }
        if (m_entries[slot]) evicted = m_entries[slot];
        if (!candidate->retain()) {
            unlock(flags);
            candidate->release();
            return PageCacheResult::OutOfMemory;
        }
        candidate->m_last_use = ++m_clock;
        m_entries[slot] = candidate;
        *output = candidate;
        unlock(flags);
        if (evicted) evicted->release();
        return PageCacheResult::Success;
    }
    return PageCacheResult::IoError;
}

uint32_t PageCache::invalidate(uint64_t identity, uint64_t offset, uint64_t length) {
    if (!identity) return 0;
    uint64_t end = length > UINT64_MAX - offset ? UINT64_MAX : offset + length;
    CachedFilePage* removed[Capacity] = {};
    uint32_t count = 0;
    uint64_t flags = lock();
    ++m_generation;
    for (uint32_t i = 0; i < Capacity; ++i) {
        CachedFilePage* entry = m_entries[i];
        if (!entry || entry->m_identity != identity) continue;
        bool matches = length == 0 || (entry->m_offset < end && offset < entry->m_offset + PAGE_SIZE);
        if (!matches) continue;
        removed[count++] = entry;
        m_entries[i] = nullptr;
    }
    unlock(flags);
    for (uint32_t i = 0; i < count; ++i) removed[i]->release();
    return count;
}

void PageCache::update(uint64_t identity, uint64_t offset, const void* data, uint32_t length) {
    if (!identity || (!data && length) || !length || offset > UINT64_MAX - length) return;
    CachedFilePage* pages[Capacity] = {};
    uint32_t count = 0;
    uint64_t end = offset + length;
    uint64_t flags = lock();
    ++m_generation;
    for (CachedFilePage* entry : m_entries) {
        if (!entry || entry->m_identity != identity ||
            entry->m_offset >= end || offset >= entry->m_offset + PAGE_SIZE || !entry->retain())
            continue;
        pages[count++] = entry;
    }
    unlock(flags);

    const auto* source = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < count; ++i) {
        CachedFilePage* page = pages[i];
        uint64_t begin = offset > page->m_offset ? offset : page->m_offset;
        uint64_t finish = end < page->m_offset + PAGE_SIZE ? end : page->m_offset + PAGE_SIZE;
        auto* destination = reinterpret_cast<uint8_t*>(page->m_physical + pmm_hhdm_offset());
        for (uint64_t at = begin; at < finish; ++at)
            destination[at - page->m_offset] = source[at - offset];
        page->release();
    }
}

bool PageCache::evict_one() {
    CachedFilePage* evicted = nullptr;
    uint64_t flags = lock();
    int32_t slot = -1;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < Capacity; ++i) {
        CachedFilePage* entry = m_entries[i];
        if (entry && entry->reference_count() == 1 && entry->m_last_use < oldest) {
            oldest = entry->m_last_use;
            slot = static_cast<int32_t>(i);
        }
    }
    if (slot >= 0) {
        evicted = m_entries[slot];
        m_entries[slot] = nullptr;
    }
    unlock(flags);
    if (evicted) evicted->release();
    return evicted != nullptr;
}

uint32_t PageCache::entry_count() {
    uint32_t count = 0;
    uint64_t flags = lock();
    for (CachedFilePage* entry : m_entries) if (entry) ++count;
    unlock(flags);
    return count;
}

PageCacheResult page_cache_acquire(VFS::FileObject* file, uint64_t offset,
                                   CachedFilePage** output) {
    return PageCache::get().acquire(file, offset, output);
}

uint32_t page_cache_invalidate(uint64_t identity, uint64_t offset, uint64_t length) {
    return PageCache::get().invalidate(identity, offset, length);
}

void page_cache_update(uint64_t identity, uint64_t offset, const void* data, uint32_t length) {
    PageCache::get().update(identity, offset, data, length);
}
