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


#include "fs/fat32.hpp"
#include "vga.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

extern VGA* g_vga;

namespace FAT32 {

FAT32Driver::FAT32Driver(FS::BlockDevice* dev) : m_dev(dev) {}

// Read sectors from device via the block device interface
bool FAT32Driver::read_sectors(uint64_t lba, uint32_t count, void* buf) {
    return m_dev->read_blocks(lba, count, buf);
}

bool FAT32Driver::mount() {
    // Read the BPB from sector 0
    uint8_t sector[512];
    if (!read_sectors(0, 1, sector)) {
        if (g_vga) g_vga->write("FAT32: Failed to read boot sector!\n");
        return false;
    }
    
    // Copy BPB
    for (int i = 0; i < (int)sizeof(BPB); i++)
        ((uint8_t*)&m_bpb)[i] = sector[i];
    
    // Validate FAT32 signature
    if (m_bpb.bytes_per_sector != 512) {
        if (g_vga) {
            g_vga->write("FAT32: Non-512 byte sectors not supported! Found: ");
            char buf[8];
            int i = 0;
            uint16_t temp = m_bpb.bytes_per_sector;
            if (temp == 0) buf[i++] = '0';
            while(temp > 0) { buf[i++] = '0' + (temp % 10); temp /= 10; }
            for(int j=0; j<i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
            buf[i] = '\0';
            g_vga->write(buf);
            g_vga->write("\n");
        }
        return false;
    }
    
    // Check FS type string (should be "FAT32   ")
    bool is_fat32 = (m_bpb.fs_type[0] == 'F' && m_bpb.fs_type[1] == 'A' &&
                     m_bpb.fs_type[2] == 'T' && m_bpb.fs_type[3] == '3' &&
                     m_bpb.fs_type[4] == '2');
    
    if (!is_fat32) {
        if (g_vga) g_vga->write("FAT32: Not a FAT32 volume!\n");
        return false;
    }
    
    m_sectors_per_cluster = m_bpb.sectors_per_cluster;
    m_fat_start_lba   = m_bpb.reserved_sectors;
    m_data_start_lba  = m_bpb.reserved_sectors 
                      + m_bpb.num_fats * m_bpb.fat_size_32;
    m_root_cluster    = m_bpb.root_cluster;
    
    return true;
}

uint32_t FAT32Driver::cluster_to_lba(uint32_t cluster) {
    return m_data_start_lba + (cluster - 2) * m_sectors_per_cluster;
}

uint32_t FAT32Driver::next_cluster(uint32_t cluster) {
    // Read the FAT sector containing this cluster's entry
    uint32_t fat_offset   = cluster * 4;
    uint32_t fat_sector   = m_fat_start_lba + (fat_offset / 512);
    uint32_t entry_offset = (fat_offset % 512) / 4;
    
    uint32_t sector[128]; // 512 bytes / 4 bytes per entry = 128 entries
    if (!read_sectors(fat_sector, 1, sector)) return 0x0FFFFFFF;
    
    return sector[entry_offset] & 0x0FFFFFFF;
}

bool FAT32Driver::read_cluster(uint32_t cluster, void* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    return read_sectors(lba, m_sectors_per_cluster, buf);
}

bool FAT32Driver::write_sectors(uint64_t lba, uint32_t count, const void* buf) {
    return m_dev->write_blocks(lba, count, buf);
}

bool FAT32Driver::write_cluster(uint32_t cluster, const void* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    return write_sectors(lba, m_sectors_per_cluster, buf);
}

bool FAT32Driver::write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset   = cluster * 4;
    uint32_t fat_sector   = m_fat_start_lba + (fat_offset / 512);
    uint32_t entry_offset = (fat_offset % 512) / 4;
    
    uint32_t sector[128];
    if (!read_sectors(fat_sector, 1, sector)) return false;
    
    // Preserve top 4 bits of the FAT32 entry
    sector[entry_offset] = (sector[entry_offset] & 0xF0000000) | (value & 0x0FFFFFFF);
    
    // Write back to FAT1
    if (!write_sectors(fat_sector, 1, sector)) return false;
    
    // Write back to FAT2 (if it exists)
    if (m_bpb.num_fats > 1) {
        uint32_t fat2_sector = fat_sector + m_bpb.fat_size_32;
        write_sectors(fat2_sector, 1, sector);
    }
    return true;
}

uint32_t FAT32Driver::allocate_cluster(uint32_t last_cluster) {
    uint32_t max_cluster = m_bpb.total_sectors_32 / m_sectors_per_cluster;
    
    for (uint32_t c = 2; c < max_cluster; c++) {
        if (next_cluster(c) == FAT32_FREE) {
            write_fat_entry(c, FAT32_EOC);
            
            uint8_t* zero_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
            vmm_map((uintptr_t)zero_buf & ~0xFFFULL, (uintptr_t)zero_buf - pmm_hhdm_offset(), VMM_MMIO);
            for (uint32_t i=0; i < m_sectors_per_cluster * 512; i++) zero_buf[i] = 0;
            write_cluster(c, zero_buf);
            
            if (last_cluster >= 2 && last_cluster < 0x0FFFFFF0) {
                write_fat_entry(last_cluster, c);
            }
            return c;
        }
    }
    return 0; // Disk Full
}

bool FAT32Driver::update_dir_entry(uint32_t dir_cluster, const char* name83, uint32_t new_size, uint32_t new_first_cluster) {
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL, (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    uint32_t cluster = dir_cluster;
    while (cluster < FAT32_EOC) {
        if (!read_cluster(cluster, cluster_buf)) break;
        uint32_t n = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* entries = (DirEntry*)cluster_buf;
        for (uint32_t i = 0; i < n; i++) {
            if (entries[i].name[0] == 0x00) return false;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == ATTR_LFN)       continue;
            if (entries[i].attr & ATTR_VOLUME_ID)  continue;
            if (match_83(&entries[i], name83)) {
                entries[i].file_size = new_size;
                if (new_first_cluster != 0) {
                    entries[i].first_cluster_hi = (new_first_cluster >> 16) & 0xFFFF;
                    entries[i].first_cluster_lo = new_first_cluster & 0xFFFF;
                }
                return write_cluster(cluster, cluster_buf);
            }
        }
        cluster = next_cluster(cluster);
    }
    return false;
}

bool FAT32Driver::match_83(const DirEntry* e, const char* name) {
    // Build the 8.3 name from the entry and compare case-insensitively
    char short_name[13];
    int idx = 0;
    // Name part (up to 8 chars, strip trailing spaces)
    int name_len = 8;
    while (name_len > 0 && e->name[name_len - 1] == ' ') name_len--;
    for (int i = 0; i < name_len; i++) {
        char c = e->name[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        short_name[idx++] = c;
    }
    // Extension part
    int ext_len = 3;
    while (ext_len > 0 && e->ext[ext_len - 1] == ' ') ext_len--;
    if (ext_len > 0) {
        short_name[idx++] = '.';
        for (int i = 0; i < ext_len; i++) {
            char c = e->ext[i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            short_name[idx++] = c;
        }
    }
    short_name[idx] = 0;
    
    // Case-insensitive compare with input name
    for (int i = 0; ; i++) {
        char a = short_name[i];
        char b = name[i];
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
        if (a == 0 && b == 0) return true;
    }
}

void FAT32Driver::list_root() {
    if (g_vga) g_vga->write("FAT32: Root directory contents:\n");
    
    // Allocate one cluster buffer (max 4KB for 8 sectors/cluster)
    // We'll use a static 4096-byte buffer
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL, 
            (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);
    
    uint32_t cluster = m_root_cluster;
    while (cluster < FAT32_EOC) {
        if (!read_cluster(cluster, cluster_buf)) break;
        
        uint32_t entries_per_cluster = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* entries = (DirEntry*)cluster_buf;
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) goto done; // End of directory
            if (entries[i].name[0] == 0xE5) continue;  // Deleted entry
            if (entries[i].attr == ATTR_LFN) continue; // Skip LFN entries
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            
            // Print the filename
            char fname[13];
            int fi = 0;
            int nl = 8; while (nl > 0 && entries[i].name[nl-1] == ' ') nl--;
            for (int j = 0; j < nl; j++) fname[fi++] = entries[i].name[j];
            int el = 3; while (el > 0 && entries[i].ext[el-1] == ' ') el--;
            if (el > 0) { fname[fi++] = '.'; for (int j = 0; j < el; j++) fname[fi++] = entries[i].ext[j]; }
            fname[fi] = 0;
            
            if (g_vga) {
                if (entries[i].attr & ATTR_DIRECTORY)
                    g_vga->write("  [DIR]  ");
                else
                    g_vga->write("  [FILE] ");
                g_vga->write(fname);
                g_vga->write("\n");
            }
        }
        
        cluster = next_cluster(cluster);
    }
done:;
}

bool FAT32Driver::read_file(const char* name, void* buf, uint32_t max_size, uint32_t& out_size) {
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL,
            (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);
    
    uint32_t cluster = m_root_cluster;
    DirEntry found_entry = {};
    bool found = false;
    
    // Search root directory for the file
    while (cluster < FAT32_EOC && !found) {
        if (!read_cluster(cluster, cluster_buf)) break;
        
        uint32_t entries_per_cluster = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* entries = (DirEntry*)cluster_buf;
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) goto search_done;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == ATTR_LFN) continue;
            if (entries[i].attr & ATTR_DIRECTORY) continue;
            
