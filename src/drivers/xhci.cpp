#include "xhci.hpp"
#include "pci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"
#include "input/dualsense.hpp"
#include "usb/usb.hpp"

// Capability Registers Offsets
#define XHCI_CAPLENGTH      0x00
#define XHCI_HCIVERSION     0x02
#define XHCI_HCSPARAMS1     0x04

// Operational Registers Offsets (added to CAPLENGTH)
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18
#define XHCI_DCBAAP         0x30
#define XHCI_CONFIG         0x38

// USBCMD bits
#define USBCMD_RS           (1 << 0) // Run/Stop
#define USBCMD_HCRST        (1 << 1) // Host Controller Reset
#define USBCMD_INTE         (1 << 2) // Interrupter Enable

// USBSTS bits
#define USBSTS_HCH          (1 << 0) // HC Halted
#define USBSTS_EINT         (1 << 3) // Event Interrupt
#define USBSTS_CNR          (1 << 11) // Controller Not Ready

// Runtime Registers Offset (from capabilities)
#define XHCI_RTSOFF         0x18

// Doorbell Array Offset (from capabilities)
#define XHCI_DBOFF          0x14

// Port Registers Offset (from operational base)
#define XHCI_PORTSC_BASE    0x400

// PORTSC bits
#define PORTSC_CCS          (1 << 0)  // Current Connect Status
#define PORTSC_PED          (1 << 1)  // Port Enabled/Disabled
#define PORTSC_PR           (1 << 4)  // Port Reset
#define PORTSC_PLS_MASK     (0xF << 5)// Port Link State
#define PORTSC_PP           (1 << 9)  // Port Power
#define PORTSC_PRC          (1 << 21) // Port Reset Change
#define PORTSC_WRC          (1 << 19) // Warm Port Reset Change

struct ERST_Entry {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size; // Number of TRBs
    uint32_t rsvd;
} __attribute__((packed));

uint32_t XHCI::read32(uint32_t offset) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_mmio_base + offset);
    return *ptr;
}

void XHCI::write32(uint32_t offset, uint32_t value) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_mmio_base + offset);
    *ptr = value;
}

void XHCI::write64(uint32_t offset, uint64_t value) {
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(m_mmio_base + offset);
    *ptr = value;
}

uint64_t XHCI::read64(uint32_t offset) {
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(m_mmio_base + offset);
    return *ptr;
}

void XHCI::ring_doorbell(uint8_t doorbell, uint32_t target) {
    uint32_t db_off = read32(XHCI_DBOFF) & ~0x3;
    write32(db_off + (doorbell * 4), target);
}

bool XHCI::reset_controller() {
    uint32_t op_base = m_cap_length;

    // 1. Ensure controller is stopped
    uint32_t cmd = read32(op_base + XHCI_USBCMD);
    cmd &= ~USBCMD_RS;
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until HC is halted
    while ((read32(op_base + XHCI_USBSTS) & USBSTS_HCH) == 0) {
        // In a real OS, we'd sleep or yield. For now, spin.
    }
    
    // 2. Send Reset
    cmd = read32(op_base + XHCI_USBCMD);
    cmd |= USBCMD_HCRST;
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until reset clears
    while (read32(op_base + XHCI_USBCMD) & USBCMD_HCRST) {
        // Spin
    }
    
    // Wait until Controller Not Ready clears
    while (read32(op_base + XHCI_USBSTS) & USBSTS_CNR) {
        // Spin
    }

    if (g_vga) g_vga->write("xHCI: Controller Reset Complete.\n");
    return true;
}

bool XHCI::initialize_data_structures() {
    // 1. Set max device slots enabled (CONFIG register)
    uint32_t op_base = m_cap_length;
    write32(op_base + XHCI_CONFIG, m_max_slots);

    // 2. Allocate DCBAA (Device Context Base Address Array)
    // Must be 64-byte aligned. pmm_alloc returns 4096-byte aligned.
    m_dcbaa_phys = pmm_alloc(1);
    if (!m_dcbaa_phys) return false;

    // Zero out the DCBAA
    uint64_t* dcbaa_virt = reinterpret_cast<uint64_t*>(m_dcbaa_phys + pmm_hhdm_offset());
    for (int i = 0; i < 256; i++) dcbaa_virt[i] = 0;

    // Pass physical address to controller
    write64(op_base + XHCI_DCBAAP, m_dcbaa_phys);

    // 3. Allocate Command Ring (1 page is enough for many commands)
    // Must be 64-byte aligned.
    m_cmd_ring_phys = pmm_alloc(1);
    if (!m_cmd_ring_phys) return false;

    // Zero out the Command Ring
    uint64_t* cmd_ring_virt = reinterpret_cast<uint64_t*>(m_cmd_ring_phys + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) cmd_ring_virt[i] = 0;

    // Pass physical address to controller (CRCR)
    // CRCR bit 0 is RCS (Ring Cycle State)
    write64(op_base + XHCI_CRCR, m_cmd_ring_phys | 1);

    if (g_vga) g_vga->write("xHCI: Data structures allocated.\n");
    return true;
}

