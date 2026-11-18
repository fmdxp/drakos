#include "vfs.hpp"
#include "vga.hpp"
#include "panic.hpp"
#include "drivers/lapic.hpp"
#include "fs/page_cache.hpp"

extern VGA* g_vga;

namespace VFS {

static VFSManager g_instance;
VFSManager& VFSManager::get() { return g_instance; }

static uint32_t vfs_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static bool vfs_streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void vfs_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static bool vfs_path_starts_with(const char* path, const char* prefix) {
    uint32_t plen = vfs_strlen(prefix);
    for (uint32_t i = 0; i < plen; ++i)
        if (path[i] != prefix[i]) return false;
    return path[plen] == '/' || path[plen] == '\0';
}

FileObject::FileObject(Node* owned_node, uint32_t flags)
    : KernelObject(KernelObjectType::File), m_node(owned_node), m_flags(flags),
      m_offset(0), m_lock(false) {}

FileObject::~FileObject() {
    if (m_node && m_node->fs) m_node->fs->release(m_node);
    m_node = nullptr;
}

void FileObject::lock(uint64_t& flags, uint32_t& saved_tpr) {
#ifdef DRAKOS_HOST_TEST
    flags = 0;
    saved_tpr = 0;
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE)) {}
#else
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    saved_tpr = lapic_lock_wait_begin();
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE))
        asm volatile("sti; pause; cli" ::: "memory");
    if (g_lapic) asm volatile("sti" ::: "memory");
#endif
}

void FileObject::unlock(uint64_t flags, uint32_t saved_tpr) {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    asm volatile("cli" ::: "memory");
    lapic_lock_wait_end(saved_tpr);
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
    (void)saved_tpr;
#endif
}

int FileObject::read_at_locked(uint64_t offset, void* buffer, uint32_t size) {
    if (!m_node || !m_node->fs || m_node->type != NodeType::FILE ||
        !(m_flags & VFS_O_READ) || (!buffer && size) || offset > UINT32_MAX ||
        size > UINT32_MAX - offset) return -1;
    if (offset >= m_node->size) return 0;
    uint64_t available = m_node->size - offset;
    if (size > available) size = static_cast<uint32_t>(available);
    return m_node->fs->read(m_node, buffer, static_cast<uint32_t>(offset), size);
}

int FileObject::write_at_locked(uint64_t offset, const void* buffer, uint32_t size) {
    if (!m_node || !m_node->fs || m_node->type != NodeType::FILE || !(m_flags & VFS_O_WRITE) ||
        (!buffer && size) || offset > UINT32_MAX || size > UINT32_MAX - offset) return -1;
    int written = m_node->fs->write(m_node, buffer, static_cast<uint32_t>(offset), size);
    if (written > 0 && offset + static_cast<uint32_t>(written) > m_node->size)
        m_node->size = offset + static_cast<uint32_t>(written);
    return written;
}

int FileObject::read(void* buffer, uint32_t size) {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    int result = read_at_locked(m_offset, buffer, size);
    if (result > 0) m_offset += static_cast<uint32_t>(result);
    unlock(flags, saved_tpr);
    return result;
}

int FileObject::read_at(uint64_t offset, void* buffer, uint32_t size) {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    int result = read_at_locked(offset, buffer, size);
    unlock(flags, saved_tpr);
    return result;
}

int FileObject::write(const void* buffer, uint32_t size) {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    uint64_t offset = m_offset;
    int result = write_at_locked(offset, buffer, size);
    if (result > 0) m_offset += static_cast<uint32_t>(result);
    unlock(flags, saved_tpr);
    if (result > 0) page_cache_update(identity(), offset, buffer, static_cast<uint32_t>(result));
    return result;
}

int FileObject::write_at(uint64_t offset, const void* buffer, uint32_t size) {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    int result = write_at_locked(offset, buffer, size);
    unlock(flags, saved_tpr);
    if (result > 0) page_cache_update(identity(), offset, buffer, static_cast<uint32_t>(result));
    return result;
}