            if (match_83(&entries[i], name)) {
                found_entry = entries[i];
                found = true;
                break;
            }
        }
        cluster = next_cluster(cluster);
    }
search_done:
    
    if (!found) return false;
    
    out_size = found_entry.file_size;
    uint32_t first_cluster = ((uint32_t)found_entry.first_cluster_hi << 16) | found_entry.first_cluster_lo;
    
    // Read file data cluster by cluster
    uint32_t bytes_read = 0;
    uint32_t fc = first_cluster;
    uint8_t* dst = (uint8_t*)buf;
    
    while (fc < FAT32_EOC && bytes_read < out_size && bytes_read < max_size) {
        if (!read_cluster(fc, cluster_buf)) break;
        
        uint32_t bytes_in_cluster = m_sectors_per_cluster * 512;
        uint32_t to_copy = out_size - bytes_read;
        if (to_copy > bytes_in_cluster) to_copy = bytes_in_cluster;
        if (bytes_read + to_copy > max_size) to_copy = max_size - bytes_read;
        
        for (uint32_t i = 0; i < to_copy; i++)
            dst[bytes_read + i] = cluster_buf[i];
        
        bytes_read += to_copy;
        fc = next_cluster(fc);
    }
    
    return true;
}

// ── VFS-facing API ─────────────────────────────────────────────────────────

