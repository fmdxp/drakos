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


#include "ahci.hpp"
#include "pci.hpp"
#include "vmm.hpp"
#include "vga.hpp"
#include "pmm.hpp"
#include "fs/fat32.hpp"
#include "fs/vfs.hpp"
#include "fs/vfs_fat32.hpp"

namespace AHCI {

#define SATA_SIG_ATA    0x00000101 // SATA drive
#define SATA_SIG_ATAPI  0xEB140101 // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101 // Enclosure management bridge
#define SATA_SIG_PM     0x96690101 // Port multiplier

#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

bool AHCIDriver::start() {
    if (g_vga) g_vga->write("AHCI: Driver Starting...\n");
    if (!g_pci) {
        if (g_vga) g_vga->write("AHCI: g_pci is NULL!\n");
        return false;
    }
    uintptr_t abar_phys = g_pci->get_ahci_bar();
    if (abar_phys == 0) {
        if (g_vga) g_vga->write("AHCI: No AHCI controller found.\n");
        return false;
    }

    // Map the entire ABAR region (multiple pages) with cache-disable
    for (int i = 0; i < 8; i++) {
        uintptr_t p = (abar_phys & ~0xFFFULL) + (i * 4096);
        vmm_map(p + pmm_hhdm_offset(), p, VMM_MMIO);
    }

    m_hba = reinterpret_cast<HBAMemory*>(abar_phys + pmm_hhdm_offset());

    // AHCI BIOS/OS Handoff (AHCI spec sec 10.7.2) - take ownership from BIOS
    if (m_hba->cap2 & (1 << 0)) { // BOH supported
        m_hba->bohc |= (1 << 1); // Set OOS (OS Ownership)
        int spin = 0;
        while ((m_hba->bohc & (1 << 0)) && spin++ < 1000000); // Wait for BOS to clear
    }

    // Ensure AHCI mode is enabled (GHC.AE = 1) - do NOT reset the whole HBA
    // UEFI already initialized the ports and link detection - we just take over
    m_hba->ghc |= (1 << 31);

    if (g_vga) g_vga->write("AHCI: Controller Ready. Scanning SATA Ports...\n");

    uint32_t pi = m_hba->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            check_port(&m_hba->ports[i], i);
        }
    }

    return true;
}

void AHCIDriver::stop() {}

void AHCIDriver::check_port(HBAPort* port, int port_no) {
    uint32_t ssts = port->ssts;
    
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;

    if (det != HBA_PORT_DET_PRESENT) return;
    if (ipm != HBA_PORT_IPM_ACTIVE) return;

    if (port->sig == SATA_SIG_ATA) {
        if (g_vga) {
            g_vga->write("AHCI: Found SATA Hard Disk on Port ");
            char buf[2];
            buf[0] = '0' + port_no; buf[1] = '\0';
            g_vga->write(buf);
            g_vga->write("!\n");
        }
        port_rebase(port, port_no);
        if (g_vga) g_vga->write("  -> Port Rebased and Ready!\n");
        
        // Test reading the first sector!
        AHCIDisk* disk = new AHCIDisk(port);
        
        // Alloca il buffer con cache-disable per garantire coerenza DMA
        uintptr_t buf_phys = pmm_alloc_page();
        uintptr_t buf_virt = buf_phys + pmm_hhdm_offset();
        vmm_map(buf_virt & ~0xFFFULL, buf_phys, VMM_MMIO);
        uint8_t* buffer = reinterpret_cast<uint8_t*>(buf_virt);
        for (int i = 0; i < 512; i++) buffer[i] = 0;
        
        if (disk->read_blocks(0, 1, buffer)) {
            if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
                if (g_vga) g_vga->write("  -> MBR Magic 0x55AA found! Disk read success!\n");
                
                // Mount via VFS as /sata
                VFS::Fat32FS* vfs_fs = new VFS::Fat32FS(disk);
                if (vfs_fs->init()) {
                    vfs_mount("/sata", vfs_fs);
                } else {
                    if (g_vga) g_vga->write("AHCI: FAT32 mount failed!\n");
                }
            } else {
                if (g_vga) g_vga->write("  -> Sector 0 read, but no MBR magic.\n");
            }
        } else {
            if (g_vga) g_vga->write("  -> Failed to read sector 0.\n");
        }
        
    } else if (port->sig == SATA_SIG_ATAPI) {
        if (g_vga) g_vga->write("AHCI: Found SATAPI CD-ROM\n");
    }
}

