#pragma once

#include <stdint.h>
#include "kernel/handle.hpp"

namespace VFS { class FileObject; }

enum class PageCacheResult : uint8_t {
    Success,
    InvalidFile,
    EndOfFile,
    IoError,
    OutOfMemory,
};

class CachedFilePage final : public KernelObject {
public:
    ~CachedFilePage() override;
    uint64_t file_identity() const { return m_identity; }
    uint64_t file_offset() const { return m_offset; }
    uintptr_t physical_address() const { return m_physical; }
    const uint8_t* data() const;

private:
    friend class PageCache;
    CachedFilePage(VFS::FileObject* retained_file, uint64_t identity,
                   uint64_t offset, uintptr_t owned_physical);
    VFS::FileObject* m_file;
    uint64_t m_identity;
    uint64_t m_offset;
    uintptr_t m_physical;
    uint64_t m_last_use;
};

class PageCache {
public:
    static constexpr uint32_t Capacity = 128;

    PageCache();
    static PageCache& get();
    PageCacheResult acquire(VFS::FileObject* file, uint64_t page_offset,
                            CachedFilePage** retained_page);
    uint32_t invalidate(uint64_t file_identity, uint64_t offset, uint64_t length);
    void update(uint64_t file_identity, uint64_t offset, const void* data, uint32_t length);
    bool evict_one();
    uint32_t entry_count();

private:
    CachedFilePage* m_entries[Capacity];
    uint64_t m_clock;
    uint64_t m_generation;
    volatile bool m_lock;
    uint64_t lock();
    void unlock(uint64_t flags);
};

PageCacheResult page_cache_acquire(VFS::FileObject* file, uint64_t page_offset,
                                   CachedFilePage** retained_page);
uint32_t page_cache_invalidate(uint64_t identity, uint64_t offset, uint64_t length);
void page_cache_update(uint64_t identity, uint64_t offset, const void* data, uint32_t length);
