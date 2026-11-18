#pragma once

#include <stdint.h>

enum class KernelObjectType : uint8_t {
    Any = 0,
    File,
    SharedMemory,
};

enum KernelObjectRight : uint32_t {
    HANDLE_RIGHT_READ      = 1u << 0,
    HANDLE_RIGHT_WRITE     = 1u << 1,
    HANDLE_RIGHT_MAP       = 1u << 2,
    HANDLE_RIGHT_DUPLICATE = 1u << 3,
    HANDLE_RIGHT_ADMIN     = 1u << 4,
};

class KernelObject {
public:
    explicit KernelObject(KernelObjectType type);
    virtual ~KernelObject() = default;

    KernelObjectType type() const { return m_type; }
    bool retain();
    void release();
    uint32_t reference_count() const;

private:
    KernelObjectType m_type;
    volatile uint32_t m_references;
};

class KernelHandleTable {
public:
    static constexpr uint32_t Capacity = 64;

    KernelHandleTable();
    ~KernelHandleTable();
    KernelHandleTable(const KernelHandleTable&) = delete;
    KernelHandleTable& operator=(const KernelHandleTable&) = delete;

    // install retains object; the caller keeps and must release its own reference.
    int32_t install(KernelObject* object, uint32_t rights);
    KernelObject* acquire(int32_t handle, KernelObjectType expected,
                          uint32_t required_rights = 0);
    int32_t duplicate(int32_t handle);
    int32_t duplicate(int32_t handle, uint32_t rights);
    bool close(int32_t handle);
    void close_all();

private:
    struct Entry {
        KernelObject* object;
        uint32_t generation;
        uint8_t rights;
    };

    Entry m_entries[Capacity];
    volatile bool m_lock;

    uint64_t lock();
    void unlock(uint64_t flags);
    static bool decode(int32_t handle, uint32_t& index, uint32_t& generation);
    static int32_t encode(uint32_t index, uint32_t generation);
    static void advance_generation(Entry& entry);
};
