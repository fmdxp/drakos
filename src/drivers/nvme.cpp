#include "nvme.hpp"
#include "vmm.hpp"
#include "pmm.hpp"
#include "vga.hpp"
#include "fs/vfs_fat32.hpp"

namespace NVMe {

NVMeController* g_nvme = nullptr;

static inline uint32_t read32(uintptr_t base, uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t*>(base + reg);
}
static inline void write32(uintptr_t base, uint32_t reg, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(base + reg) = val;
}
static inline void write64(uintptr_t base, uint32_t reg, uint64_t val) {
    *reinterpret_cast<volatile uint64_t*>(base + reg) = val;
}

NVMeNamespace::NVMeNamespace(NVMeController* ctrl, uint32_t nsid, uint64_t lba_count, uint32_t lba_size)
    : m_ctrl(ctrl), m_nsid(nsid), m_sector_count(lba_count), m_sector_size(lba_size) {
}

bool NVMeNamespace::read_blocks(uint64_t lba, uint32_t count, void* buffer) {
    if (lba + count > m_sector_count) return false;
    uintptr_t bounce_phys = pmm_alloc_page();
    uint8_t* bounce_virt = (uint8_t*)(bounce_phys + pmm_hhdm_offset());
    uint8_t* dst = (uint8_t*)buffer;
    uint32_t sectors_done = 0;

    while (sectors_done < count) {
        uint32_t chunk = (count - sectors_done > 8) ? 8 : (count - sectors_done);
        
        for(uint32_t j=0; j<4096; j++) bounce_virt[j] = 0xBB; // DEBUG: fill with 0xBB

        if (!m_ctrl->submit_io(m_nsid, lba + sectors_done, chunk, bounce_phys, 0, false)) {
            pmm_free_page(bounce_phys);
            return false;
        }
        for (uint32_t i = 0; i < chunk * m_sector_size; i++) {
            dst[sectors_done * m_sector_size + i] = bounce_virt[i];
        }
        sectors_done += chunk;
    }
    pmm_free_page(bounce_phys);
    return true;
}

bool NVMeNamespace::write_blocks(uint64_t lba, uint32_t count, const void* buffer) {
    if (lba + count > m_sector_count) return false;
    uintptr_t bounce_phys = pmm_alloc_page();
    uint8_t* bounce_virt = (uint8_t*)(bounce_phys + pmm_hhdm_offset());
    const uint8_t* src = (const uint8_t*)buffer;
    uint32_t sectors_done = 0;

    while (sectors_done < count) {
        uint32_t chunk = (count - sectors_done > 8) ? 8 : (count - sectors_done);
        for (uint32_t i = 0; i < chunk * m_sector_size; i++) {
            bounce_virt[i] = src[sectors_done * m_sector_size + i];
        }
        if (!m_ctrl->submit_io(m_nsid, lba + sectors_done, chunk, bounce_phys, 0, true)) {
            pmm_free_page(bounce_phys);
            return false;
        }
        sectors_done += chunk;
    }
    pmm_free_page(bounce_phys);
    return true;
}

NVMeController::NVMeController(const PCIDevice& pci_dev)
    : m_pci(pci_dev), m_bar0(0), m_doorbell_stride(0),
      m_asq(nullptr), m_acq(nullptr), m_asq_phys(0), m_acq_phys(0),
      m_asq_tail(0), m_acq_head(0), m_acq_phase(1),
      m_iosq(nullptr), m_iocq(nullptr), m_iosq_phys(0), m_iocq_phys(0),
      m_iosq_tail(0), m_iocq_head(0), m_iocq_phase(1), m_cid_counter(1) {}

bool NVMeController::wait_ready(bool expected) {
    uint32_t expected_val = expected ? 1 : 0;
    for (int i = 0; i < 5000000; i++) {
        uint32_t csts = read32(m_bar0, NVME_REG_CSTS);
        if ((csts & 1) == expected_val) return true;
        for (int j = 0; j < 100; j++) { asm volatile("pause" ::: "memory"); } // Short delay
    }
    return false;
}

uint16_t NVMeController::submit_admin_cmd(SQEntry& cmd) {
    cmd.cid = m_cid_counter++;
    m_asq[m_asq_tail] = cmd;
    
    m_asq_tail++;
    if (m_asq_tail == 64) m_asq_tail = 0; // Queue size = 64
    
    // Ensure all memory writes are globally visible before ringing doorbell
    asm volatile("mfence" ::: "memory");
    
    // Ring ASQ doorbell (Offset 0x1000)
    write32(m_bar0, 0x1000, m_asq_tail);
    
    // Wait for completion
    while (true) {
        CQEntry* cq = &m_acq[m_acq_head];
        if ((cq->status & 1) == m_acq_phase) {
            uint16_t status = cq->status >> 1;
            
            m_acq_head++;
            if (m_acq_head == 64) {
                m_acq_head = 0;
                m_acq_phase = !m_acq_phase;
            }
            
            // Ring ACQ doorbell
            write32(m_bar0, 0x1000 + 1 * (4 << m_doorbell_stride), m_acq_head);
            return status;
        }
    }
}

bool NVMeController::submit_io(uint32_t nsid, uint64_t lba, uint32_t count, uintptr_t prp1, uintptr_t prp2, bool write) {
    SQEntry cmd = {};
    cmd.opcode = write ? 0x01 : 0x02; // Write or Read
    cmd.nsid = nsid;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFF; // Number of Logical Blocks (0-based)
    
    cmd.cid = m_cid_counter++;
    m_iosq[m_iosq_tail] = cmd;
    
    m_iosq_tail++;
    if (m_iosq_tail == 64) m_iosq_tail = 0;
    
    // Ensure all memory writes are globally visible before ringing doorbell
    asm volatile("mfence" ::: "memory");
    
    // Ring IO SQ doorbell (Queue 1 SQ is at y=2)
    write32(m_bar0, 0x1000 + 2 * (4 << m_doorbell_stride), m_iosq_tail);
    
    // Wait for completion
    while (true) {
        CQEntry* cq = &m_iocq[m_iocq_head];
        if ((cq->status & 1) == m_iocq_phase) {
            uint16_t status = cq->status >> 1;
            
            m_iocq_head++;
            if (m_iocq_head == 64) {
                m_iocq_head = 0;
                m_iocq_phase = !m_iocq_phase;
            }
            
            // Ring IO CQ doorbell (Queue 1 CQ is at y=3)
            write32(m_bar0, 0x1000 + 3 * (4 << m_doorbell_stride), m_iocq_head);
            
            if (status != 0) {
                if (g_vga) {
                    g_vga->write("NVMe IO Error! Status: ");
                    char buf[8];
                    int i = 0;
                    uint16_t temp = status;
                    if (temp == 0) buf[i++] = '0';
                    while(temp > 0) { buf[i++] = '0' + (temp % 10); temp /= 10; }
                    for(int j=0; j<i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
                    buf[i] = '\0';
                    g_vga->write(buf);
                    g_vga->write("\n");
                }
            }
            return status == 0;
        }
    }
}

bool NVMeController::init() {
    uint32_t bar0_low = g_pci->read(m_pci.bus, m_pci.device, m_pci.function, 0x10);
    uint32_t bar0_high = g_pci->read(m_pci.bus, m_pci.device, m_pci.function, 0x14);
    uintptr_t phys_base = (bar0_low & 0xFFFFFFF0) | ((uint64_t)bar0_high << 32);
    
    m_bar0 = phys_base + pmm_hhdm_offset();
    vmm_map(m_bar0 & ~0xFFFULL, phys_base & ~0xFFFULL, VMM_MMIO);
    vmm_map((m_bar0 + 0x1000) & ~0xFFFULL, (phys_base + 0x1000) & ~0xFFFULL, VMM_MMIO);

    g_pci->write(m_pci.bus, m_pci.device, m_pci.function, 0x04, g_pci->read(m_pci.bus, m_pci.device, m_pci.function, 0x04) | 0x06); // Enable Bus Master, Memory Space

    uint64_t cap = *reinterpret_cast<volatile uint64_t*>(m_bar0 + NVME_REG_CAP);
    m_doorbell_stride = (cap >> 32) & 0xF;
    
    // Disable controller
    uint32_t cc = read32(m_bar0, NVME_REG_CC);
    if (cc & 1) {
        write32(m_bar0, NVME_REG_CC, cc & ~1);
        if (!wait_ready(false)) {
            if (g_vga) g_vga->write("NVMe: Failed to disable controller!\n");
            return false;
        }
    }
    
    // Allocate Admin Queues (Queue size = 64)
    m_asq_phys = pmm_alloc_page();
    m_acq_phys = pmm_alloc_page();
    m_asq = reinterpret_cast<SQEntry*>(m_asq_phys + pmm_hhdm_offset());
    m_acq = reinterpret_cast<CQEntry*>(m_acq_phys + pmm_hhdm_offset());
    for (int i = 0; i < 4096; i++) { ((uint8_t*)m_asq)[i] = 0; ((uint8_t*)m_acq)[i] = 0; }
    
    write32(m_bar0, NVME_REG_AQA, (63 << 16) | 63); // ASQ/ACQ size = 64 (0-based)
    write64(m_bar0, NVME_REG_ASQ, m_asq_phys);
    write64(m_bar0, NVME_REG_ACQ, m_acq_phys);
    
    // Enable Controller
    // IOCQES=4 (16 bytes), IOSQES=6 (64 bytes), MPS=0 (4KB), CSS=0 (NVM Command Set), EN=1
    cc = (4 << 20) | (6 << 16) | (0 << 7) | (0 << 4) | 1;
    write32(m_bar0, NVME_REG_CC, cc);
    
    if (!wait_ready(true)) {
        if (g_vga) g_vga->write("NVMe: Failed to enable controller!\n");
        return false;
    }
    
    if (g_vga) g_vga->write("NVMe: Controller Ready!\n");
    
    // Allocate I/O Queues
    m_iosq_phys = pmm_alloc_page();
    m_iocq_phys = pmm_alloc_page();
    m_iosq = reinterpret_cast<SQEntry*>(m_iosq_phys + pmm_hhdm_offset());
    m_iocq = reinterpret_cast<CQEntry*>(m_iocq_phys + pmm_hhdm_offset());
    for (int i = 0; i < 4096; i++) { ((uint8_t*)m_iosq)[i] = 0; ((uint8_t*)m_iocq)[i] = 0; }
    
    // Create I/O CQ (Command 0x05)
    SQEntry create_cq = {};
    create_cq.opcode = 0x05;
    create_cq.prp1 = m_iocq_phys;
    create_cq.cdw10 = (63 << 16) | 1; // Size=64, QID=1
    create_cq.cdw11 = 1; // Physically Contiguous
    if (submit_admin_cmd(create_cq) != 0) {
        if (g_vga) g_vga->write("NVMe: Failed to create I/O CQ!\n");
        return false;
    }
    
    // Create I/O SQ (Command 0x01)
    SQEntry create_sq = {};
    create_sq.opcode = 0x01;
    create_sq.prp1 = m_iosq_phys;
    create_sq.cdw10 = (63 << 16) | 1; // Size=64, QID=1
    create_sq.cdw11 = (1 << 16) | 1; // CQID=1, Physically Contiguous
    if (submit_admin_cmd(create_sq) != 0) {
        if (g_vga) g_vga->write("NVMe: Failed to create I/O SQ!\n");
        return false;
    }
    
    // Identify Namespace 1
    uintptr_t id_phys = pmm_alloc_page();
    void* id_virt = (void*)(id_phys + pmm_hhdm_offset());
    for(int i=0; i<4096; i++) ((uint8_t*)id_virt)[i] = 0xAA; // DEBUG
    
    SQEntry id_ns = {};
    id_ns.opcode = 0x06; // Identify
    id_ns.nsid = 1;
    id_ns.prp1 = id_phys;
    id_ns.cdw10 = 0x00; // Identify Namespace
    
    if (submit_admin_cmd(id_ns) == 0) {
        uint64_t nsze = *reinterpret_cast<uint64_t*>((uintptr_t)id_virt + 0);
        
        if (g_vga) {
            g_vga->write("NVMe: Namespace 1 Found!\n");
        }
        
        NVMeNamespace* ns = new NVMeNamespace(this, 1, nsze, 512);
        
        VFS::Fat32FS* fs = new VFS::Fat32FS(ns);
        if (fs->init()) {
            vfs_mount("/nvme", fs);
            if (g_vga) g_vga->write("NVMe: Mounted /nvme!\n");
        }
    }
    
    return true;
}

void init_nvme(const PCIDevice& dev) {
    NVMeController* ctrl = new NVMeController(dev);
    if (ctrl->init()) {
        g_nvme = ctrl;
    } else {
        delete ctrl;
    }
}

} // namespace NVMe
