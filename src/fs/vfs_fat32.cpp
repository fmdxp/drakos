#include "fs/vfs_fat32.hpp"
#include "vga.hpp"
#include "pmm.hpp"
#include "heap.hpp"

extern VGA* g_vga;

namespace VFS {

// ── helpers ────────────────────────────────────────────────────────────────

static uint32_t fat_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

// Split first path component from rest.
// "subdir/file.txt" → component="subdir", rest="file.txt"
// "file.txt"        → component="file.txt", rest=""
static void split_first(const char* path, char* component, const char** rest) {
    uint32_t i = 0;
    while (path[i] && path[i] != '/') { component[i] = path[i]; i++; }
    component[i] = 0;
    *rest = (path[i] == '/') ? path + i + 1 : path + i;
}

// ── Fat32FS ────────────────────────────────────────────────────────────────

Fat32FS::Fat32FS(FS::BlockDevice* dev) : m_fat(dev) {}

bool Fat32FS::init() {
    return m_fat.mount();
}

Fat32FileInfo* Fat32FS::alloc_info(uint32_t cluster, uint32_t size, bool is_dir) {
    // Use kernel heap
    Fat32FileInfo* info = new Fat32FileInfo();
    info->first_cluster = cluster;
    info->file_size     = size;
    info->is_root_dir   = is_dir && (cluster == m_fat.root_cluster());
    return info;
}

Node* Fat32FS::lookup(const char* rel_path) {
    // Empty or "." → root directory
    if (!rel_path || rel_path[0] == '\0' || (rel_path[0] == '.' && rel_path[1] == '\0')) {
        Node* n       = new Node();
        n->type       = NodeType::DIRECTORY;
        n->size       = 0;
        n->fs         = this;
        n->fs_data    = alloc_info(m_fat.root_cluster(), 0, true);
        n->name[0]    = '/'; n->name[1] = 0;
        return n;
    }

    // Walk path components: "subdir/file.txt"
    uint32_t dir_cluster = m_fat.root_cluster();
    const char* p = rel_path;

    while (*p) {
        char component[256] = {};
        const char* rest = nullptr;
        split_first(p, component, &rest);

        uint32_t next_cluster = 0, next_size = 0;
        bool is_dir = false;

        if (!m_fat.lookup_entry(dir_cluster, component, next_cluster, next_size, is_dir))
            return nullptr; // Not found

        if (*rest == '\0') {
            // This is the final component
            Node* n    = new Node();
            n->type    = is_dir ? NodeType::DIRECTORY : NodeType::FILE;
            n->size    = is_dir ? 0 : next_size;
            n->fs      = this;
            n->fs_data = alloc_info(next_cluster, next_size, is_dir);

            // Copy name
            uint32_t nlen = fat_strlen(component);
            for (uint32_t i = 0; i <= nlen && i < 255; i++) n->name[i] = component[i];
            return n;
        }

        if (!is_dir) return nullptr; // Tried to traverse a file as directory
        dir_cluster = next_cluster;
        p = rest;
    }
    return nullptr;
}

int Fat32FS::read(Node* node, void* buf, uint32_t offset, uint32_t size) {
    if (!node || node->type != NodeType::FILE) return -1;
    Fat32FileInfo* info = static_cast<Fat32FileInfo*>(node->fs_data);
    return m_fat.read_at(info->first_cluster, info->file_size, buf, offset, size);
}

int Fat32FS::write(Node* /*node*/, const void* /*buf*/, uint32_t /*offset*/, uint32_t /*size*/) {
    // FAT32 write not yet implemented
    if (g_vga) g_vga->write("VFS/FAT32: write not supported yet\n");
    return -1;
}

bool Fat32FS::listdir(Node* dir, DirEntry* out, uint32_t max, uint32_t& count) {
    if (!dir || dir->type != NodeType::DIRECTORY) return false;
    Fat32FileInfo* info = static_cast<Fat32FileInfo*>(dir->fs_data);

    // Use a temporary buffer from listdir_cluster
    // DirEnum is defined inline in the FAT32Driver signature
    using DE = FAT32::FAT32Driver::DirEnum;
    // Allocate stack buffer for up to 'max' entries
    // Since we can't do VLAs and stack is limited, cap at 64 per call
    const uint32_t CHUNK = 64;
    uint32_t total = 0;

    // We need to bridge FAT32::DirEnum → VFS::DirEntry
    // Allocate a small buffer
    DE raw[64];
    uint32_t raw_count = 0;

    // Pass a DirEnum pointer: the type is defined inline in fat32.hpp
    // We need to call m_fat.listdir_cluster but it uses the inner struct type.
    // Since DirEnum is a public nested-struct in FAT32Driver, cast properly.
    m_fat.listdir_cluster(info->first_cluster,
        reinterpret_cast<FAT32::FAT32Driver::DirEnum*>(raw), CHUNK, raw_count);

    for (uint32_t i = 0; i < raw_count && total < max; i++, total++) {
        for (uint32_t j = 0; j < 256 && raw[i].name[j]; j++)
            out[total].name[j] = raw[i].name[j];
        out[total].name[255] = 0;
        out[total].type = raw[i].is_dir ? NodeType::DIRECTORY : NodeType::FILE;
        out[total].size = raw[i].size;
    }
    count = total;
    return true;
}

void Fat32FS::release(Node* node) {
    if (!node) return;
    delete static_cast<Fat32FileInfo*>(node->fs_data);
    delete node;
}

} // namespace VFS
