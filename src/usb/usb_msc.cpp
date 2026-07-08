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
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            

#include "usb/usb_msc.hpp"
#include "vga.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

extern VGA* g_vga;

namespace USB {

static uintptr_t alloc_dma_page() {
    uintptr_t phys = pmm_alloc_page();
    uintptr_t virt = phys + pmm_hhdm_offset();
    vmm_map(virt & ~0xFFFULL, phys, VMM_MMIO);
    for (int i = 0; i < 4096; i++) ((uint8_t*)virt)[i] = 0;
    return phys;
}

USBMassStorage::USBMassStorage(uint32_t slot_id, XHCI* xhci)
    : m_slot_id(slot_id), m_xhci(xhci) {}

void* USBMassStorage::cbw_virt() const {
    return reinterpret_cast<void*>(m_cbw_phys + pmm_hhdm_offset());
}
void* USBMassStorage::csw_virt() const {
    return reinterpret_cast<void*>(m_csw_phys + pmm_hhdm_offset());
}

bool USBMassStorage::init() {
    // Allocate DMA buffers
    m_cbw_phys  = alloc_dma_page();
    m_csw_phys  = alloc_dma_page();
    m_data_phys = alloc_dma_page();

    // Reset the interface with Bulk-Only Mass Storage Reset
    // bmRequestType=0x21 (Class,Interface,OUT), bRequest=0xFF, wValue=0, wIndex=0, wLength=0
    uintptr_t dummy = alloc_dma_page();
    m_xhci->do_control_transfer(m_slot_id, 0x21, 0xFF, 0, 0, 0,
                                 reinterpret_cast<void*>(dummy));

    // Get Max LUN
    m_xhci->do_control_transfer(m_slot_id, 0xA1, 0xFE, 0, 0, 1,
                                 reinterpret_cast<void*>(dummy));

    // Send INQUIRY (0x12) to identify the device
    uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!send_scsi(inquiry_cdb, 6, reinterpret_cast<void*>(m_data_phys), 36, true)) {
        if (g_vga) g_vga->write("USB MSC: INQUIRY failed!\n");
        return false;
    }
    
    if (g_vga) {
        uint8_t* data = reinterpret_cast<uint8_t*>(m_data_phys + pmm_hhdm_offset());
        char vendor[9] = {0};
        char product[17] = {0};
        for (int i = 0; i < 8; i++)  vendor[i]  = (data[8+i] >= 0x20) ? data[8+i] : ' ';
        for (int i = 0; i < 16; i++) product[i] = (data[16+i] >= 0x20) ? data[16+i] : ' ';
        // Log rimosso
    }

    // TEST UNIT READY (0x00)
    uint8_t tur_cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    send_scsi(tur_cdb, 6, reinterpret_cast<void*>(m_data_phys), 0, true);

    // READ CAPACITY to get sector count
    if (!read_capacity()) {
        if (g_vga) g_vga->write("USB MSC: READ CAPACITY failed!\n");
        return false;
    }

    return true;
}

bool USBMassStorage::read_capacity() {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // READ CAPACITY(10)
    if (!send_scsi(cdb, 10, reinterpret_cast<void*>(m_data_phys), 8, true))
        return false;

    uint8_t* data = reinterpret_cast<uint8_t*>(m_data_phys + pmm_hhdm_offset());
    uint32_t last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                        ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    m_sector_count = (uint64_t)last_lba + 1;

    if (g_vga) {
        // Log rimosso
    }
    return true;
}

bool USBMassStorage::send_scsi(const uint8_t* cdb, uint8_t cdb_len,
                                void* data_phys, uint32_t data_len, bool is_in) {
    // Build CBW
    CBW* cbw = reinterpret_cast<CBW*>(cbw_virt());
    cbw->dCBWSignature         = CBW_SIGNATURE;
    cbw->dCBWTag               = m_tag++;
    cbw->dCBWDataTransferLength = data_len;
    cbw->bmCBWFlags            = is_in ? 0x80 : 0x00;
    cbw->bCBWLUN               = 0;
    cbw->bCBWCBLength          = cdb_len;
    for (int i = 0; i < 16; i++) cbw->CBWCB[i] = (i < cdb_len) ? cdb[i] : 0;

    // Send CBW via Bulk OUT
    if (!m_xhci->submit_bulk_out(m_slot_id, reinterpret_cast<void*>(m_cbw_phys), 31))
        return false;

    // Data phase
    if (data_len > 0) {
        if (is_in) {
            if (!m_xhci->submit_bulk_in(m_slot_id, data_phys, data_len))
                return false;
        } else {
            if (!m_xhci->submit_bulk_out(m_slot_id, data_phys, data_len))
                return false;
        }
    }

    // Receive CSW via Bulk IN
    if (!m_xhci->submit_bulk_in(m_slot_id, reinterpret_cast<void*>(m_csw_phys), 13))
        return false;

    CSW* csw = reinterpret_cast<CSW*>(csw_virt());
    if (csw->dCSWSignature != CSW_SIGNATURE) return false;
    return csw->bCSWStatus == 0;
}

bool USBMassStorage::read_blocks(uint64_t lba, uint32_t count, void* buffer) {
    // READ(10): CDB[0]=0x28
    uint8_t cdb[10] = {
        0x28,
        0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0x00,
        (uint8_t)(count >> 8), (uint8_t)(count),
        0x00
    };

    uint32_t bytes = count * 512;
    // Use the pre-allocated data buffer (max 4KB per call for now)
    if (bytes > 4096) bytes = 4096;

    if (!send_scsi(cdb, 10, reinterpret_cast<void*>(m_data_phys), bytes, true))
        return false;

    // Copy from DMA buffer to user buffer
    uint8_t* src = reinterpret_cast<uint8_t*>(m_data_phys + pmm_hhdm_offset());
    uint8_t* dst = reinterpret_cast<uint8_t*>(buffer);
    for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];
    return true;
}

bool USBMassStorage::write_blocks(uint64_t lba, uint32_t count, const void* buffer) {
    // WRITE(10): CDB[0]=0x2A
    uint8_t cdb[10] = {
        0x2A,
        0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0x00,
        (uint8_t)(count >> 8), (uint8_t)(count),
        0x00
    };

    uint32_t bytes = count * 512;
    if (bytes > 4096) bytes = 4096;

    // Copy from user buffer to DMA buffer
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buffer);
    uint8_t* dst = reinterpret_cast<uint8_t*>(m_data_phys + pmm_hhdm_offset());
    for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];

    // send_scsi with in=false (Bulk OUT)
    if (!send_scsi(cdb, 10, reinterpret_cast<void*>(m_data_phys), bytes, false))
        return false;

    return true;
}

} // namespace USB
