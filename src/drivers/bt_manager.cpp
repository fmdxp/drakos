/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 */

#include "drivers/bt_manager.hpp"
#include "input/dualsense.hpp"
#include "input/hid_transport.hpp"
#include "vga.hpp"

BluetoothManager* g_bluetooth_manager = nullptr;

static uint32_t dualsense_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return ~crc;
}

class BTHIDTransport : public HIDTransport {
public:
    BTHIDTransport(BluetoothL2CAP* l2cap) : m_l2cap(l2cap), m_driver(nullptr) {}

    void start_listening(DualSenseDriver* driver) override {
        m_driver = driver;
    }

    void send_output_report(uint8_t report_id, const uint8_t* data, size_t length) override {
        (void)report_id;
        // Over Bluetooth, DualSense requires Output Report ID 0x31 (78 bytes total after 0xA2) with CRC32
        uint8_t pkt[78];
        for (size_t i = 0; i < 78; i++) pkt[i] = 0;

        pkt[0] = 0xA2; // HIDP_DATA_OUTPUT
        pkt[1] = 0x31; // BT Report ID for DualSense extended output
        pkt[2] = 0x10; // Tag / Sub-command (0x10 = enable extended features output)

        size_t copy_len = length;
        if (copy_len > 47) copy_len = 47;
        for (size_t i = 0; i < copy_len; i++) {
            pkt[3 + i] = data[i];
        }

        // Compute CRC32 over pkt[0..73] (74 bytes)
        uint32_t crc = dualsense_crc32(pkt, 74);
        pkt[74] = crc & 0xFF;
        pkt[75] = (crc >> 8) & 0xFF;
        pkt[76] = (crc >> 16) & 0xFF;
        pkt[77] = (crc >> 24) & 0xFF;

        uint16_t cid = m_l2cap->get_hid_control_cid();
        if (cid) m_l2cap->send_data(cid, pkt, 78);
    }
    
    void on_report_received(const uint8_t* report, size_t length) {
        if (m_driver) {
            // Over Bluetooth, DualSense report usually has 0xA1 (HIDP_DATA_INPUT) then Report ID
            if (length >= 2 && report[0] == 0xA1) {
                m_driver->on_report_received(report + 1, length - 1);
            }
        }
    }

private:
    BluetoothL2CAP* m_l2cap;
    DualSenseDriver* m_driver;
};

static BTHIDTransport* g_bt_transport = nullptr;

BluetoothManager::BluetoothManager(BluetoothHCI* hci, BluetoothL2CAP* l2cap)
    : m_hci(hci), m_l2cap(l2cap), m_state(BTState::IDLE), m_timer(0), m_target_dev(nullptr), m_dualsense(nullptr)
{
    g_bluetooth_manager = this;
    g_bt_transport = new BTHIDTransport(l2cap);
    
    // Set L2CAP callback
    m_l2cap->on_hid_data = BluetoothManager::on_hid_data_received;
}

void BluetoothManager::on_hid_data_received(const uint8_t* data, size_t length) {
    if (g_bt_transport) g_bt_transport->on_report_received(data, length);
}

bool BluetoothManager::update() {
    // Process HCI events first
    bool worked = m_hci->poll_events();

    // If an ACL connection is active, ensure we aren't stuck in Inquiry or Idle
    if (m_hci->get_acl_handle() != 0 && 
       (m_state == BTState::IDLE || m_state == BTState::INQUIRY || m_state == BTState::CANCELING_INQUIRY || m_state == BTState::CONNECTING)) {
        if (m_l2cap->is_hid_interrupt_open()) {
            if (!m_dualsense) m_dualsense = new DualSenseDriver(g_bt_transport);
            m_state = BTState::CONNECTED;
        } else if (!m_l2cap->is_hid_control_open()) {
            m_l2cap->connect(L2CAP_PSM_HID_CONTROL);
            m_state = BTState::L2CAP_CONNECTING_CTRL;
            m_timer = 50000000;
        }
    }

    switch (m_state) {
        case BTState::IDLE:
            handle_idle();
            break;
        case BTState::INQUIRY:
            handle_inquiry();
            break;
        case BTState::CANCELING_INQUIRY:
            handle_canceling_inquiry();
            break;
        case BTState::CONNECTING:
            handle_connecting();
            break;
        case BTState::AUTHENTICATING:
            handle_authenticating();
            break;
        case BTState::L2CAP_CONNECTING_CTRL:
            handle_l2cap_connecting_ctrl();
            break;
        case BTState::L2CAP_CONNECTING_INTR:
            handle_l2cap_connecting_intr();
            break;
        case BTState::CONNECTED:
            handle_connected();
            break;
    }

    return worked;
}