int FileObject::seek(int32_t offset, int whence) {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    if (!m_node) {
        unlock(flags, saved_tpr);
        return -1;
    }
    int64_t base = 0;
    if (whence == VFS_SEEK_CUR) base = m_offset;
    else if (whence == VFS_SEEK_END) base = static_cast<int64_t>(m_node->size);
    else if (whence != VFS_SEEK_SET) {
        unlock(flags, saved_tpr);
        return -1;
    }
    int64_t destination = base + offset;
    if (destination < 0 || destination > UINT32_MAX) {
        unlock(flags, saved_tpr);
        return -1;
    }
    m_offset = static_cast<uint32_t>(destination);
    unlock(flags, saved_tpr);
    return static_cast<int>(m_offset);
}

uint64_t FileObject::size() {
    uint64_t flags;
    uint32_t saved_tpr;
    lock(flags, saved_tpr);
    uint64_t result = m_node ? m_node->size : 0;
    unlock(flags, saved_tpr);
    return result;
}

uint64_t FileObject::identity() const {
    if (!m_node || !m_node->fs) return 0;
    return m_node->fs->file_identity(m_node);
}

uint64_t VFSManager::lock_manager() {
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

void VFSManager::unlock_manager(uint64_t flags) {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

int VFSManager::alloc_fd() {
    for (int i = 3; i < MAX_FDS; ++i)
        if (!m_fds[i].used) return i;
    return -1;
}

bool VFSManager::mount(const char* path, FileSystem* fs) {
    if (!path || !path[0] || !fs || vfs_strlen(path) >= sizeof(m_mounts[0].path)) return false;
    uint64_t flags = lock_manager();
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (m_mounts[i].used && vfs_streq(m_mounts[i].path, path)) {
            unlock_manager(flags);
            return false;
        }
    }
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (m_mounts[i].used) continue;
        vfs_strcpy(m_mounts[i].path, path, sizeof(m_mounts[i].path));
        m_mounts[i].fs = fs;
        m_mounts[i].used = true;
        unlock_manager(flags);
        return true;
    }
    unlock_manager(flags);
    if (g_vga) g_vga->write("VFS: mount table full!\n");
    return false;
}

bool VFSManager::unmount(const char* path) {
    if (!path) return false;
    uint64_t flags = lock_manager();
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (m_mounts[i].used && vfs_streq(m_mounts[i].path, path)) {
            m_mounts[i].used = false;
            m_mounts[i].fs = nullptr;
            unlock_manager(flags);
            return true;
        }
    }
    unlock_manager(flags);
    return false;
}

FileSystem* VFSManager::resolve(const char* path, char* rel_out, size_t rel_capacity) {
    if (!path || !rel_out || !rel_capacity) return nullptr;
    int best_len = -1;
    FileSystem* best_fs = nullptr;
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (!m_mounts[i].used) continue;
        uint32_t len = vfs_strlen(m_mounts[i].path);
        if (static_cast<int>(len) > best_len && vfs_path_starts_with(path, m_mounts[i].path)) {
            best_len = static_cast<int>(len);
            best_fs = m_mounts[i].fs;
        }
    }
    if (!best_fs) return nullptr;
    const char* relative = path + best_len;
    if (*relative == '/') ++relative;
    uint32_t length = vfs_strlen(relative);
    if (length >= rel_capacity) return nullptr;
    for (uint32_t i = 0; i <= length; ++i) rel_out[i] = relative[i];
    return best_fs;
}

FileObject* VFSManager::open_file(const char* path, int flags) {
    if (!path) return nullptr;
    if (flags == 0) flags = VFS_O_READ;
    constexpr int allowed = VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE;
    if ((flags & ~allowed) || !(flags & (VFS_O_READ | VFS_O_WRITE))) return nullptr;

    char rel[256];
    uint64_t lock_flags = lock_manager();
    FileSystem* fs = resolve(path, rel, sizeof(rel));
    unlock_manager(lock_flags);
    if (!fs) return nullptr;

    Node* node = fs->lookup(rel);
    if (!node && (flags & VFS_O_CREATE)) node = fs->create(rel, false);
    if (!node) return nullptr;
    if (node->type != NodeType::FILE || node->fs != fs) {
        fs->release(node);
        return nullptr;
    }
    FileObject* file = new FileObject(node, static_cast<uint32_t>(flags));
    if (!file) fs->release(node);
    return file;
}