void AHCIDriver::port_rebase(HBAPort* port, int port_no) {
    (void)port_no;
    // Stop the port command engine
    port->cmd &= ~(1 << 0); // ST = 0
    port->cmd &= ~(1 << 4); // FRE = 0
    
    // Wait until FR and CR are both clear
    int spin = 0;
    while (spin++ < 500000) {
        if (!(port->cmd & (1 << 14)) && !(port->cmd & (1 << 15))) break;
    }

    // --- Allocate Command List (1K) + FIS (256B) in one page ---
    uintptr_t cl_phys = pmm_alloc_page();
    uintptr_t cl_virt = cl_phys + pmm_hhdm_offset();
    // Zero it
    for (int i = 0; i < 4096; i++) ((uint8_t*)cl_virt)[i] = 0;
    // Map with cache-disable so DMA writes are immediately visible
    vmm_map(cl_virt & ~0xFFFULL, cl_phys, VMM_MMIO);

    port->clb  = (uint32_t)(cl_phys & 0xFFFFFFFF);
    port->clbu = (uint32_t)(cl_phys >> 32);
    
    uintptr_t fis_phys = cl_phys + 1024; // FIS at offset 1K within same page
    port->fb  = (uint32_t)(fis_phys & 0xFFFFFFFF);
    port->fbu = (uint32_t)(fis_phys >> 32);

    // --- Allocate Command Tables: 32 slots x 256 bytes = 8KB (order-1 = 2 pages) ---
    uintptr_t ct_phys = pmm_alloc(1);
    uintptr_t ct_virt = ct_phys + pmm_hhdm_offset();
    for (int i = 0; i < 8192; i++) ((uint8_t*)ct_virt)[i] = 0;
    // Map both pages with cache-disable
    vmm_map(ct_virt & ~0xFFFULL,        ct_phys,        VMM_MMIO);
    vmm_map((ct_virt & ~0xFFFULL) + 4096, ct_phys + 4096, VMM_MMIO);

    // Set Command Table Base Address for each of the 32 command slots
    HBACommandHeader* cmdhdr = reinterpret_cast<HBACommandHeader*>(cl_virt);
    for (int i = 0; i < 32; i++) {
        uintptr_t ct_entry_phys = ct_phys + (i * 256);
        cmdhdr[i].prdtl = 8;
        cmdhdr[i].ctba  = (uint32_t)(ct_entry_phys & 0xFFFFFFFF);
        cmdhdr[i].ctbau = (uint32_t)(ct_entry_phys >> 32);
    }

    // Clear port error and interrupt bits
    port->serr = 0xFFFFFFFF;
    port->is   = 0xFFFFFFFF;

    // Restart command engine
    port->cmd |= (1 << 4); // FRE = 1
    port->cmd |= (1 << 0); // ST  = 1
}

} // namespace AHCI

REGISTER_MODULE(g_ahci_driver, AHCI::AHCIDriver, 5_usb);

namespace AHCI {

AHCIDisk::AHCIDisk(HBAPort* port) : m_port(port) {
    // A full implementation would send IDENTIFY command here to get sector_count
    // For now we just hardcode a dummy value or assume something
    m_sector_count = 100 * 1024 * 1024 / 512; // 100MB disk from our qemu-img
}

int AHCIDisk::find_cmdslot() {
    uint32_t slots = (m_port->sact | m_port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & (1 << i)) == 0) return i;
    }
    return -1;
}