void FAT32Driver::build_83(const DirEntry* e, char* buf) {
    int idx = 0;
    int nl = 8; while (nl > 0 && e->name[nl-1] == ' ') nl--;
    for (int i = 0; i < nl; i++) buf[idx++] = e->name[i];
    int el = 3; while (el > 0 && e->ext[el-1] == ' ') el--;
    if (el > 0) { buf[idx++] = '.'; for (int i = 0; i < el; i++) buf[idx++] = e->ext[i]; }
    buf[idx] = 0;
}

bool FAT32Driver::lookup_entry(uint32_t dir_cluster, const char* name83,
                                uint32_t& out_cluster, uint32_t& out_size, bool& out_is_dir) {
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL,
            (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    // "." means the directory itself — return it as-is
    if (name83[0] == '.' && name83[1] == '\0') {
        out_cluster = dir_cluster;
        out_size    = 0;
        out_is_dir  = true;
        return true;
    }

    uint32_t cluster = dir_cluster;
    while (cluster < FAT32_EOC) {
        if (!read_cluster(cluster, cluster_buf)) break;
        uint32_t n = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* entries = (DirEntry*)cluster_buf;
        for (uint32_t i = 0; i < n; i++) {
            if (entries[i].name[0] == 0x00) return false;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == ATTR_LFN)       continue;
            if (entries[i].attr & ATTR_VOLUME_ID)  continue;
            if (match_83(&entries[i], name83)) {
                out_cluster = ((uint32_t)entries[i].first_cluster_hi << 16) | entries[i].first_cluster_lo;
                out_size    = entries[i].file_size;
                out_is_dir  = (entries[i].attr & ATTR_DIRECTORY) != 0;
                return true;
            }
        }
        cluster = next_cluster(cluster);
    }
    return false;
}