bool XHCI::configure_interrupter() {
    uint32_t rts_off = read32(XHCI_RTSOFF) & ~0x1F;
    uint32_t ir0_base = rts_off + 0x0020; // Interrupter 0 registers

    // Allocate Event Ring Segment
    m_event_ring_phys = pmm_alloc(1);
    if (!m_event_ring_phys) return false;
    
    // Zero out Event Ring
    uint64_t* event_ring_virt = reinterpret_cast<uint64_t*>(m_event_ring_phys + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) event_ring_virt[i] = 0;

    // Allocate ERST (Event Ring Segment Table)
    m_erst_phys = pmm_alloc(1);
    if (!m_erst_phys) return false;

    ERST_Entry* erst_virt = reinterpret_cast<ERST_Entry*>(m_erst_phys + pmm_hhdm_offset());
    erst_virt[0].ring_segment_base_address = m_event_ring_phys;
    erst_virt[0].ring_segment_size = 256; // 256 TRBs (16 bytes each) fits in one 4096 page
    erst_virt[0].rsvd = 0;

    // Configure Interrupter 0
    write32(ir0_base + 0x08, 1); // ERSTSZ: 1 segment
    write64(ir0_base + 0x18, m_event_ring_phys | 0x08); // ERDP (Dequeue pointer + EHB bit)
    write64(ir0_base + 0x10, m_erst_phys); // ERSTBA

    // Enable Interrupter 0 (IMAN - Interrupt Management)
    uint32_t iman = read32(ir0_base + 0x00);
    iman |= (1 << 1); // IE (Interrupt Enable)
    write32(ir0_base + 0x00, iman);

    return true;
}

bool XHCI::start_controller() {
    uint32_t op_base = m_cap_length;
    
    uint32_t cmd = read32(op_base + XHCI_USBCMD);
    cmd |= USBCMD_RS | USBCMD_INTE; // Start and enable global interrupts
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until HC Halted is cleared
    while (read32(op_base + XHCI_USBSTS) & USBSTS_HCH) {
        // Spin
    }

    if (g_vga) g_vga->write("xHCI: Controller is RUNNING.\n");
    return true;
}

void XHCI::enumerate_ports() {
    uint32_t op_base = m_cap_length;
    
    for (uint32_t i = 1; i <= m_max_ports; i++) {
        uint32_t portsc_offset = op_base + XHCI_PORTSC_BASE + (0x10 * (i - 1));
        uint32_t portsc = read32(portsc_offset);
        
        // Bit 0 is CCS (Current Connect Status)
        if (portsc & PORTSC_CCS) {
            if (g_vga) {
                g_vga->write("xHCI: Device DETECTED on Port ");
                char buf[3];
                buf[0] = (i / 10) + '0';
                buf[1] = (i % 10) + '0';
                if (buf[0] == '0') {
                    buf[0] = buf[1];
                    buf[1] = '\0';
                } else {
                    buf[2] = '\0';
                }
                g_vga->write(buf);
                g_vga->write("!\n");
            }
            
            // Reset the port to negotiate speed and start enumeration
            m_port_setup_complete = false;
            reset_port(i);
            
            while (!m_port_setup_complete) {
                // Wait for the interrupt handler to finish setting up this port
            }
        }
    }
}

void XHCI::reset_port(uint32_t port_num) {
    uint32_t op_base = m_cap_length;
    uint32_t portsc_offset = op_base + XHCI_PORTSC_BASE + (0x10 * (port_num - 1));
    
    uint32_t portsc = read32(portsc_offset);
    
    // Clear status change bits to avoid clearing them accidentally
    portsc &= ~((1<<17) | (1<<18) | (1<<19) | (1<<20) | (1<<21) | (1<<22));
    
    // Issue Port Reset
    portsc |= PORTSC_PR;
    write32(portsc_offset, portsc);
    
    // Wait for Port Reset Change
    while ((read32(portsc_offset) & PORTSC_PRC) == 0) {
        // Spin wait (a real driver should yield or timeout)
    }
    
    // Clear PRC
    portsc = read32(portsc_offset);
    portsc &= ~((1<<17) | (1<<18) | (1<<19) | (1<<20) | (1<<22));
    portsc |= PORTSC_PRC; // Write 1 to clear
    write32(portsc_offset, portsc);
    
    // Read Speed (Bits 10:13)
    portsc = read32(portsc_offset);
    m_current_port_speed = (portsc >> 10) & 0xF;
    m_current_port_num = port_num;
    
    if (g_vga) {
        g_vga->write("xHCI: Port Reset Complete. Speed: ");
        char buf[2];
        buf[0] = m_current_port_speed + '0';
        buf[1] = '\0';
        g_vga->write(buf);
        g_vga->write(" Issuing Enable Slot...\n");
    }
    
    // Send Enable Slot Command
    TRB cmd = {0};
    cmd.control = (TRB_CMD_ENABLE_SLOT << 10);
    send_command(cmd);
}

