#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "fs/vfs.hpp"
#include "kernel/handle.hpp"
#include "vga.hpp"

VGA* g_vga = nullptr;
void VGA::write(const char*) {}
uint32_t page_cache_invalidate(uint64_t, uint64_t, uint64_t) { return 0; }
void page_cache_update(uint64_t, uint64_t, const void*, uint32_t) {}

class TestFs final : public VFS::FileSystem {
public:
    std::atomic<uint32_t> released{0};
    uint8_t bytes[2][32] = {};

    TestFs() {
        for (uint32_t i = 0; i < sizeof(bytes[0]); ++i) {
            bytes[0][i] = static_cast<uint8_t>('A' + i % 26);
            bytes[1][i] = static_cast<uint8_t>('a' + i % 26);
        }
    }

    VFS::Node* lookup(const char* path) override {
        int index = path && path[0] == 'b' ? 1 : 0;
        auto* node = new VFS::Node{};
        if (!node) return nullptr;
        node->name[0] = index ? 'b' : 'a';
        node->name[1] = 0;
        node->type = VFS::NodeType::FILE;
        node->size = sizeof(bytes[index]);
        node->fs_data = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));
        node->fs = this;
        return node;
    }

    VFS::Node* create(const char*, bool) override { return lookup("a"); }

    int read(VFS::Node* node, void* out, uint32_t offset, uint32_t size) override {
        if (!node || !out || offset >= sizeof(bytes[0])) return 0;
        int index = static_cast<int>(reinterpret_cast<uintptr_t>(node->fs_data)) - 1;
        if (size > sizeof(bytes[index]) - offset) size = sizeof(bytes[index]) - offset;
        std::memcpy(out, bytes[index] + offset, size);
        return static_cast<int>(size);
    }

    int write(VFS::Node* node, const void* data, uint32_t offset, uint32_t size) override {
        if (!node || !data || offset > sizeof(bytes[0]) || size > sizeof(bytes[0]) - offset)
            return -1;
        int index = static_cast<int>(reinterpret_cast<uintptr_t>(node->fs_data)) - 1;
        std::memcpy(bytes[index] + offset, data, size);
        return static_cast<int>(size);
    }

    bool listdir(VFS::Node*, VFS::DirEntry*, uint32_t, uint32_t& count) override {
        count = 0;
        return true;
    }

    void release(VFS::Node* node) override {
        released.fetch_add(1, std::memory_order_relaxed);
        delete node;
    }

    uint64_t file_identity(const VFS::Node* node) const override {
        return reinterpret_cast<uintptr_t>(node->fs_data);
    }
};