int FAT32Driver::read_at(uint32_t first_cluster, uint32_t file_size,
                          void* buf, uint32_t offset, uint32_t size) {
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;
    if (size == 0) return 0;

    uint32_t cluster_bytes = m_sectors_per_cluster * 512;
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL,
            (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    // Skip clusters before 'offset'
    uint32_t skip_clusters = offset / cluster_bytes;
    uint32_t within_cluster = offset % cluster_bytes;

    uint32_t fc = first_cluster;
    for (uint32_t i = 0; i < skip_clusters && fc < FAT32_EOC; i++)
        fc = next_cluster(fc);

    uint8_t* dst = (uint8_t*)buf;
    uint32_t bytes_read = 0;

    while (fc < FAT32_EOC && bytes_read < size) {
        if (!read_cluster(fc, cluster_buf)) return (int)bytes_read;

        uint32_t src_off = within_cluster;
        uint32_t avail   = cluster_bytes - src_off;
        uint32_t to_copy = size - bytes_read;
        if (to_copy > avail) to_copy = avail;

        for (uint32_t i = 0; i < to_copy; i++)
            dst[bytes_read + i] = cluster_buf[src_off + i];

        bytes_read     += to_copy;
        within_cluster  = 0;        // Only skip on the first cluster
        fc = next_cluster(fc);
    }
    return (int)bytes_read;
}

bool FAT32Driver::listdir_cluster(uint32_t dir_cluster,
    DirEnum* entries,
    uint32_t max, uint32_t& count) {

    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL,
            (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    count = 0;
    uint32_t cluster = dir_cluster;
    while (cluster < FAT32_EOC && count < max) {
        if (!read_cluster(cluster, cluster_buf)) break;
        uint32_t n = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* dir_entries = (DirEntry*)cluster_buf;
        for (uint32_t i = 0; i < n && count < max; i++) {
            if (dir_entries[i].name[0] == 0x00) return true; // EOD
            if (dir_entries[i].name[0] == 0xE5) continue;
            if (dir_entries[i].attr == ATTR_LFN)      continue;
            if (dir_entries[i].attr & ATTR_VOLUME_ID) continue;
            // Skip . and ..
            if (dir_entries[i].name[0] == '.') continue;

            build_83(&dir_entries[i], entries[count].name);
            entries[count].is_dir = (dir_entries[i].attr & ATTR_DIRECTORY) != 0;
            entries[count].size   = dir_entries[i].file_size;
            count++;
        }
        cluster = next_cluster(cluster);
    }
    return true;
}

int FAT32Driver::write_at(uint32_t first_cluster, uint32_t file_size,
                          const void* buf, uint32_t offset, uint32_t size,
                          uint32_t dir_cluster, const char* name83) {
    if (size == 0) return 0;
    
    uint32_t cluster_bytes = m_sectors_per_cluster * 512;
    uint32_t end_offset = offset + size;
    uint32_t new_file_size = (end_offset > file_size) ? end_offset : file_size;
    
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL, (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    if (first_cluster == 0) {
        first_cluster = allocate_cluster(0);
        if (first_cluster == 0) return -1;
    }

    uint32_t skip_clusters = offset / cluster_bytes;
    uint32_t within_cluster = offset % cluster_bytes;

    uint32_t fc = first_cluster;
    uint32_t last_fc = fc;
    for (uint32_t i = 0; i < skip_clusters; i++) {
        uint32_t next = next_cluster(fc);
        if (next >= FAT32_EOC) {
            next = allocate_cluster(fc);
            if (next == 0) return -1;
        }
        last_fc = fc;
        fc = next;
    }

    const uint8_t* src = (const uint8_t*)buf;
    uint32_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t to_write = size - bytes_written;
        uint32_t avail = cluster_bytes - within_cluster;
        if (to_write > avail) to_write = avail;

        if (to_write < cluster_bytes) {
            read_cluster(fc, cluster_buf);
        }
        
        for (uint32_t i = 0; i < to_write; i++) {
            cluster_buf[within_cluster + i] = src[bytes_written + i];
        }

        write_cluster(fc, cluster_buf);

        bytes_written += to_write;
        within_cluster = 0;

        if (bytes_written < size) {
            uint32_t next = next_cluster(fc);
            if (next >= FAT32_EOC) {
                next = allocate_cluster(fc);
                if (next == 0) break;
            }
            fc = next;
        }
    }

    if (new_file_size != file_size || file_size == 0) {
        update_dir_entry(dir_cluster, name83, new_file_size, first_cluster);
    }
    
    return bytes_written;
}

bool FAT32Driver::create_entry(uint32_t dir_cluster, const char* name83, bool is_dir, uint32_t& out_cluster) {
    uint8_t* cluster_buf = (uint8_t*)(pmm_alloc_page() + pmm_hhdm_offset());
    vmm_map((uintptr_t)cluster_buf & ~0xFFFULL, (uintptr_t)cluster_buf - pmm_hhdm_offset(), VMM_MMIO);

    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = cluster;
    
    while (cluster < FAT32_EOC) {
        if (!read_cluster(cluster, cluster_buf)) return false;
        
        uint32_t n = (m_sectors_per_cluster * 512) / sizeof(DirEntry);
        DirEntry* entries = (DirEntry*)cluster_buf;
        
        for (uint32_t i = 0; i < n; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                for (int j=0; j<8; j++) entries[i].name[j] = ' ';
                for (int j=0; j<3; j++) entries[i].ext[j] = ' ';
                int idx=0;
                while (name83[idx] && name83[idx] != '.') { entries[i].name[idx] = name83[idx]; idx++; }
                if (name83[idx] == '.') {
                    idx++;
                    for (int j=0; j<3 && name83[idx]; j++, idx++) entries[i].ext[j] = name83[idx];
                }
                
                entries[i].attr = is_dir ? ATTR_DIRECTORY : 0;
                entries[i].file_size = 0;
                entries[i].first_cluster_hi = 0;
                entries[i].first_cluster_lo = 0;
                out_cluster = 0;
                
                if (is_dir) {
                    out_cluster = allocate_cluster(0);
                    entries[i].first_cluster_hi = (out_cluster >> 16) & 0xFFFF;
                    entries[i].first_cluster_lo = out_cluster & 0xFFFF;
                }
                
                write_cluster(cluster, cluster_buf);
                return true;
            }
        }
        last_cluster = cluster;
        cluster = next_cluster(cluster);
    }
    
    uint32_t new_cluster = allocate_cluster(last_cluster);
    if (new_cluster == 0) return false;
    return create_entry(new_cluster, name83, is_dir, out_cluster);
}

} // namespace FAT32
