/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "fs/vfs.hpp"
#include "vga.hpp"

extern VGA* g_vga;

namespace VFS {

// ─── Singleton ───────────────────────────────────────────────────────────────
static VFSManager g_instance;
VFSManager& VFSManager::get() { return g_instance; }

// ─── String helpers (no stdlib) ──────────────────────────────────────────────
static uint32_t vfs_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}
static bool vfs_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void vfs_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
// strncmp prefix: does path start with prefix + '/' (or exact match)?
static bool vfs_path_starts_with(const char* path, const char* prefix) {
    uint32_t plen = vfs_strlen(prefix);
    for (uint32_t i = 0; i < plen; i++)
        if (path[i] != prefix[i]) return false;
    return path[plen] == '/' || path[plen] == '\0';
}

// ─── alloc_fd ────────────────────────────────────────────────────────────────
int VFSManager::alloc_fd() {
    // Reserve 0,1,2 for stdin/stdout/stderr (future use)
    for (int i = 3; i < MAX_FDS; i++) {
        if (!m_fds[i].used) return i;
    }
    return -1; // Out of FDs
}

// ─── mount / unmount ─────────────────────────────────────────────────────────
bool VFSManager::mount(const char* path, FileSystem* fs) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!m_mounts[i].used) {
            vfs_strcpy(m_mounts[i].path, path, 256);
            m_mounts[i].fs   = fs;
            m_mounts[i].used = true;
            if (g_vga) {
                g_vga->write("VFS: Mounted ");
                g_vga->write(path);
                g_vga->write("\n");
            }
            return true;
        }
    }
    if (g_vga) g_vga->write("VFS: mount table full!\n");
    return false;
}

bool VFSManager::unmount(const char* path) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (m_mounts[i].used && vfs_streq(m_mounts[i].path, path)) {
            m_mounts[i].used = false;
            return true;
        }
    }
    return false;
}

// ─── resolve: find longest-prefix matching mount ──────────────────────────────
FileSystem* VFSManager::resolve(const char* path, const char** rel_out) {

    int         best_len = -1;
    FileSystem* best_fs = nullptr;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!m_mounts[i].used) continue;
        uint32_t plen = vfs_strlen(m_mounts[i].path);
        if (vfs_path_starts_with(path, m_mounts[i].path)) {
            if ((int)plen > best_len) {
                best_len  = (int)plen;
                best_fs   = m_mounts[i].fs;
            }
        }
    }


    if (!best_fs) return nullptr;

    // Advance past mount prefix; skip leading '/'
    *rel_out = path + best_len;
    if (**rel_out == '/') (*rel_out)++;
    
    return best_fs;
}

// ─── open ────────────────────────────────────────────────────────────────────
int VFSManager::open(const char* path, int flags) {
    const char* rel = nullptr;
    FileSystem* fs  = resolve(path, &rel);
    if (!fs) {
        if (g_vga) { g_vga->write("VFS: no mount for "); g_vga->write(path); g_vga->write("\n"); }
        return -1;
    }

    Node* node = fs->lookup(rel);
    if (!node) {
        if (g_vga) { g_vga->write("VFS: not found: "); g_vga->write(path); g_vga->write("\n"); }
        return -1;
    }

    int fd = alloc_fd();
    if (fd < 0) { fs->release(node); return -1; }

    m_fds[fd].node   = node;
    m_fds[fd].offset = 0;
    m_fds[fd].flags  = flags;
    m_fds[fd].used   = true;
    return fd;
}

// ─── read ────────────────────────────────────────────────────────────────────
int VFSManager::read(int fd, void* buf, uint32_t size) {
    if (fd < 0 || fd >= MAX_FDS || !m_fds[fd].used) return -1;
    if (!(m_fds[fd].flags & VFS_O_READ)) return -1;

    FileDescriptor& desc = m_fds[fd];
    Node*  node = desc.node;

    // Clamp to file size
    if (desc.offset >= (uint32_t)node->size) return 0;
    if (desc.offset + size > (uint32_t)node->size)
        size = (uint32_t)node->size - desc.offset;

    int n = node->fs->read(node, buf, desc.offset, size);
    if (n > 0) desc.offset += (uint32_t)n;
    return n;
}

// ─── write ───────────────────────────────────────────────────────────────────
int VFSManager::write(int fd, const void* buf, uint32_t size) {
    if (fd < 0 || fd >= MAX_FDS || !m_fds[fd].used) return -1;
    if (!(m_fds[fd].flags & VFS_O_WRITE)) return -1;

    FileDescriptor& desc = m_fds[fd];
    int n = desc.node->fs->write(desc.node, buf, desc.offset, size);
    if (n > 0) desc.offset += (uint32_t)n;
    return n;
}

// ─── seek ────────────────────────────────────────────────────────────────────
int VFSManager::seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS || !m_fds[fd].used) return -1;
    FileDescriptor& desc = m_fds[fd];
    uint32_t new_off = 0;
    switch (whence) {
        case VFS_SEEK_SET: new_off = (uint32_t)offset; break;
        case VFS_SEEK_CUR: new_off = desc.offset + (uint32_t)offset; break;
        case VFS_SEEK_END: new_off = (uint32_t)desc.node->size + (uint32_t)offset; break;
        default: return -1;
    }
    desc.offset = new_off;
    return (int)new_off;
}

// ─── close ───────────────────────────────────────────────────────────────────
int VFSManager::close(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !m_fds[fd].used) return -1;
    FileDescriptor& desc = m_fds[fd];
    desc.node->fs->release(desc.node);
    desc.node   = nullptr;
    desc.offset = 0;
    desc.flags  = 0;
    desc.used   = false;
    return 0;
}

// ─── filesize ────────────────────────────────────────────────────────────────
int64_t VFSManager::filesize(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !m_fds[fd].used) return -1;
    return (int64_t)m_fds[fd].node->size;
}

// ─── listdir ─────────────────────────────────────────────────────────────────
bool VFSManager::listdir(const char* path, DirEntry* out, uint32_t max, uint32_t& count) {
    const char* rel = nullptr;
    FileSystem* fs  = resolve(path, &rel);
    if (!fs) return false;

    // If rel is empty we're listing the mount root (pass "" or ".")
    const char* dir_path = (rel[0] == '\0') ? "." : rel;
    Node* dir = fs->lookup(dir_path);
    if (!dir) return false;

    bool ok = fs->listdir(dir, out, max, count);
    fs->release(dir);
    return ok;
}

} // namespace VFS

// ─── Global API ──────────────────────────────────────────────────────────────
bool    vfs_mount(const char* path, VFS::FileSystem* fs) { return VFS::VFSManager::get().mount(path, fs); }
int     vfs_open(const char* path, int flags)            { return VFS::VFSManager::get().open(path, flags); }
int     vfs_read(int fd, void* buf, uint32_t size)       { return VFS::VFSManager::get().read(fd, buf, size); }
int     vfs_write(int fd, const void* buf, uint32_t size){ return VFS::VFSManager::get().write(fd, buf, size); }
int     vfs_seek(int fd, int32_t offset, int whence)     { return VFS::VFSManager::get().seek(fd, offset, whence); }
int     vfs_close(int fd)                                { return VFS::VFSManager::get().close(fd); }
int64_t vfs_filesize(int fd)                             { return VFS::VFSManager::get().filesize(fd); }
bool    vfs_listdir(const char* path, VFS::DirEntry* out, uint32_t max, uint32_t& count) {
    return VFS::VFSManager::get().listdir(path, out, max, count);
}
