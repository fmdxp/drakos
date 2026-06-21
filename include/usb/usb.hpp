#pragma once
#include <stdint.h>
#include "module.hpp"

// Forward declaration of XHCI logic to avoid circular dependencies
class XHCI;

struct USBDeviceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct USBConfigDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct USBInterfaceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct USBEndpointDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));


class USBManager : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;
    bool has_pending_tasks();

    // Called by XHCI ISR: records pending device, returns IMMEDIATELY (no heavy work)
    void register_device(uint8_t slot_id, XHCI* xhci_controller);
    
    // Called from the main kernel loop: does descriptor fetch, driver init
    void update();

private:
    static const int MAX_PENDING = 8;
    struct PendingDevice {
        uint8_t slot_id;
        XHCI*   xhci;
        bool    valid;
    };
    PendingDevice m_pending[MAX_PENDING] = {};
    
    void enumerate_device(uint8_t slot_id, XHCI* xhci);
};

extern USBManager* g_usb_manager;
