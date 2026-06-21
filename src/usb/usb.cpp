#include "usb/usb.hpp"
#include "drivers/xhci.hpp"
#include "input/dualsense.hpp"
#include "vga.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "heap.hpp"

// Hex char helper
static char hex_digit(uint8_t v) {
    if (v < 10) return '0' + v;
    return 'A' + (v - 10);
}

bool USBManager::start() {
    // TEMPORARY: Disable USB Manager to test multithreading
    return true;
}

const char* USBManager::get_name() const {
    return "USB Core Manager";
}

void USBManager::stop() {}

// Called from ISR: ONLY saves slot info, returns immediately
void USBManager::register_device(uint8_t slot_id, XHCI* xhci_controller) {
    if (g_vga) {
        g_vga->write("USB: New Device Registered with Slot ID: ");
        char buf[2];
        buf[0] = slot_id + '0';
        buf[1] = '\0';
        g_vga->write(buf);
        g_vga->write("\n");
    }
    
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!m_pending[i].valid) {
            m_pending[i].slot_id = slot_id;
            m_pending[i].xhci    = xhci_controller;
            m_pending[i].valid   = true;
            return;
        }
    }
    
    if (g_vga) g_vga->write("USB: Pending queue full!\n");
}

// Called from main kernel loop: safe to do heavy work here
void USBManager::update() {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (m_pending[i].valid) {
            m_pending[i].valid = false;
            enumerate_device(m_pending[i].slot_id, m_pending[i].xhci);
        }
    }
}

