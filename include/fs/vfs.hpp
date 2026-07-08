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


#pragma once
#include <stdint.h>
#include <stddef.h>

namespace VFS {

class FileSystem;

// ─── Node ───────────────────────────────────────────────────────────────────
enum class NodeType : uint8_t { FILE = 0, DIRECTORY = 1, DEVICE = 2 };

struct Node {
    char        name[256];
    NodeType    type;
    uint64_t    size;       // bytes (0 for dirs)
    void*       fs_data;    // FS-private (e.g. Fat32FileInfo*)
    FileSystem* fs;
};

// ─── DirEntry ────────────────────────────────────────────────────────────────
struct DirEntry {
    char     name[256];
    NodeType type;
    uint64_t size;
};

// ─── FileSystem (abstract) ───────────────────────────────────────────────────
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // Lookup relative path from mount root → allocates Node (caller owns it)
    virtual Node* lookup(const char* rel_path) = 0;

    // Read size bytes at offset into buf; returns bytes read or <0 on error
    virtual int read(Node* node, void* buf, uint32_t offset, uint32_t size) = 0;

    // Write size bytes at offset; returns bytes written or <0 on error  
    virtual int write(Node* node, const void* buf, uint32_t offset, uint32_t size) = 0;

    // List directory entries
    virtual bool listdir(Node* dir, DirEntry* out, uint32_t max, uint32_t& count) = 0;

    // Create a new file or directory at the given path
    virtual Node* create(const char* rel_path, bool is_dir) = 0;

    // Free node resources
    virtual void release(Node* node) = 0;
};

// ─── Open flags / seek whence ─────────────────────────────────────────────────
#define VFS_O_READ   (1 << 0)
#define VFS_O_WRITE  (1 << 1)
#define VFS_O_CREATE (1 << 2)
#define VFS_O_RDWR   (VFS_O_READ | VFS_O_WRITE)

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

// ─── VFSManager ──────────────────────────────────────────────────────────────
static constexpr int MAX_FDS    = 64;
static constexpr int MAX_MOUNTS = 16;

struct FileDescriptor {
    Node*    node   = nullptr;
    uint32_t offset = 0;
    uint32_t flags  = 0;
    bool     used   = false;
};

struct MountEntry {
    char        path[256] = {};
    FileSystem* fs        = nullptr;
    bool        used      = false;
};

class VFSManager {
public:
    static VFSManager& get();

    bool  mount(const char* path, FileSystem* fs);
    bool  unmount(const char* path);

    int   open(const char* path, int flags = VFS_O_READ);
    int   read(int fd, void* buf, uint32_t size);
    int   write(int fd, const void* buf, uint32_t size);
    int   seek(int fd, int32_t offset, int whence);
    int   close(int fd);
    int64_t filesize(int fd);

    bool  listdir(const char* path, DirEntry* out, uint32_t max, uint32_t& count);

private:
    FileDescriptor m_fds[MAX_FDS]       = {};
    MountEntry     m_mounts[MAX_MOUNTS] = {};

    FileSystem* resolve(const char* path, const char** rel_path_out);
    int         alloc_fd();
};

} // namespace VFS

// ─── Global convenience API ──────────────────────────────────────────────────
bool    vfs_mount(const char* path, VFS::FileSystem* fs);
int     vfs_open(const char* path, int flags = VFS_O_READ);
int     vfs_read(int fd, void* buf, uint32_t size);
int     vfs_write(int fd, const void* buf, uint32_t size);
int     vfs_seek(int fd, int32_t offset, int whence);
int     vfs_close(int fd);
int64_t vfs_filesize(int fd);
bool    vfs_listdir(const char* path, VFS::DirEntry* out, uint32_t max, uint32_t& count);