void XHCI::send_command(const TRB& trb) {
    TRB* cmd_ring = reinterpret_cast<TRB*>(m_cmd_ring_phys + pmm_hhdm_offset());
    
    TRB new_trb = trb;
    if (m_cmd_ring_pcs) {
        new_trb.control |= 1; // Cycle bit
    } else {
        new_trb.control &= ~1;
    }
    
    cmd_ring[m_cmd_ring_index] = new_trb;
    m_cmd_ring_index++;
    
    if (m_cmd_ring_index == 255) {
        // Handle Link TRB to wrap around (simplified)
        TRB link_trb = {0};
        link_trb.parameter1 = m_cmd_ring_phys & 0xFFFFFFFF;
        link_trb.parameter2 = (m_cmd_ring_phys >> 32) & 0xFFFFFFFF;
        link_trb.control = (TRB_LINK << 10) | (1 << 1); // Toggle Cycle
        if (m_cmd_ring_pcs) link_trb.control |= 1;
        
        cmd_ring[m_cmd_ring_index] = link_trb;
        m_cmd_ring_pcs = !m_cmd_ring_pcs;
        m_cmd_ring_index = 0;
    }
    
    ring_doorbell(0, 0); // Doorbell 0 is Host Controller (Command Ring)
}

bool XHCI::start() {
    if (!g_pci) return false;
    
    uintptr_t phys_bar = g_pci->get_xhci_bar();
    if (phys_bar == 0) return false; // No xHCI found
    
    m_mmio_base = phys_bar + pmm_hhdm_offset();

    // The bootloader HHDM only maps RAM, not MMIO!
    // We MUST map the xHCI MMIO space explicitly to prevent a Page Fault.
    // We'll map 4 pages (16KB) to be safe for capability + operational + runtime registers.
    for (int i = 0; i < 4; i++) {
        vmm_map(m_mmio_base + (i * 4096), phys_bar + (i * 4096), VMM_MMIO);
    }
    
    // Parse capabilities
    m_cap_length = read32(XHCI_CAPLENGTH) & 0xFF;
    
    uint32_t params1 = read32(XHCI_HCSPARAMS1);
    m_max_slots = params1 & 0xFF;
    m_max_ports = (params1 >> 24) & 0xFF;
    
    if (!reset_controller()) return false;
    if (!initialize_data_structures()) return false;
    if (!configure_interrupter()) return false;

    // Configure MSI (Route to Vector 40)
    PCIDevice pci_dev = g_pci->get_xhci_device();
    if (!g_pci->configure_msi(pci_dev.bus, pci_dev.device, pci_dev.function, 40)) {
        if (g_vga) g_vga->write("xHCI: Failed to configure MSI!\n");
        return false;
    }

    if (!start_controller()) return false;

    // Detect connected devices immediately after starting
    enumerate_ports();

    return true;
}

void XHCI::stop() {
}

const char* XHCI::get_name() const {
    return "xHCI Controller";
}

