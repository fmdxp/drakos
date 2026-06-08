#include "xhci.hpp"
#include "pci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"

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
            reset_port(i);
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

    // 3. Send Address Device Command (TRB Type 11)
    TRB cmd = {0};
    cmd.parameter1 = in_ctx_phys & 0xFFFFFFFF;
    cmd.parameter2 = (in_ctx_phys >> 32) & 0xFFFFFFFF;
    cmd.control = (11 << 10) | (slot_id << 24);
    
    send_command(cmd);
}

void XHCI::handle_interrupt() {
    uint32_t op_base = m_cap_length;
    
    // Clear Event Interrupt Status
    uint32_t sts = read32(op_base + XHCI_USBSTS);
    if (sts & USBSTS_EINT) {
        write32(op_base + XHCI_USBSTS, USBSTS_EINT); // Write 1 to clear
    }
    
    uint32_t rts_off = read32(XHCI_RTSOFF) & ~0x1F;
    uint32_t ir0_base = rts_off + 0x0020;
    
    // Clear Interrupt Pending in IMAN
    uint32_t iman = read32(ir0_base + 0x00);
    if (iman & 1) {
        write32(ir0_base + 0x00, iman | 1); // Write 1 to clear IP
    }
    
    // Parse Event Ring
    TRB* event_ring = reinterpret_cast<TRB*>(m_event_ring_phys + pmm_hhdm_offset());
    TRB current_event = event_ring[m_event_ring_index];
    
    bool event_cycle = (current_event.control & 1) != 0;
    if (event_cycle == m_event_ring_ccs) {
        uint32_t trb_type = (current_event.control >> 10) & 0x3F;
        
        if (trb_type == TRB_EVENT_CMD_COMPLETION) {
            uint32_t slot_id = (current_event.control >> 24) & 0xFF;
            uint32_t cmd_completion_code = (current_event.status >> 24) & 0xFF;
            
            // To be precise we should read the physical pointer in current_event.parameter1 to know exactly which command completed.
            // For now, if we get a Slot ID, we assume it's the Enable Slot or Address Device completion.
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
            
            // Very simple state machine for the prototype
            // If we just got a Slot ID (Enable Slot success), configure it
            if (cmd_completion_code == 1 && slot_id > 0) {
                // Determine which command just completed by assuming state.
                // In a real driver, we'd check the command TRB parameter.
                // For now, if Address Device succeeded, tell the user!
                // We assume if m_current_port_speed is set, and it's the first Enable Slot, we call configure_slot.
                // Wait, if configure_slot completes, it ALSO generates a Command Completion with code 1.
                // To prevent infinite loop, we only configure slot if we haven't done it yet.
                static bool addressed = false;
                if (!addressed) {
                    configure_slot(slot_id, m_current_port_speed, m_current_port_num);
                    addressed = true;
                } else {
                    if (g_vga) g_vga->write("xHCI: Device Addressed Successfully! USB setup complete!\n");
                    g_usb_manager->register_device(slot_id, this);
                }
            } else if (cmd_completion_code != 1) {
                if (g_vga) g_vga->write("xHCI: TRB ERROR! Check context structures.\n");
            }
            
        } else if (trb_type == TRB_EVENT_PORT_STATUS) {
            if (g_vga) g_vga->write("xHCI: Port Status Change Event Received.\n");
        }
        
        m_event_ring_index++;
        if (m_event_ring_index == 256) {
            m_event_ring_index = 0;
            m_event_ring_ccs = !m_event_ring_ccs;
        }
        
        // Update ERDP
        uintptr_t current_trb_phys = m_event_ring_phys + (m_event_ring_index * sizeof(TRB));
        write64(ir0_base + 0x18, current_trb_phys | 0x08); // ERDP + EHB
    }
    
    // Acknowledge interrupt to LAPIC
    extern void lapic_eoi();
    lapic_eoi();
}

extern "C" __attribute__((interrupt)) void xhci_isr(InterruptFrame* frame) {
    (void)frame;
    if (g_xhci) {
        g_xhci->handle_interrupt();
    }
}

// Register as level 5 (after basic dev/pci)
REGISTER_MODULE(g_xhci, XHCI, 5_usb);