class TestObject final : public KernelObject {
public:
    explicit TestObject(std::atomic<uint32_t>& destroyed)
        : KernelObject(KernelObjectType::SharedMemory), m_destroyed(destroyed) {}
    ~TestObject() override { m_destroyed.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<uint32_t>& m_destroyed;
};

static bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main() {
    TestFs fs;
    bool ok = VFS::VFSManager::get().mount("/test", &fs);
    KernelHandleTable first_process;
    KernelHandleTable second_process;

    VFS::FileObject* file_a = vfs_open_file("/test/a", VFS_O_READ | VFS_O_WRITE);
    VFS::FileObject* file_b = vfs_open_file("/test/b", VFS_O_READ);
    ok &= check(file_a && file_b, "open returns retained file objects");
    int32_t handle_a = first_process.install(file_a,
        HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_DUPLICATE);
    int32_t handle_b = second_process.install(file_b, HANDLE_RIGHT_READ);
    if (file_a) file_a->release();
    if (file_b) file_b->release();
    ok &= check(handle_a == 3 && handle_b == 3,
                "process handle namespaces are independent and preserve initial fd values");

    auto* a = static_cast<VFS::FileObject*>(first_process.acquire(handle_a,
        KernelObjectType::File, HANDLE_RIGHT_READ));
    auto* b = static_cast<VFS::FileObject*>(second_process.acquire(handle_b,
        KernelObjectType::File, HANDLE_RIGHT_READ));
    ok &= check(a && b && a->identity() != b->identity(),
                "same numeric handle resolves independently in each process");
    if (a) a->release();
    if (b) b->release();

    int32_t duplicate = first_process.duplicate(handle_a,
        HANDLE_RIGHT_READ | HANDLE_RIGHT_DUPLICATE);
    ok &= check(duplicate >= 0, "duplicate retains the object with a rights subset");
    ok &= check(first_process.acquire(handle_a, KernelObjectType::File, HANDLE_RIGHT_ADMIN) == nullptr,
                "handle rights are checked on lookup");
    auto* in_flight = static_cast<VFS::FileObject*>(first_process.acquire(handle_a,
        KernelObjectType::File, HANDLE_RIGHT_READ));
    ok &= check(in_flight != nullptr, "operation can retain a file before close");
    ok &= check(first_process.close(handle_a), "close removes one handle reference");
    ok &= check(first_process.acquire(handle_a, KernelObjectType::Any) == nullptr,
                "closed generation is stale immediately");
    char data[4] = {};
    ok &= check(in_flight && in_flight->read(data, sizeof(data)) == 4 && data[0] == 'A',
                "in-flight retained operation survives descriptor close");
    ok &= check(fs.released.load() == 0, "node stays alive while another handle or operation references it");
    if (in_flight) in_flight->release();
    ok &= check(first_process.close(duplicate), "duplicate handle closes independently");
    ok &= check(fs.released.load() == 1, "last file-object reference releases its node exactly once");
    ok &= check(!first_process.close(handle_a), "stale close cannot affect a later object");

    VFS::FileObject* reused_file = vfs_open_file("/test/a", VFS_O_READ);
    int32_t reused_handle = first_process.install(reused_file, HANDLE_RIGHT_READ);
    if (reused_file) reused_file->release();
    ok &= check(reused_handle >= 0 && reused_handle != handle_a,
                "reused slot receives a new generation-tagged handle");

    std::atomic<bool> go{false};
    auto* concurrent = static_cast<VFS::FileObject*>(second_process.acquire(handle_b,
        KernelObjectType::File, HANDLE_RIGHT_READ));
    std::thread reader([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        char byte = 0;
        if (!concurrent || concurrent->read(&byte, 1) != 1 || byte != 'a')
            go.store(false, std::memory_order_release);
    });
    ok &= check(second_process.close(handle_b), "close races safely with a retained operation");
    go.store(true, std::memory_order_release);
    reader.join();
    ok &= check(concurrent != nullptr && go.load(std::memory_order_acquire),
                "reader completed successfully after close raced with its retained reference");
    if (concurrent) concurrent->release();
    ok &= check(fs.released.load() == 2, "concurrent close releases node after the active operation");

    std::atomic<uint32_t> destroyed{0};
    {
        KernelHandleTable table;
        auto* object = new TestObject(destroyed);
        int32_t first = table.install(object, HANDLE_RIGHT_READ | HANDLE_RIGHT_DUPLICATE);
        object->release();
        int32_t second = table.duplicate(first, HANDLE_RIGHT_READ);
        ok &= check(first >= 0 && second >= 0, "generic kernel objects can be duplicated");
        table.close_all();
        ok &= check(destroyed.load() == 1, "process-table destruction releases every remaining object");
    }
    {
        KernelHandleTable terminating_process;
        auto* object = new TestObject(destroyed);
        ok &= check(terminating_process.install(object, HANDLE_RIGHT_READ) >= 0,
                    "process can exit with an open kernel object");
        object->release();
    }
    ok &= check(destroyed.load() == 2, "handle-table destructor closes leaked process handles");

    KernelHandleTable full_table;
    auto* full_object = new TestObject(destroyed);
    uint32_t installed = 0;
    while (full_table.install(full_object, HANDLE_RIGHT_READ) >= 0) ++installed;
    ok &= check(installed == KernelHandleTable::Capacity - 3,
                "handle table enforces capacity");
    ok &= check(full_object->reference_count() == installed + 1,
                "failed handle allocation releases its temporary reference");
    full_table.close_all();
    full_object->release();
    ok &= check(destroyed.load() == 3, "capacity cleanup releases all object references");

    int legacy_fd = vfs_open("/test/a", VFS_O_READ);
    ok &= check(legacy_fd == 3 && vfs_close(legacy_fd) == 0,
                "kernel-side legacy VFS descriptor API remains available");
    ok &= check(VFS::VFSManager::get().unmount("/test"), "test mount can be removed");
    if (!ok) return 1;
    std::puts("VFS file references and process handle-table checks passed.");
    return 0;
}