void XHCI::configure_slot(uint32_t slot_id, uint32_t port_speed, uint32_t port_num) {
    if (g_vga) g_vga->write("xHCI: Configuring Contexts and Sending Address Device...\n");

    // Get Context Size from HCCPARAMS1 (Bit 2)
    uint32_t hccparams1 = read32(0x10);
    bool csz = (hccparams1 & (1 << 2)) != 0;
    uint32_t ctx_size = csz ? 64 : 32;
    uint32_t slot_ctx_idx = ctx_size / 4;
    uint32_t ep0_ctx_idx = (ctx_size * 2) / 4;

    // 1. Allocate Device Context
    uintptr_t dev_ctx_phys = pmm_alloc(1);
    uint64_t* dcbaa = reinterpret_cast<uint64_t*>(m_dcbaa_phys + pmm_hhdm_offset());
    dcbaa[slot_id] = dev_ctx_phys;

    // 2. Allocate Input Context
    uintptr_t in_ctx_phys = pmm_alloc(1);
    uint32_t* in_ctx = reinterpret_cast<uint32_t*>(in_ctx_phys + pmm_hhdm_offset());
    for(int i = 0; i < 1024; i++) in_ctx[i] = 0; // zero out 4KB page
    
    // Input Control Context (Add Slot and EP0)
    in_ctx[1] = 3; // Add Context Flags: Bit 0 = Slot, Bit 1 = EP0
    
    // Slot Context 
    // Dword 0: Context Entries = 1 (Bits 27:31), Speed = port_speed (Bits 20:23)
    in_ctx[slot_ctx_idx + 0] = (1 << 27) | (port_speed << 20);
    
    // Dword 1: Root Hub Port Number = port_num (Bits 16:23)
    in_ctx[slot_ctx_idx + 1] = (port_num << 16);
    
    // EP0 Context 
    // Max Packet Size: 512 for SuperSpeed (4), 64 for HighSpeed (3), 8 for FullSpeed (1)
    uint32_t max_packet_size = (port_speed == 4) ? 512 : ((port_speed == 3) ? 64 : 8);
    
    // Dword 1: CErr = 3 (Bits 1:2), EP Type = 4 (Bits 3:5), Max Packet Size (Bits 16:31)
    in_ctx[ep0_ctx_idx + 1] = (3 << 1) | (4 << 3) | (max_packet_size << 16);
    
    // Allocate EP0 Transfer Ring
    uintptr_t ep0_ring_phys = pmm_alloc(1);
    in_ctx[ep0_ctx_idx + 2] = ep0_ring_phys & 0xFFFFFFFF; // TR Dequeue Pointer Low
    in_ctx[ep0_ctx_idx + 3] = (ep0_ring_phys >> 32) & 0xFFFFFFFF; // TR Dequeue Pointer High
    in_ctx[ep0_ctx_idx + 2] |= 1; // DCS bit (Dequeue Cycle State)
    
    m_ep0_rings_phys[slot_id] = ep0_ring_phys;
    m_ep0_ring_indices[slot_id] = 0;
    m_ep0_ring_pcs[slot_id] = true;

    // 3. Send Address Device Command (TRB Type 11)
    TRB cmd = {0};
    cmd.parameter1 = in_ctx_phys & 0xFFFFFFFF;
    cmd.parameter2 = (in_ctx_phys >> 32) & 0xFFFFFFFF;
    cmd.control = (11 << 10) | (slot_id << 24);
    
    send_command(cmd);
}

bool XHCI::do_control_transfer(uint32_t slot_id, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, void* buffer_phys) {
    uintptr_t ring_phys = m_ep0_rings_phys[slot_id];
    if (ring_phys == 0) return false;
    
    TRB* ep0_ring = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    uint32_t& ring_idx = m_ep0_ring_indices[slot_id];
    bool& pcs = m_ep0_ring_pcs[slot_id];
    
    auto enqueue_trb = [&](TRB& trb) {
        if (pcs) trb.control |= 1; else trb.control &= ~1;
        ep0_ring[ring_idx] = trb;
        ring_idx++;
    };
    
    TRB setup = {0};
    setup.parameter1 = request_type | (request << 8) | (value << 16);
    setup.parameter2 = index | (length << 16);
    setup.status = 8;
    uint32_t trt = length > 0 ? 3 : 0; 
    setup.control = (TRB_SETUP_STAGE << 10) | (trt << 16) | (1 << 6);
    enqueue_trb(setup);
    
    if (length > 0) {
        TRB data = {0};
        data.parameter1 = reinterpret_cast<uintptr_t>(buffer_phys) & 0xFFFFFFFF;
        data.parameter2 = (reinterpret_cast<uintptr_t>(buffer_phys) >> 32) & 0xFFFFFFFF;
        data.status = length;
        uint32_t dir = (request_type & 0x80) ? 1 : 0;
        data.control = (TRB_DATA_STAGE << 10) | (dir << 16);
        enqueue_trb(data);
    }
    
    TRB status = {0};
    uint32_t dir = (request_type & 0x80) ? 0 : 1;
    if (length == 0) dir = 1;
    status.control = (TRB_STATUS_STAGE << 10) | (dir << 16) | (1 << 5);
    enqueue_trb(status);
    
    m_transfer_complete = false;
    ring_doorbell(slot_id, 1);
    
    while (!m_transfer_complete) {
        asm volatile("pause");
    }
    
    return true;
}

