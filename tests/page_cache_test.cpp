#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

#include "fs/page_cache.hpp"
#include "fs/vfs.hpp"
#include "memory/pmm.hpp"

class CacheTestFs final : public VFS::FileSystem {
public:
    uint8_t data[5000];

    CacheTestFs() {
        for (uint32_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i % 251);
    }
    VFS::Node* lookup(const char*) override {
        auto* node = new VFS::Node{};
        if (!node) return nullptr;
        node->type = VFS::NodeType::FILE;
        node->size = sizeof(data);
        node->fs = this;
        return node;
    }
    VFS::Node* create(const char*, bool) override { return lookup(""); }
    int read(VFS::Node*, void* output, uint32_t offset, uint32_t size) override {
        if (!output || offset >= sizeof(data)) return 0;
        if (size > sizeof(data) - offset) size = sizeof(data) - offset;
        std::memcpy(output, data + offset, size);
        return static_cast<int>(size);
    }
    int write(VFS::Node* node, const void* input, uint32_t offset, uint32_t size) override {
        if (!node || !input || offset > sizeof(data) || size > sizeof(data) - offset) return -1;
        std::memcpy(data + offset, input, size);
        if (offset + size > node->size) node->size = offset + size;
        return static_cast<int>(size);
    }
    bool listdir(VFS::Node*, VFS::DirEntry*, uint32_t, uint32_t& count) override {
        count = 0;
        return true;
    }
    void release(VFS::Node* node) override { delete node; }
    uint64_t file_identity(const VFS::Node*) const override { return 0x4452414B4F530001ULL; }
};

static bool check(bool condition, const char* name) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

bool run_page_cache_test() {
    CacheTestFs fs;
    bool ok = VFS::VFSManager::get().mount("/cache", &fs);
    VFS::FileObject* first_file = vfs_open_file("/cache/file", VFS_O_READ | VFS_O_WRITE);
    VFS::FileObject* second_file = vfs_open_file("/cache/file", VFS_O_READ);
    ok &= check(first_file && second_file, "cache test opens two file references");

    CachedFilePage* first = nullptr;
    CachedFilePage* second = nullptr;
    ok &= check(page_cache_acquire(first_file, 0, &first) == PageCacheResult::Success &&
        page_cache_acquire(second_file, 0, &second) == PageCacheResult::Success,
        "cache loads a file page and serves a repeated lookup");
    ok &= check(first && second && first == second &&
        first->physical_address() == second->physical_address() && first->data()[0] == 0,
        "same file identity and page offset reuse one persistent physical page");

    CachedFilePage* tail = nullptr;
    ok &= check(page_cache_acquire(first_file, 4096, &tail) == PageCacheResult::Success && tail,
        "partial final file page is cached");
    bool tail_ok = tail && tail->data()[0] == static_cast<uint8_t>(4096 % 251);
    if (tail) for (size_t i = 904; i < PAGE_SIZE; ++i) tail_ok &= tail->data()[i] == 0;
    ok &= check(tail_ok, "partial final page keeps file bytes and zero-fills its EOF tail");
    CachedFilePage* eof = nullptr;
    ok &= check(page_cache_acquire(first_file, 8192, &eof) == PageCacheResult::EndOfFile && !eof,
        "page requests beyond EOF fail without creating cache entries");

    uint8_t replacement = 'Z';
    ok &= check(first_file->write_at(0, &replacement, 1) == 1,
        "file writes invalidate cached pages after modifying data");
    CachedFilePage* updated = nullptr;
    ok &= check(page_cache_acquire(second_file, 0, &updated) == PageCacheResult::Success && updated,
        "lookup after write reloads the invalidated file page");
    ok &= check(updated && updated == first && updated->data()[0] == replacement &&
        first && first->data()[0] == replacement,
        "file writes update cached pages held by other references");

    uint64_t identity = first_file ? first_file->identity() : 0;
    page_cache_invalidate(identity, 0, PAGE_SIZE);
    CachedFilePage* concurrent[8] = {};
    std::atomic<bool> concurrent_ok{true};
    std::thread readers[8];
    for (size_t i = 0; i < 8; ++i) {
        readers[i] = std::thread([&, i] {
            if (page_cache_acquire(i & 1 ? first_file : second_file, 0, &concurrent[i]) !=
                    PageCacheResult::Success || !concurrent[i])
                concurrent_ok.store(false, std::memory_order_relaxed);
        });
    }
    for (auto& reader : readers) reader.join();
    bool same_concurrent_page = concurrent_ok.load();
    for (size_t i = 1; i < 8; ++i)
        same_concurrent_page &= concurrent[i] && concurrent[0] == concurrent[i];
    ok &= check(same_concurrent_page,
        "concurrent cache misses converge on one persistent entry");
    for (CachedFilePage* page : concurrent) if (page) page->release();

    if (first) first->release();
    if (second) second->release();
    if (tail) tail->release();
    if (updated) updated->release();
    uint32_t removed = page_cache_invalidate(identity, 0, 0);
    ok &= check(removed >= 2, "cache invalidation removes all cached pages for a file");
    uintptr_t before_exhaustion = pmm_alloc_page();
    if (before_exhaustion) pmm_free_page(before_exhaustion);
    static uintptr_t exhausted_pages[4096];
    size_t exhausted_count = 0;
    while (exhausted_count < sizeof(exhausted_pages) / sizeof(exhausted_pages[0]) &&
           (exhausted_pages[exhausted_count] = pmm_alloc_page())) ++exhausted_count;
    CachedFilePage* failed = nullptr;
    PageCacheResult exhausted_result = page_cache_acquire(first_file, 0, &failed);
    ok &= check(exhausted_count != 0 && exhausted_result == PageCacheResult::OutOfMemory && !failed,
        "PMM exhaustion fails page loading without publishing a partial cache entry");
    while (exhausted_count) pmm_free_page(exhausted_pages[--exhausted_count]);
    uintptr_t evict_phys = 0;
    CachedFilePage* reclaim = nullptr;
    if (page_cache_acquire(first_file, 0, &reclaim) == PageCacheResult::Success && reclaim) {
        evict_phys = reclaim->physical_address();
        reclaim->release();
    }
    bool evicted = PageCache::get().evict_one();
    ok &= check(evicted && evict_phys && !pmm_is_allocated_page(evict_phys),
        "unreferenced cache eviction releases its PMM frame");
    if (first_file) first_file->release();
    if (second_file) second_file->release();
    ok &= check(VFS::VFSManager::get().unmount("/cache"), "cache test mount can be removed");
    if (ok) std::puts("Page cache identity, reuse, EOF, invalidation, and lifetime checks passed.");
    return ok;
}