bool AHCIDisk::read_blocks(uint64_t lba, uint32_t count, void* buffer) {
    m_port->is = 0xFFFF; // Clear pending interrupt bits
    int slot = find_cmdslot();
    if (slot == -1) return false;
    
    uintptr_t clb = m_port->clb | ((uintptr_t)m_port->clbu << 32);
    HBACommandHeader* cmdheader = reinterpret_cast<HBACommandHeader*>(clb + pmm_hhdm_offset());
    
    cmdheader[slot].cfl = sizeof(FIS_REG_H2D)/sizeof(uint32_t); // Command FIS size
    cmdheader[slot].w = 0; // Read
    cmdheader[slot].prdtl = (uint16_t)((count - 1) >> 4) + 1; // PRDT entries count
    
    uintptr_t ctba = cmdheader[slot].ctba | ((uintptr_t)cmdheader[slot].ctbau << 32);
    HBACommandTable* cmdtbl = reinterpret_cast<HBACommandTable*>(ctba + pmm_hhdm_offset());
    
    for (int i = 0; i < 8192; i++) ((uint8_t*)cmdtbl)[i] = 0;
    
    // 8K bytes (16 sectors) per PRDT
    int prdtl = cmdheader[slot].prdtl;
    uintptr_t buf_phys = (uintptr_t)buffer - pmm_hhdm_offset(); // Convert virt to phys
    for (int i = 0; i < prdtl - 1; i++) {
        cmdtbl->prdt_entry[i].dba = (uint32_t)buf_phys;
        cmdtbl->prdt_entry[i].dbau = (uint32_t)(buf_phys >> 32);
        cmdtbl->prdt_entry[i].dbc = 8192 - 1; // 8K bytes
        cmdtbl->prdt_entry[i].i = 0;
        buf_phys += 8192;
        count -= 16; 
    }
    
    cmdtbl->prdt_entry[prdtl-1].dba = (uint32_t)buf_phys;
    cmdtbl->prdt_entry[prdtl-1].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt_entry[prdtl-1].dbc = (count * 512) - 1; 
    cmdtbl->prdt_entry[prdtl-1].i = 0; // No interrupt
    
    // Setup command
    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    
    cmdfis->fis_type = 0x27; // FIS_TYPE_REG_H2D
    cmdfis->c = 1; // Command
    cmdfis->command = 0x25; // ATA_CMD_READ_DMA_EX
    
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6; // LBA mode
    
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;
    
    // Wait for port
    int spin = 0; 
    while ((m_port->tfd & (0x80 | 0x08)) && spin < 1000000) spin++;
    if (spin == 1000000) return false;
    
    m_port->ci = 1 << slot; 
    
    // Wait for completion
    int timeout = 0;
    while (1) {
        if ((m_port->ci & (1 << slot)) == 0) break;
        if (m_port->is & (1 << 30)) {
            if (g_vga) g_vga->write("AHCIDisk: Task file error!\n");
            return false; // Task file error
        }
        timeout++;
        if (timeout > 10000000) {
            if (g_vga) {
                g_vga->write("AHCIDisk: READ TIMEOUT!\n");
                g_vga->write("  -> IS: ");
                char buf[16];
                int idx = 0;
                uint32_t val = m_port->is;
                const char* hex = "0123456789ABCDEF";
                for (int i = 7; i >= 0; i--) buf[idx++] = hex[(val >> (i * 4)) & 0xF];
                buf[idx++] = '\n'; buf[idx] = 0;
                g_vga->write(buf);
                
                g_vga->write("  -> TFD: ");
                idx = 0;
                val = m_port->tfd;
                for (int i = 7; i >= 0; i--) buf[idx++] = hex[(val >> (i * 4)) & 0xF];
                buf[idx++] = '\n'; buf[idx] = 0;
                g_vga->write(buf);
            }
            return false;
        }
    }
    
    return true;
}

bool AHCIDisk::write_blocks([[maybe_unused]] uint64_t lba, [[maybe_unused]] uint32_t count, [[maybe_unused]] const void* buffer) {
    return false; // TODO: Implement me
}

} // namespace AHCI