bool XHCI::configure_endpoint(uint32_t slot_id, uint8_t ep_num, uint8_t ep_type, uint16_t max_packet_size, uint8_t interval) {
    uintptr_t in_ctx_phys = pmm_alloc(1);
    uint32_t* in_ctx = reinterpret_cast<uint32_t*>(in_ctx_phys + pmm_hhdm_offset());
    for(int i = 0; i < 1024; i++) in_ctx[i] = 0;
    
    uint8_t dci = (ep_num * 2) + 1; // Assuming IN endpoint
    m_int_ep_dci[slot_id] = dci;
    
    uint32_t hccparams1 = read32(0x10);
    bool csz = (hccparams1 & (1 << 2)) != 0;
    uint32_t ctx_size = csz ? 64 : 32;
    // In the Input Context, the index for EP dci is dci + 1 (Index 0 is Input Control, 1 is Slot)
    uint32_t ep_ctx_idx = (ctx_size * (dci + 1)) / 4;
    
    in_ctx[1] = (1 << 0) | (1 << dci); // Add Slot and EP
    
    uint32_t slot_ctx_idx = ctx_size / 4;
    in_ctx[slot_ctx_idx + 0] = (dci << 27); // Context Entries
    
    in_ctx[ep_ctx_idx + 0] = (interval << 16);
    in_ctx[ep_ctx_idx + 1] = (3 << 1) | (ep_type << 3) | (max_packet_size << 16);
    
    uintptr_t ring_phys = pmm_alloc(1);
    in_ctx[ep_ctx_idx + 2] = ring_phys & 0xFFFFFFFF;
    in_ctx[ep_ctx_idx + 3] = (ring_phys >> 32) & 0xFFFFFFFF;
    in_ctx[ep_ctx_idx + 2] |= 1; // DCS
    
    m_int_rings_phys[slot_id] = ring_phys;
    m_int_ring_indices[slot_id] = 0;
    m_int_ring_pcs[slot_id] = true;
    
    // Initialize Link TRB at the end of the ring (index 255)
    TRB* ring = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    for (int i = 0; i < 256; i++) {
        ring[i].parameter1 = 0;
        ring[i].parameter2 = 0;
        ring[i].status = 0;
        ring[i].control = 0;
    }
    ring[255].parameter1 = ring_phys & 0xFFFFFFFF;
    ring[255].parameter2 = (ring_phys >> 32) & 0xFFFFFFFF;
    // Type 6 = Link TRB. Bit 1 = Toggle Cycle (TC).
    ring[255].control = (6 << 10) | (1 << 1); 
    
    in_ctx[ep_ctx_idx + 4] = max_packet_size;
    
    TRB cmd = {0};
    cmd.parameter1 = in_ctx_phys & 0xFFFFFFFF;
    cmd.parameter2 = (in_ctx_phys >> 32) & 0xFFFFFFFF;
    cmd.control = (12 << 10) | (slot_id << 24); // Configure Endpoint TRB
    
    m_transfer_complete = false;
    send_command(cmd);
    
    while (!m_transfer_complete) {
        asm volatile("pause");
    }
    
    if (g_vga) g_vga->write("xHCI: Interrupt Endpoint Configured!\n");
    return true;
}

bool XHCI::submit_interrupt_in(uint32_t slot_id, void* buffer_phys, uint32_t length) {
    uintptr_t ring_phys = m_int_rings_phys[slot_id];
    if (ring_phys == 0) return false;
    
    TRB* ring = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    uint32_t& ring_idx = m_int_ring_indices[slot_id];
    bool& pcs = m_int_ring_pcs[slot_id];
    
    TRB trb = {0};
    trb.parameter1 = reinterpret_cast<uintptr_t>(buffer_phys) & 0xFFFFFFFF;
    trb.parameter2 = (reinterpret_cast<uintptr_t>(buffer_phys) >> 32) & 0xFFFFFFFF;
    trb.status = length;
    trb.control = (TRB_NORMAL << 10) | (1 << 5); // Normal TRB, IOC
    
    if (pcs) trb.control |= 1; else trb.control &= ~1;
    ring[ring_idx] = trb;
    ring_idx++;
    
    if (ring_idx == 255) {
        // We reached the Link TRB. We must update its Cycle Bit to match the CURRENT pcs
        // so the hardware executes it and wraps around!
        if (pcs) {
            ring[255].control |= 1;
        } else {
            ring[255].control &= ~1;
        }
        
        ring_idx = 0;
        pcs = !pcs; // Toggle internal state for the next pass
    }
    
    ring_doorbell(slot_id, m_int_ep_dci[slot_id]);
    return true;
}

// Helper to add a ring to the bulk endpoint arrays and configure it
static uintptr_t alloc_and_init_ring(TRB*& ring_out) {
    uintptr_t ring_phys = pmm_alloc(1);
    ring_out = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    for (int i = 0; i < 256; i++) {
        ring_out[i].parameter1 = 0;
        ring_out[i].parameter2 = 0;
        ring_out[i].status     = 0;
        ring_out[i].control    = 0;
    }
    // Link TRB at slot 255
    ring_out[255].parameter1 = ring_phys & 0xFFFFFFFF;
    ring_out[255].parameter2 = (ring_phys >> 32) & 0xFFFFFFFF;
    ring_out[255].control    = (6 << 10) | (1 << 1); // Link TRB, TC
    return ring_phys;
}

