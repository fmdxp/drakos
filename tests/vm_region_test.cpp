#include <atomic>
#include <cstdio>
#include <thread>

#include "kernel/vm_region.hpp"

class TestObject final : public KernelObject {
public:
    explicit TestObject(std::atomic<uint32_t>& deleted)
        : KernelObject(KernelObjectType::File), m_deleted(deleted) {}
    ~TestObject() override { m_deleted.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<uint32_t>& m_deleted;
};

static bool check(bool condition, const char* name) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

bool run_vm_region_metadata_test() {
    bool ok = true;
    std::atomic<uint32_t> deleted{0};
    auto* backing = new TestObject(deleted);
    {
        VmRegionTable table;
        VmRegion file{VM_PERM_READ, VmRegionType::File, VmRegionSharing::Private, 0,
                      0x10000, 0x16000, 0x2000, backing};
        ok &= check(table.insert(file) == VmRegionResult::Success,
                    "file region retains its backing object");
        ok &= check(backing->reference_count() == 2, "region owns a file reference");
        VmRegion overlap{VM_PERM_READ, VmRegionType::Anonymous, VmRegionSharing::Private, 0,
                         0x15000, 0x17000, 0, nullptr};
        ok &= check(table.insert(overlap) == VmRegionResult::Overlap,
                    "overlapping region insertion is rejected");
        VmRegion invalid{VM_PERM_READ, VmRegionType::Anonymous, VmRegionSharing::Private, 0,
                         0x10001, 0x12000, 0, nullptr};
        ok &= check(table.insert(invalid) == VmRegionResult::InvalidRange,
                    "unaligned region ranges are rejected");
        ok &= check(table.covers(0x11000, 0x15000), "region lookup covers interior ranges");
        ok &= check(!table.covers(0xF000, 0x11000), "coverage rejects unmapped prefixes");

        ok &= check(table.remove(0x12000, 0x14000) == VmRegionResult::Success,
                    "partial region removal splits the mapping");
        ok &= check(table.count() == 2 && table.covers(0x10000, 0x12000) &&
                    table.covers(0x14000, 0x16000), "split retains both sides around the hole");
        VmRegion snapshot{};
        ok &= check(table.find(0x14000, &snapshot) && snapshot.file_offset == 0x6000,
                    "right split advances the file offset");
        VmRegionTable::release_snapshot(snapshot);
        ok &= check(backing->reference_count() == 3,
                    "split region retains an independent backing reference");
        ok &= check(table.remove(0x10000, 0x12000) == VmRegionResult::Success,
                    "left split can be removed");
        ok &= check(table.remove(0x14000, 0x16000) == VmRegionResult::Success,
                    "right split can be removed");
        ok &= check(table.count() == 0 && backing->reference_count() == 1,
                    "removal releases all region references");

        std::atomic<bool> insert_ok{true};
        std::thread first([&] {
            VmRegion region{VM_PERM_READ | VM_PERM_WRITE, VmRegionType::Anonymous,
                            VmRegionSharing::Private, 0, 0x20000, 0x21000, 0, nullptr};
            if (table.insert(region) != VmRegionResult::Success) insert_ok.store(false);
        });
        std::thread second([&] {
            VmRegion region{VM_PERM_READ, VmRegionType::Stack, VmRegionSharing::Private, 0,
                            0x30000, 0x31000, 0, nullptr};
            if (table.insert(region) != VmRegionResult::Success) insert_ok.store(false);
        });
        first.join();
        second.join();
        ok &= check(insert_ok.load() && table.count() == 2,
                    "concurrent disjoint region insertion is serialized safely");
        table.clear();
    }
    backing->release();
    ok &= check(deleted.load() == 1, "backing object is destroyed after its final reference");
    if (ok) std::puts("VM region validation, splitting, and lifetime checks passed.");
    return ok;
}
