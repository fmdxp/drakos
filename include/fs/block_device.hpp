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

namespace FS {

// Interfaccia pura per un dispositivo a blocchi (Disco SATA, NVMe, USB)
class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    
    // Legge 'count' settori a partire dal blocco logico (LBA)
    virtual bool read_blocks(uint64_t lba, uint32_t count, void* buffer) = 0;
    
    // Scrive 'count' settori a partire dal blocco logico (LBA)
    virtual bool write_blocks(uint64_t lba, uint32_t count, const void* buffer) = 0;
    
    // Ritorna la grandezza del settore (tipicamente 512 byte)
    virtual uint32_t get_sector_size() const = 0;
    
    // Ritorna il numero totale di settori nel disco
    virtual uint64_t get_sector_count() const = 0;
};

} // namespace FS