bool XHCI::configure_bulk_endpoints(uint32_t slot_id, uint8_t ep_out_num, uint8_t ep_in_num, uint16_t max_packet_size) {
    uint32_t hccparams1 = read32(0x10);
    bool csz = (hccparams1 & (1 << 2)) != 0;
    uint32_t ctx_size = csz ? 64 : 32;

    uintptr_t in_ctx_phys = pmm_alloc(1);
    uint32_t* in_ctx = reinterpret_cast<uint32_t*>(in_ctx_phys + pmm_hhdm_offset());
    for (int i = 0; i < 1024; i++) in_ctx[i] = 0;

    // DCI: OUT endpoint = ep_out_num*2, IN endpoint = ep_in_num*2 + 1
    uint8_t dci_out = ep_out_num * 2;
    uint8_t dci_in  = ep_in_num  * 2 + 1;
    m_bulk_out_dci[slot_id] = dci_out;
    m_bulk_in_dci[slot_id]  = dci_in;

    // Input Control Context: add slot + both endpoints
    in_ctx[1] = (1 << 0) | (1 << dci_out) | (1 << dci_in);

    // Slot Context: set Context Entries = max DCI
    uint32_t slot_ctx_idx = ctx_size / 4;
    uint8_t max_dci = (dci_in > dci_out) ? dci_in : dci_out;
    // Keep existing slot info but update context entries
    // Read existing output context from DCBAA
    uintptr_t* dcbaa = reinterpret_cast<uintptr_t*>(m_dcbaa_phys + pmm_hhdm_offset());
    if (dcbaa[slot_id]) {
        uint32_t* out_ctx = reinterpret_cast<uint32_t*>(dcbaa[slot_id] + pmm_hhdm_offset());
        // Copy slot context from output ctx
        uint32_t out_slot_base = ctx_size / 4;
        for (uint32_t i = 0; i < ctx_size / 4; i++)
            in_ctx[slot_ctx_idx + i] = out_ctx[out_slot_base + i];
    }
    in_ctx[slot_ctx_idx + 0] = (in_ctx[slot_ctx_idx + 0] & ~(0x1F << 27)) | ((uint32_t)max_dci << 27);

    // Allocate OUT ring
    TRB* out_ring;
    uintptr_t out_ring_phys = alloc_and_init_ring(out_ring);
    m_bulk_out_rings_phys[slot_id]  = out_ring_phys;
    m_bulk_out_ring_indices[slot_id] = 0;
    m_bulk_out_ring_pcs[slot_id]    = true;

    // Allocate IN ring
    TRB* in_ring;
    uintptr_t in_ring_phys = alloc_and_init_ring(in_ring);
    m_bulk_in_rings_phys[slot_id]  = in_ring_phys;
    m_bulk_in_ring_indices[slot_id] = 0;
    m_bulk_in_ring_pcs[slot_id]    = true;

    // Configure OUT endpoint context (Bulk OUT = type 2)
    uint32_t ep_out_idx = (ctx_size * (dci_out + 1)) / 4;
    in_ctx[ep_out_idx + 1] = (2 << 3) | (max_packet_size << 16); // EP Type=Bulk OUT
    in_ctx[ep_out_idx + 2] = (out_ring_phys & 0xFFFFFFFF) | 1;   // TR Dequeue Lo | DCS
    in_ctx[ep_out_idx + 3] = (out_ring_phys >> 32) & 0xFFFFFFFF;
    in_ctx[ep_out_idx + 4] = max_packet_size;

    // Configure IN endpoint context (Bulk IN = type 6)
    uint32_t ep_in_idx = (ctx_size * (dci_in + 1)) / 4;
    in_ctx[ep_in_idx + 1] = (6 << 3) | (max_packet_size << 16);  // EP Type=Bulk IN
    in_ctx[ep_in_idx + 2] = (in_ring_phys & 0xFFFFFFFF) | 1;     // TR Dequeue Lo | DCS
    in_ctx[ep_in_idx + 3] = (in_ring_phys >> 32) & 0xFFFFFFFF;
    in_ctx[ep_in_idx + 4] = max_packet_size;

    // Send Configure Endpoint command
    TRB cmd = {0};
    cmd.parameter1 = in_ctx_phys & 0xFFFFFFFF;
    cmd.parameter2 = (in_ctx_phys >> 32) & 0xFFFFFFFF;
    cmd.control    = (12 << 10) | (slot_id << 24); // Configure Endpoint TRB

    m_transfer_complete = false;
    send_command(cmd);
    while (!m_transfer_complete) asm volatile("pause");

    return true;
}

