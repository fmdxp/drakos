#pragma once

#include <stdint.h>
#include <stddef.h>
#include "pci.hpp"
#include "fs/vfs.hpp"
#include "fs/block_device.hpp"

namespace NVMe {

// Registers offset
#define NVME_REG_CAP    0x00 // Controller Capabilities
#define NVME_REG_VS     0x08 // Version
#define NVME_REG_INTMS  0x0C // Interrupt Mask Set
#define NVME_REG_INTMC  0x10 // Interrupt Mask Clear
#define NVME_REG_CC     0x14 // Controller Configuration
#define NVME_REG_CSTS   0x1C // Controller Status
#define NVME_REG_AQA    0x24 // Admin Queue Attributes
#define NVME_REG_ASQ    0x28 // Admin Submission Queue Base Address
#define NVME_REG_ACQ    0x30 // Admin Completion Queue Base Address

// Submission Queue Entry (64 bytes)
struct alignas(64) SQEntry {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

// Completion Queue Entry (16 bytes)
struct alignas(16) CQEntry {
    uint32_t cdw0;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; // bit 0 is Phase Tag (P)
};

class NVMeController;

class NVMeNamespace : public FS::BlockDevice {
public:
    NVMeNamespace(NVMeController* ctrl, uint32_t nsid, uint64_t lba_count, uint32_t lba_size);
    ~NVMeNamespace() override = default;

    bool read_blocks(uint64_t lba, uint32_t count, void* buffer) override;
    bool write_blocks(uint64_t lba, uint32_t count, const void* buffer) override;
    uint32_t get_sector_size() const override { return m_sector_size; }
    uint64_t get_sector_count() const override { return m_sector_count; }

private:
    NVMeController* m_ctrl;
    uint32_t m_nsid;
    uint64_t m_sector_count;
    uint32_t m_sector_size;
};

class NVMeController {
public:
    NVMeController(const PCIDevice& pci_dev);
    ~NVMeController() = default;

    bool init();

    // Internal I/O methods called by NVMeNamespace
    bool submit_io(uint32_t nsid, uint64_t lba, uint32_t count, uintptr_t prp1, uintptr_t prp2, bool write);

private:
    bool wait_ready(bool expected);
    uint16_t submit_admin_cmd(SQEntry& cmd);

    PCIDevice m_pci;
    uintptr_t m_bar0;
    uint32_t m_doorbell_stride;

    // Admin Queue
    SQEntry* m_asq;
    CQEntry* m_acq;
    uintptr_t m_asq_phys;
    uintptr_t m_acq_phys;
    uint16_t m_asq_tail;
    uint16_t m_acq_head;
    uint8_t  m_acq_phase;

    // I/O Queue (Queue ID 1)
    SQEntry* m_iosq;
    CQEntry* m_iocq;
    uintptr_t m_iosq_phys;
    uintptr_t m_iocq_phys;
    uint16_t m_iosq_tail;
    uint16_t m_iocq_head;
    uint8_t  m_iocq_phase;
    
    uint16_t m_cid_counter;
};

extern NVMeController* g_nvme;

void init_nvme(const PCIDevice& dev);

} // namespace NVMe
