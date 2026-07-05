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
#include "block_device.hpp"

namespace FAT32 {

// FAT32 Boot Parameter Block
struct BPB {
    uint8_t  jmp_boot[3];       // 0x00 - Jump instruction
    uint8_t  oem_name[8];       // 0x03 - OEM name
    uint16_t bytes_per_sector;  // 0x0B - Bytes per sector (usually 512)
    uint8_t  sectors_per_cluster; // 0x0D
    uint16_t reserved_sectors;  // 0x0E - Reserved sectors (includes boot sector)
    uint8_t  num_fats;          // 0x10 - Number of FAT copies (usually 2)
    uint16_t root_entry_count;  // 0x11 - FAT12/16 only, 0 for FAT32
    uint16_t total_sectors_16;  // 0x13 - FAT12/16 only, 0 for FAT32
    uint8_t  media_type;        // 0x15
    uint16_t fat_size_16;       // 0x16 - FAT12/16 only, 0 for FAT32
    uint16_t sectors_per_track; // 0x18
    uint16_t num_heads;         // 0x1A
    uint32_t hidden_sectors;    // 0x1C
    uint32_t total_sectors_32;  // 0x20

    // FAT32 extended BPB
    uint32_t fat_size_32;       // 0x24 - Sectors per FAT
    uint16_t ext_flags;         // 0x28
    uint16_t fs_version;        // 0x2A
    uint32_t root_cluster;      // 0x2C - Root directory cluster (usually 2)
    uint16_t fs_info;           // 0x30
    uint16_t backup_boot_sec;   // 0x32
    uint8_t  reserved[12];      // 0x34
    uint8_t  drive_number;      // 0x40
    uint8_t  reserved1;         // 0x41
    uint8_t  boot_sig;          // 0x42
    uint32_t volume_id;         // 0x43
    uint8_t  volume_label[11];  // 0x47
    uint8_t  fs_type[8];        // 0x52 - "FAT32   "
} __attribute__((packed));

// Directory Entry (32 bytes)
struct DirEntry {
    uint8_t  name[8];           // Short name
    uint8_t  ext[3];            // Extension
    uint8_t  attr;              // Attributes
    uint8_t  reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_acc_date;
    uint16_t first_cluster_hi;  // High 16 bits of first cluster
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;  // Low 16 bits of first cluster
    uint32_t file_size;
} __attribute__((packed));

// Directory Entry attributes
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F  // Long File Name entry

// FAT32 cluster values
#define FAT32_EOC       0x0FFFFFF8  // End of chain
#define FAT32_FREE      0x00000000
#define FAT32_BAD       0x0FFFFFF7

class FAT32Driver {
public:
    FAT32Driver(FS::BlockDevice* dev);
    
    bool mount();
    void list_root();
    bool read_file(const char* name, void* buf, uint32_t max_size, uint32_t& out_size);

    // ── VFS-facing API ───────────────────────────────────────────────────────

    // Find a file/directory in a given dir cluster by 8.3 name component.
    // Returns true and fills out_cluster, out_size, out_is_dir on success.
    bool lookup_entry(uint32_t dir_cluster, const char* name83,
                      uint32_t& out_cluster, uint32_t& out_size, bool& out_is_dir);

    // Read 'size' bytes from a file at byte 'offset', given its first cluster.
    // Walks the FAT chain to skip clusters as needed.
    // Returns bytes read or -1 on error.
    int  read_at(uint32_t first_cluster, uint32_t file_size,
                 void* buf, uint32_t offset, uint32_t size);

    struct DirEnum { char name[13]; bool is_dir; uint32_t size; };

    // Enumerate directory contents (one cluster page at a time).
    // Calls 'cb' for each valid, non-deleted, non-LFN entry.
    // 'entries' and 'max' are the caller-provided output buffer.
    bool listdir_cluster(uint32_t dir_cluster,
                         DirEnum* entries,
                         uint32_t max, uint32_t& count);

    uint32_t root_cluster() const { return m_root_cluster; }

private:
    FS::BlockDevice* m_dev;
    BPB m_bpb;
    
    uint32_t m_fat_start_lba;
    uint32_t m_data_start_lba;
    uint32_t m_sectors_per_cluster;
    uint32_t m_root_cluster;
    
    uint32_t cluster_to_lba(uint32_t cluster);
    uint32_t next_cluster(uint32_t cluster);
    bool read_cluster(uint32_t cluster, void* buf);
    bool read_sectors(uint64_t lba, uint32_t count, void* buf);
    
    // Simple string compare for 8.3 names
    bool match_83(const DirEntry* entry, const char* name);
    // Build 8.3 printable name into buf[13]
    void build_83(const DirEntry* entry, char* buf);
};


} // namespace FAT32
