#pragma once
#include <stdint.h>
#include "module.hpp"

// Forward declaration of XHCI logic to avoid circular dependencies
class XHCI;

class USBDevice {
public:
    uint8_t slot_id;
    uint16_t vendor_id;
    uint16_t product_id;
    
    // Abstract interface that specific drivers will implement
    virtual ~USBDevice() = default;
    virtual void poll() = 0;
};

class USBManager : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Called by XHCI when a device gets a Slot ID and Address
    void register_device(uint8_t slot_id, XHCI* xhci_controller);
    
private:
    USBDevice* m_devices[16] = {nullptr};
};

extern USBManager* g_usb_manager;
