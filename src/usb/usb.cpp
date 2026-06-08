#include "usb/usb.hpp"
#include "vga.hpp"

bool USBManager::start() {
    if (g_vga) g_vga->write("USB: Manager Initialized.\n");
    return true;
}

const char* USBManager::get_name() const {
    return "USB Core Manager";
}

void USBManager::stop() {
    // Nothing to do for now
}

void USBManager::register_device(uint8_t slot_id, XHCI* xhci_controller) {
    (void)xhci_controller;
    
    if (g_vga) {
        g_vga->write("USB: New Device Registered with Slot ID: ");
        char buf[2];
        buf[0] = slot_id + '0';
        buf[1] = '\0';
        g_vga->write(buf);
        g_vga->write("\n");
    }
    
    // Later we will fetch descriptors and load DualSenseDriver here
}

REGISTER_MODULE(g_usb_manager, USBManager, 4_usb_core);