bool XHCI::submit_bulk_out(uint32_t slot_id, void* buffer_phys, uint32_t length) {
    uintptr_t ring_phys = m_bulk_out_rings_phys[slot_id];
    if (!ring_phys) return false;

    TRB* ring = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    uint32_t& idx = m_bulk_out_ring_indices[slot_id];
    bool& pcs = m_bulk_out_ring_pcs[slot_id];

    TRB trb = {0};
    trb.parameter1 = reinterpret_cast<uintptr_t>(buffer_phys) & 0xFFFFFFFF;
    trb.parameter2 = (reinterpret_cast<uintptr_t>(buffer_phys) >> 32) & 0xFFFFFFFF;
    trb.status  = length;
    trb.control = (TRB_NORMAL << 10) | (1 << 5); // Normal TRB, IOC
    if (pcs) trb.control |= 1;

    ring[idx] = trb;
    idx++;
    if (idx == 255) {
        if (pcs) ring[255].control |= 1; else ring[255].control &= ~1;
        idx = 0; pcs = !pcs;
    }

    m_transfer_complete = false;
    ring_doorbell(slot_id, m_bulk_out_dci[slot_id]);
    int timeout = 0;
    while (!m_transfer_complete && timeout++ < 5000000) asm volatile("pause");
    return m_transfer_complete;
}

bool XHCI::submit_bulk_in(uint32_t slot_id, void* buffer_phys, uint32_t length) {
    uintptr_t ring_phys = m_bulk_in_rings_phys[slot_id];
    if (!ring_phys) return false;

    TRB* ring = reinterpret_cast<TRB*>(ring_phys + pmm_hhdm_offset());
    uint32_t& idx = m_bulk_in_ring_indices[slot_id];
    bool& pcs = m_bulk_in_ring_pcs[slot_id];

    TRB trb = {0};
    trb.parameter1 = reinterpret_cast<uintptr_t>(buffer_phys) & 0xFFFFFFFF;
    trb.parameter2 = (reinterpret_cast<uintptr_t>(buffer_phys) >> 32) & 0xFFFFFFFF;
    trb.status  = length;
    trb.control = (TRB_NORMAL << 10) | (1 << 5); // Normal TRB, IOC
    if (pcs) trb.control |= 1;

    ring[idx] = trb;
    idx++;
    if (idx == 255) {
        if (pcs) ring[255].control |= 1; else ring[255].control &= ~1;
        idx = 0; pcs = !pcs;
    }

    m_transfer_complete = false;
    ring_doorbell(slot_id, m_bulk_in_dci[slot_id]);
    int timeout = 0;
    while (!m_transfer_complete && timeout++ < 5000000) asm volatile("pause");
    return m_transfer_complete;
}

