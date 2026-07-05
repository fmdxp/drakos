#pragma once
#include <stdint.h>
#include "fs/block_device.hpp"
#include "xhci.hpp"

namespace USB {

// Command Block Wrapper (CBW) - 31 bytes
struct CBW {
    uint32_t dCBWSignature;         // 0x43425355 "USBC"
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;            // 0x80 = IN (device-to-host), 0x00 = OUT
    uint8_t  bCBWLUN;              // Logical Unit Number (usually 0)
    uint8_t  bCBWCBLength;         // Length of CBWCB (6-16)
    uint8_t  CBWCB[16];            // SCSI Command Block
} __attribute__((packed));

// Command Status Wrapper (CSW) - 13 bytes
struct CSW {
    uint32_t dCSWSignature;         // 0x53425355 "USBS"
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;            // 0 = success
} __attribute__((packed));

#define CSW_SIGNATURE 0x53425355
#define CBW_SIGNATURE 0x43425355

// USB MSC block device - wraps a USB device and exposes it as FS::BlockDevice
class USBMassStorage : public FS::BlockDevice {
public:
    USBMassStorage(uint32_t slot_id, XHCI* xhci);
    
    bool init();
    
    bool read_blocks(uint64_t lba, uint32_t count, void* buffer) override;
    bool write_blocks(uint64_t lba, uint32_t count, const void* buffer) override;
    uint32_t get_sector_size()  const override { return 512; }
    uint64_t get_sector_count() const override { return m_sector_count; }

private:
    uint32_t m_slot_id;
    XHCI*    m_xhci;
    uint32_t m_tag = 1;
    uint64_t m_sector_count = 0;

    // Phys-mapped DMA buffers (allocated once in init)
    uintptr_t m_cbw_phys = 0;
    uintptr_t m_csw_phys = 0;
    uintptr_t m_data_phys = 0;

    bool send_scsi(const uint8_t* cdb, uint8_t cdb_len,
                   void* data_phys, uint32_t data_len, bool is_in);
    bool read_capacity();
    void* cbw_virt() const;
    void* csw_virt() const;
};

} // namespace USB