void BluetoothManager::handle_idle() {
    if (m_hci->get_acl_handle() != 0) {
        if (m_l2cap->is_hid_interrupt_open()) {
            if (!m_dualsense) m_dualsense = new DualSenseDriver(g_bt_transport);
            m_state = BTState::CONNECTED;
        } else {
            m_l2cap->connect(L2CAP_PSM_HID_CONTROL);
            m_state = BTState::L2CAP_CONNECTING_CTRL;
            m_timer = 50000000;
        }
        return;
    }

    if (m_timer == 0) {
        // Auto-start inquiry on boot
        m_hci->start_inquiry();
        m_state = BTState::INQUIRY;
        m_timer = 50000000; // very large timeout
    }
}

void BluetoothManager::handle_inquiry() {
    m_timer--;
    
    // Check if we found a DualSense
    BTDevice* devs = m_hci->get_devices();
    for (int i = 0; i < m_hci->get_device_count(); i++) {
        if (devs[i].valid && devs[i].is_dualsense) {
            // Found one! Cancel inquiry first before connecting.
            m_target_dev = &devs[i];
            m_hci->cancel_inquiry();
            m_state = BTState::CANCELING_INQUIRY;
            m_timer = 50000000;
            return;
        }
    }
    
    if (m_hci->is_inquiry_complete() || m_timer == 0) {
        // Restart inquiry
        m_hci->start_inquiry();
        m_timer = 50000000;
    }
}

void BluetoothManager::handle_canceling_inquiry() {
    m_timer--;
    
    if (m_hci->is_inquiry_complete() || m_timer == 0) {
        m_hci->connect_device(m_target_dev);
        m_state = BTState::CONNECTING;
        m_timer = 50000000;
    }
}

void BluetoothManager::handle_connecting() {
    m_timer--;
    
    if (m_hci->get_acl_handle() != 0) {
        m_l2cap->connect(L2CAP_PSM_HID_CONTROL);
        m_state = BTState::L2CAP_CONNECTING_CTRL;
        m_timer = 50000000;
    } else if (m_timer == 0) {
        if (g_vga) g_vga->write("BTManager: ACL connection timed out.\n");
        m_state = BTState::IDLE;
        m_timer = 0;
    }
}

void BluetoothManager::handle_authenticating() {
    m_timer--;
    if (m_timer % 10000000 == 0 && g_vga) {
        g_vga->write("BTManager: Waiting for Auth & Encryption...\n");
    }
    
    if (m_hci->is_authenticated()) {
        if (g_vga) g_vga->write("BTManager: Link Authenticated & Encrypted! Opening HID Control...\n");
        m_l2cap->connect(L2CAP_PSM_HID_CONTROL);
        m_state = BTState::L2CAP_CONNECTING_CTRL;
        m_timer = 50000000;
    } else if (m_timer == 0) {
        if (g_vga) g_vga->write("BTManager: Auth timed out.\n");
        m_state = BTState::IDLE;
        m_timer = 0;
    }
}

void BluetoothManager::handle_l2cap_connecting_ctrl() {
    m_timer--;
    if (m_l2cap->is_hid_control_open()) {
        m_l2cap->connect(L2CAP_PSM_HID_INTERRUPT);
        m_state = BTState::L2CAP_CONNECTING_INTR;
        m_timer = 50000000;
    } else if (m_timer == 0) {
        if (g_vga) g_vga->write("BTManager: L2CAP Control timed out.\n");
        m_state = BTState::IDLE;
        m_timer = 0;
    }
}

void BluetoothManager::handle_l2cap_connecting_intr() {
    m_timer--;
    if (m_l2cap->is_hid_interrupt_open()) {
        if (g_vga) g_vga->write("BTManager: HID Interrupt open! DualSense ready.\n");
        m_dualsense = new DualSenseDriver(g_bt_transport);
        m_state = BTState::CONNECTED;
    } else if (m_timer == 0) {
        if (g_vga) g_vga->write("BTManager: L2CAP Interrupt timed out.\n");
        m_state = BTState::IDLE; // Reset and try again
        m_timer = 0;
    }
}

void BluetoothManager::handle_connected() {
    if (m_hci->get_acl_handle() == 0) {
        if (g_vga) g_vga->write("BTManager: Controller disconnected.\n");
        m_state = BTState::IDLE;
        m_timer = 0;
    }
}