void XHCI::poll_event_ring() {
    uint32_t rts_off = read32(XHCI_RTSOFF) & ~0x1F;
    uint32_t ir0_base = rts_off + 0x0020;
    
    TRB* event_ring = reinterpret_cast<TRB*>(m_event_ring_phys + pmm_hhdm_offset());
    
    while (true) {
        TRB current_event = event_ring[m_event_ring_index];
        bool event_cycle = (current_event.control & 1) != 0;
        
        if (event_cycle != m_event_ring_ccs) {
            break; // No more events
        }
        
        // Advance ring pointer BEFORE processing callbacks to prevent re-entrancy issues
        m_event_ring_index++;
        if (m_event_ring_index == 256) {
            m_event_ring_index = 0;
            m_event_ring_ccs = !m_event_ring_ccs;
        }
        
        // Update ERDP to tell hardware we processed this TRB (must point to NEXT TRB)
        uintptr_t next_trb_phys = m_event_ring_phys + (m_event_ring_index * sizeof(TRB));
        write64(ir0_base + 0x18, next_trb_phys | 0x08); // ERDP + EHB
        
        uint32_t trb_type = (current_event.control >> 10) & 0x3F;
        
        if (trb_type == TRB_EVENT_CMD_COMPLETION) {
            uint32_t slot_id = (current_event.control >> 24) & 0xFF;
            uint32_t cmd_completion_code = (current_event.status >> 24) & 0xFF;
            
            uintptr_t cmd_trb_phys = current_event.parameter1 | (static_cast<uint64_t>(current_event.parameter2) << 32);
            uint32_t cmd_trb_offset = cmd_trb_phys - m_cmd_ring_phys;
            TRB* cmd_ring = reinterpret_cast<TRB*>(m_cmd_ring_phys + pmm_hhdm_offset());
            uint32_t cmd_type = (cmd_ring[cmd_trb_offset / sizeof(TRB)].control >> 10) & 0x3F;
            
            if (g_vga) {
                g_vga->write("xHCI: Command Completed. Code: ");
                char buf[3];
                buf[0] = (cmd_completion_code / 10) + '0';
                buf[1] = (cmd_completion_code % 10) + '0';
                buf[2] = '\0';
                g_vga->write(buf);
                g_vga->write(" Slot ID: ");
                buf[0] = slot_id + '0';
                buf[1] = '\0';
                g_vga->write(buf);
                g_vga->write("\n");
            }
            
            if (cmd_completion_code == 1 && slot_id > 0) {
                if (cmd_type == TRB_CMD_ENABLE_SLOT) {
                    configure_slot(slot_id, m_current_port_speed, m_current_port_num);
                } else if (cmd_type == TRB_CMD_ADDRESS_DEVICE) {
                    if (g_vga) g_vga->write("xHCI: Device Addressed Successfully! USB setup complete!\n");
                    if (g_usb_manager) g_usb_manager->register_device(slot_id, this);
                    m_port_setup_complete = true;
                } else if (cmd_type == 12) { // Configure Endpoint
                    m_transfer_complete = true;
                }
            } else if (cmd_completion_code != 1) {
                if (g_vga) g_vga->write("xHCI: TRB ERROR! Check context structures.\n");
                m_port_setup_complete = true;
                m_transfer_complete = true;
            }
            
        } else if (trb_type == TRB_EVENT_TRANSFER) {
            uint32_t ep_id = (current_event.control >> 16) & 0x1F;
            uint32_t completion_code = (current_event.status >> 24) & 0xFF;
            uint32_t residual = current_event.status & 0xFFFFFF;
            
            if (ep_id > 1) {
                static int evt_prints = 0;
                if (g_vga && evt_prints < 10) {
                    g_vga->write("xHCI: Transfer Event on EP ");
                    char buf[3];
                    buf[0] = ep_id + '0';
                    buf[1] = '\0';
                    if (ep_id > 9) {
                        buf[0] = (ep_id / 10) + '0';
                        buf[1] = (ep_id % 10) + '0';
                        buf[2] = '\0';
                    }
                    g_vga->write(buf);
                    g_vga->write(" Code: ");
                    buf[0] = (completion_code / 10) + '0';
                    buf[1] = (completion_code % 10) + '0';
                    buf[2] = '\0';
                    g_vga->write(buf);
                    g_vga->write(" Resid: ");
                    
                    // Print residual length
                    char res_buf[10];
                    int idx = 0;
                    uint32_t temp = residual;
                    if (temp == 0) { res_buf[idx++] = '0'; }
                    while (temp > 0) { res_buf[idx++] = (temp % 10) + '0'; temp /= 10; }
                    for (int i = 0; i < idx / 2; i++) { char t = res_buf[i]; res_buf[i] = res_buf[idx - 1 - i]; res_buf[idx - 1 - i] = t; }
                    res_buf[idx] = '\0';
                    g_vga->write(res_buf);
                    g_vga->write("\n");
                    
                    evt_prints++;
                }
                if (g_dualsense_driver) {
                    g_dualsense_driver->on_report_received();
                }
                // Always signal completion so bulk waiters (MSC) can proceed
                m_transfer_complete = true;
            } else {
                // EP 0/1 (control): signal completion
                m_transfer_complete = true;
            }
        } else if (trb_type == TRB_EVENT_PORT_STATUS) {
            if (g_vga) g_vga->write("xHCI: Port Status Change Event Received.\n");
        }
    }
}

void XHCI::handle_interrupt() {
    uint32_t op_base = m_cap_length;
    uint32_t rts_off = read32(XHCI_RTSOFF) & ~0x1F;
    uint32_t ir0_base = rts_off + 0x0020;
    
    // Clear Event Interrupt Status
    uint32_t sts = read32(op_base + XHCI_USBSTS);
    if (sts & USBSTS_EINT) {
        write32(op_base + XHCI_USBSTS, USBSTS_EINT); // Write 1 to clear
    }
    
    // Clear IP if set
    uint32_t iman = read32(ir0_base + 0x00);
    if (iman & 1) {
        write32(ir0_base + 0x00, iman | 1); // Write 1 to clear IP
    }
    
    poll_event_ring();
    
    // Acknowledge interrupt to LAPIC
    extern void lapic_eoi();
    lapic_eoi();
}

extern "C" void xhci_handle_interrupt() {
    if (g_xhci) {
        g_xhci->handle_interrupt();
    }
}

// Register as level 5 (after basic dev/pci)
REGISTER_MODULE(g_xhci, XHCI, 5_usb);