void USBManager::enumerate_device(uint8_t slot_id, XHCI* xhci) {
    // --- Step 1: Device Descriptor ---
    uintptr_t dev_buf_phys = pmm_alloc(1);
    void*     dev_buf_virt = reinterpret_cast<void*>(dev_buf_phys + pmm_hhdm_offset());
    
    bool ok = xhci->do_control_transfer(slot_id, 0x80, 6, 0x0100, 0, 18,
                                         reinterpret_cast<void*>(dev_buf_phys));
    if (!ok) return;
    
    USBDeviceDescriptor* dev_desc = reinterpret_cast<USBDeviceDescriptor*>(dev_buf_virt);
    uint16_t vid = dev_desc->idVendor;
    uint16_t pid = dev_desc->idProduct;
    
    if (g_vga) {
        char hex[5];
        g_vga->write("USB: Found Device! VID: 0x");
        hex[0] = hex_digit((vid >> 12) & 0xF); hex[1] = hex_digit((vid >> 8) & 0xF);
        hex[2] = hex_digit((vid >>  4) & 0xF); hex[3] = hex_digit(vid & 0xF); hex[4] = '\0';
        g_vga->write(hex);
        g_vga->write(", PID: 0x");
        hex[0] = hex_digit((pid >> 12) & 0xF); hex[1] = hex_digit((pid >> 8) & 0xF);
        hex[2] = hex_digit((pid >>  4) & 0xF); hex[3] = hex_digit(pid & 0xF);
        g_vga->write(hex);
        g_vga->write("\n");
    }
    
    // --- Step 2: Configuration Descriptor header (9 bytes) ---
    uintptr_t cfg_buf_phys = pmm_alloc(2); // 8KB - enough for any config descriptor
    void*     cfg_buf_virt = reinterpret_cast<void*>(cfg_buf_phys + pmm_hhdm_offset());
    
    ok = xhci->do_control_transfer(slot_id, 0x80, 6, 0x0200, 0, 9,
                                    reinterpret_cast<void*>(cfg_buf_phys));
    if (!ok) return;
    
    USBConfigDescriptor* cfg = reinterpret_cast<USBConfigDescriptor*>(cfg_buf_virt);
    uint16_t total_len = cfg->wTotalLength;
    
    // Sanity check: real config descriptors are always < 1KB
    if (total_len > 4096 || total_len < 9) return;
    
    // --- Step 3: Full Configuration Descriptor ---
    ok = xhci->do_control_transfer(slot_id, 0x80, 6, 0x0200, 0, total_len,
                                    reinterpret_cast<void*>(cfg_buf_phys));
    if (!ok) return;
    
    // --- Step 4: Parse - find first Interrupt IN endpoint inside a HID Interface ---
    uint8_t* ptr = reinterpret_cast<uint8_t*>(cfg_buf_virt);
    uint8_t* end = ptr + total_len;
    uint8_t  int_in_ep         = 0;
    uint16_t int_in_max_packet = 64;
    bool in_hid_interface = false;
    
    while (ptr < end) {
        uint8_t len  = ptr[0];
        uint8_t type = ptr[1];
        if (len == 0) break;
        
        if (type == 4) { // Interface Descriptor
            USBInterfaceDescriptor* intf = reinterpret_cast<USBInterfaceDescriptor*>(ptr);
            // Class 3 is HID
            if (intf->bInterfaceClass == 3) {
                in_hid_interface = true;
            } else {
                in_hid_interface = false;
            }
        }
        
        if (type == 5 && in_hid_interface) { // Endpoint Descriptor
            USBEndpointDescriptor* ep = reinterpret_cast<USBEndpointDescriptor*>(ptr);
            bool is_interrupt_in = ((ep->bmAttributes & 0x03) == 3) &&
                                   ((ep->bEndpointAddress & 0x80) != 0);
            if (is_interrupt_in) {
                int_in_ep         = ep->bEndpointAddress & 0x0F;
                int_in_max_packet = ep->wMaxPacketSize;
                break;
            }
        }
        ptr += len;
    }
    
    // --- Step 5: Device-specific driver instantiation ---
    if (vid == 0x054C && pid == 0x0CE6) {
        if (g_vga) g_vga->write("USB: SONY DualSense Detected! Loading Driver...\n");
        
        // SET_CONFIGURATION to activate the device
        uint8_t cfg_val = reinterpret_cast<USBConfigDescriptor*>(cfg_buf_virt)->bConfigurationValue;
        xhci->do_control_transfer(slot_id, 0x00, 9, cfg_val, 0, 0,
                                   reinterpret_cast<void*>(pmm_alloc(1))); // dummy buf for status
        
        // SONY DUALSENSE "WAKE UP" PACKET
        // The DualSense controller sometimes stays in a dummy state (sending only Sequence Counters and centered sticks)
        // until the OS driver reads a Feature Report (like the MAC address - Feature Report 0x09).
        uintptr_t in_buf_phys = pmm_alloc(1);
        
        // Find the interface number of the HID interface to send the GET_REPORT to
        uint8_t hid_intf_num = 3; // Default for DualSense
        ptr = reinterpret_cast<uint8_t*>(cfg_buf_virt);
        while (ptr < end) {
            if (ptr[1] == 4 && reinterpret_cast<USBInterfaceDescriptor*>(ptr)->bInterfaceClass == 3) {
                hid_intf_num = reinterpret_cast<USBInterfaceDescriptor*>(ptr)->bInterfaceNumber;
                break;
            }
            ptr += ptr[0];
        }

        // GET_REPORT: bmRequestType=0xA1, bRequest=0x01, wValue=0x0309 (Feature Report 0x09), wIndex=Interface, wLength=64
        xhci->do_control_transfer(slot_id, 0xA1, 0x01, 0x0309, hid_intf_num, 64, reinterpret_cast<void*>(in_buf_phys));
        
        if (int_in_ep != 0) {
            g_dualsense_driver = new DualSenseDriver(slot_id, int_in_ep, int_in_max_packet, xhci);
        } else {
            if (g_vga) g_vga->write("USB: DualSense Interrupt IN endpoint not found!\n");
        }
    }
}

REGISTER_MODULE(g_usb_manager, USBManager, 3_drv);
