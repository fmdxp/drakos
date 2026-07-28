/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

class XHCI;

// HCI Packet types
#define HCI_CMD_PKT     0x01
#define HCI_ACL_PKT     0x02
#define HCI_EVT_PKT     0x04

// Intel specific HCI Commands
#define HCI_OP_INTEL_READ_VERSION   0xFC05
#define HCI_OP_INTEL_FW_DOWNLOAD    0xFC09
#define HCI_OP_INTEL_RESET          0xFC01

// HCI Event codes
#define HCI_EVT_INQUIRY_COMPLETE        0x01
#define HCI_EVT_INQUIRY_RESULT          0x02
#define HCI_EVT_CONNECTION_COMPLETE     0x03
#define HCI_EVT_DISCONNECTION_COMPLETE  0x05
#define HCI_EVT_AUTH_COMPLETE           0x06
#define HCI_EVT_REMOTE_NAME_COMPLETE    0x07
#define HCI_EVT_ENCRYPT_CHANGE          0x08
#define HCI_EVT_COMMAND_COMPLETE        0x0E
#define HCI_EVT_COMMAND_STATUS          0x0F
#define HCI_EVT_CONNECTION_REQUEST      0x04
#define HCI_EVT_PIN_CODE_REQUEST        0x16
#define HCI_EVT_LINK_KEY_REQUEST        0x17
#define HCI_EVT_LINK_KEY_NOTIFICATION   0x18
#define HCI_EVT_INQUIRY_RESULT_RSSI     0x22
#define HCI_EVT_EXT_INQUIRY_RESULT      0x2F
#define HCI_EVT_IO_CAPABILITY_REQUEST   0x31
#define HCI_EVT_IO_CAPABILITY_RESPONSE  0x32
#define HCI_EVT_USER_CONFIRM_REQUEST    0x33
#define HCI_EVT_SSP_COMPLETE            0x36

// HCI Command OpCodes (OGF << 10 | OCF)
// OGF 0x01 - Link Control
#define HCI_OP_INQUIRY              0x0401
#define HCI_OP_INQUIRY_CANCEL       0x0402
#define HCI_OP_CREATE_CONNECTION    0x0405
#define HCI_OP_ACCEPT_CONN_REQ     0x0409
#define HCI_OP_LINK_KEY_REPLY      0x040B
#define HCI_OP_LINK_KEY_NEG_REPLY  0x040C
#define HCI_OP_PIN_CODE_REPLY      0x040D
#define HCI_OP_PIN_CODE_NEG_REPLY  0x040E
#define HCI_OP_AUTH_REQUESTED      0x0411
#define HCI_OP_SET_CONN_ENCRYPT    0x0413
#define HCI_OP_REMOTE_NAME_REQ     0x0419
#define HCI_OP_IO_CAPABILITY_REPLY   0x042B
#define HCI_OP_USER_CONFIRM_REPLY    0x042C

// OGF 0x03 - Controller & Baseband
#define HCI_OP_RESET                0x0C03
#define HCI_OP_WRITE_AUTH_ENABLE    0x0C20
#define HCI_OP_WRITE_SCAN_ENABLE    0x0C1A
#define HCI_OP_WRITE_CLASS_OF_DEV   0x0C24
#define HCI_OP_WRITE_INQUIRY_MODE   0x0C45
#define HCI_OP_WRITE_SSP_MODE       0x0C56

// OGF 0x04 - Informational
#define HCI_OP_READ_BD_ADDR         0x1009
#define HCI_OP_READ_BUFFER_SIZE     0x1005
#define HCI_OP_READ_LOCAL_FEATURES  0x1003

// Bluetooth Device Address (BD_ADDR) - 6 bytes
struct BDAddr {
    uint8_t b[6];
} __attribute__((packed));

// Discovered device info
struct BTDevice {
    BDAddr  addr;
    uint8_t page_scan_rep_mode;
    uint32_t class_of_device;
    int8_t  rssi;
    char    name[249];
    bool    valid;
    bool    is_dualsense;  // Set when CoD matches gamepad
};

class BluetoothHCI {
public:
    BluetoothHCI(uint32_t slot_id, XHCI* xhci, 
                 uint8_t int_in_ep, uint8_t bulk_out_ep, uint8_t bulk_in_ep,
                 uint16_t int_in_mps, uint16_t bulk_mps, uint16_t vid);

    bool     is_intel;

    bool init();         // Reset, read BD_ADDR, configure
    void start_inquiry();  // Scan for nearby devices
    bool poll_events();    // Process one pending HCI event, return true if event was processed
    
    // Connection management
    void cancel_inquiry();
    void connect_device(const BTDevice* dev);
    void request_authentication();
    void set_connection_encryption(bool enable = true);
    bool is_authenticated() const { return m_authenticated; }
    
    // Get discovered devices
    BTDevice* get_devices() { return m_devices; }
    int get_device_count() const { return m_device_count; }
    bool is_inquiry_complete() const { return m_inquiry_complete; }
    
    // Get our local BD_ADDR
    const BDAddr& get_local_addr() const { return m_local_addr; }

    // L2CAP will need these
    uint32_t get_slot_id() const { return m_slot_id; }
    XHCI* get_xhci() const { return m_xhci; }
    uint16_t get_acl_handle() const { return m_acl_handle; }
    void on_acl_in_complete() { m_acl_in_ready = true; }

private:
    uint32_t m_slot_id;
    XHCI*    m_xhci;
    uint8_t  m_int_in_ep;
    uint8_t  m_bulk_out_ep;
    uint8_t  m_bulk_in_ep;
    uint16_t m_int_in_mps;
    uint16_t m_bulk_mps;

    BDAddr   m_local_addr;
    uint16_t m_acl_handle;
    bool     m_intel_fw_loaded;
    bool     m_inquiry_complete;
    bool     m_authenticated;
    bool     m_auth_requested;
    volatile bool m_acl_in_ready;

    // Event receive buffer (physical DMA)
    uintptr_t m_evt_buf_phys;
    uint8_t*  m_evt_buf_virt;

    // ACL receive buffer
    uintptr_t m_acl_in_buf_phys;
    uint8_t*  m_acl_in_buf_virt;

    // Discovered devices
    static const int MAX_DEVICES = 16;
    BTDevice m_devices[MAX_DEVICES];
    int m_device_count;

    // Send an HCI command via USB Control Transfer (endpoint 0)
    void send_command(uint16_t opcode, const uint8_t* params, uint8_t param_len);
    
    // Wait for a Command Complete or Command Status event
    bool wait_command_complete(uint16_t expected_opcode, uint8_t* out_params = nullptr, uint8_t max_params = 0);
    
    // Submit an Interrupt IN read for events
    void prime_event_read();
    
    // Submit a Bulk IN read for ACL data
    void prime_acl_read();
    
    // Parse an HCI event packet
    void handle_event(const uint8_t* data, size_t length);
    
    // Event payload handlers
    void handle_intel_version(const uint8_t* data, size_t length);
    
    // Parse Inquiry Result
    void handle_inquiry_result(const uint8_t* data, size_t length);
    void handle_inquiry_result_rssi(const uint8_t* data, size_t length);
    void handle_ext_inquiry_result(const uint8_t* data, size_t length);
    
    // Handle connection events
    void handle_connection_complete(const uint8_t* data);
    void handle_connection_request(const uint8_t* data);
    void handle_link_key_request(const uint8_t* data);
    void handle_pin_code_request(const uint8_t* data);
    void handle_io_capability_request(const uint8_t* data);
    void handle_user_confirm_request(const uint8_t* data);
};

extern BluetoothHCI* g_bluetooth_hci;
