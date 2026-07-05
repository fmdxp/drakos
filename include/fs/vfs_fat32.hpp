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
#include "fs/vfs.hpp"
#include "fs/fat32.hpp"
#include "fs/block_device.hpp"

namespace VFS {

// Per-file metadata stored in Node::fs_data
struct Fat32FileInfo {
    uint32_t first_cluster; // Starting cluster
    uint32_t file_size;     // Bytes
    bool     is_root_dir;   // True if this is the root directory node
};

// FAT32 VFS Adapter: wraps a FAT32::FAT32Driver and exposes VFS::FileSystem
class Fat32FS : public FileSystem {
public:
    explicit Fat32FS(FS::BlockDevice* dev);

    // Mount the FAT32 volume; returns false if not FAT32
    bool init();

    // VFS::FileSystem interface
    Node* lookup(const char* rel_path) override;
    int   read(Node* node, void* buf, uint32_t offset, uint32_t size) override;
    int   write(Node* node, const void* buf, uint32_t offset, uint32_t size) override;
    bool  listdir(Node* dir, DirEntry* out, uint32_t max, uint32_t& count) override;
    void  release(Node* node) override;

private:
    FAT32::FAT32Driver m_fat;

    // Reads 'size' bytes from file starting at cluster 'first_cluster'
    // beginning at byte 'offset' within the file
    int read_at(uint32_t first_cluster, uint32_t file_size,
                void* buf, uint32_t offset, uint32_t size);

    // Walk path components (e.g. "subdir/FILE.TXT") from a starting dir cluster
    // Returns the cluster + size of the target, or 0 if not found
    bool walk_path(const char* rel_path, uint32_t dir_cluster,
                   uint32_t& out_cluster, uint32_t& out_size, bool& out_is_dir);

    // Allocate a Fat32FileInfo via heap (uses pmm_alloc_page)
    Fat32FileInfo* alloc_info(uint32_t cluster, uint32_t size, bool is_dir);
};

} // namespace VFS