int VFSManager::open(const char* path, int flags) {
    FileObject* file = open_file(path, flags);
    if (!file) return -1;
    uint64_t lock_flags = lock_manager();
    int fd = alloc_fd();
    if (fd >= 0) {
        m_fds[fd].file = file; // transfer the initial reference to the descriptor
        m_fds[fd].used = true;
    }
    unlock_manager(lock_flags);
    if (fd < 0) file->release();
    return fd;
}

int VFSManager::read(int fd, void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uint64_t flags = lock_manager();
    FileObject* file = m_fds[fd].used ? m_fds[fd].file : nullptr;
    bool retained = file && file->retain();
    unlock_manager(flags);
    if (!retained) return -1;
    int result = file->read(buffer, size);
    file->release();
    return result;
}

int VFSManager::write(int fd, const void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uint64_t flags = lock_manager();
    FileObject* file = m_fds[fd].used ? m_fds[fd].file : nullptr;
    bool retained = file && file->retain();
    unlock_manager(flags);
    if (!retained) return -1;
    int result = file->write(buffer, size);
    file->release();
    return result;
}

int VFSManager::seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uint64_t flags = lock_manager();
    FileObject* file = m_fds[fd].used ? m_fds[fd].file : nullptr;
    bool retained = file && file->retain();
    unlock_manager(flags);
    if (!retained) return -1;
    int result = file->seek(offset, whence);
    file->release();
    return result;
}

int VFSManager::close(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uint64_t flags = lock_manager();
    if (!m_fds[fd].used || !m_fds[fd].file) {
        unlock_manager(flags);
        return -1;
    }
    FileObject* file = m_fds[fd].file;
    m_fds[fd] = {};
    unlock_manager(flags);
    file->release();
    return 0;
}

int64_t VFSManager::filesize(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    uint64_t flags = lock_manager();
    FileObject* file = m_fds[fd].used ? m_fds[fd].file : nullptr;
    bool retained = file && file->retain();
    unlock_manager(flags);
    if (!retained) return -1;
    uint64_t length = file->size();
    file->release();
    return static_cast<int64_t>(length);
}

bool VFSManager::listdir(const char* path, DirEntry* out, uint32_t max, uint32_t& count) {
    if (!path || (!out && max)) return false;
    char rel[256];
    uint64_t flags = lock_manager();
    FileSystem* fs = resolve(path, rel, sizeof(rel));
    unlock_manager(flags);
    if (!fs) return false;
    Node* dir = fs->lookup((rel[0] == '\0') ? "." : rel);
    if (!dir) return false;
    bool ok = fs->listdir(dir, out, max, count);
    fs->release(dir);
    return ok;
}

} // namespace VFS

bool vfs_mount(const char* path, VFS::FileSystem* fs) {
    return VFS::VFSManager::get().mount(path, fs);
}
int vfs_open(const char* path, int flags) {
    return VFS::VFSManager::get().open(path, flags);
}
VFS::FileObject* vfs_open_file(const char* path, int flags) {
    return VFS::VFSManager::get().open_file(path, flags);
}
int vfs_read(int fd, void* buffer, uint32_t size) {
    return VFS::VFSManager::get().read(fd, buffer, size);
}
int vfs_write(int fd, const void* buffer, uint32_t size) {
    return VFS::VFSManager::get().write(fd, buffer, size);
}
int vfs_seek(int fd, int32_t offset, int whence) {
    return VFS::VFSManager::get().seek(fd, offset, whence);
}
int vfs_close(int fd) { return VFS::VFSManager::get().close(fd); }
int64_t vfs_filesize(int fd) { return VFS::VFSManager::get().filesize(fd); }
bool vfs_listdir(const char* path, VFS::DirEntry* out, uint32_t max, uint32_t& count) {
    return VFS::VFSManager::get().listdir(path, out, max, count);
}
