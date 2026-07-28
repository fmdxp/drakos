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
class BluetoothHCI;

// L2CAP PSM values for HID
#define L2CAP_PSM_HID_CONTROL   0x0011
#define L2CAP_PSM_HID_INTERRUPT 0x0013

// L2CAP Signal Codes
#define L2CAP_CMD_REJECT        0x01
#define L2CAP_CONN_REQ          0x02
#define L2CAP_CONN_RSP          0x03
#define L2CAP_CONFIG_REQ        0x04
#define L2CAP_CONFIG_RSP        0x05
#define L2CAP_DISCONN_REQ       0x06
#define L2CAP_DISCONN_RSP       0x07
#define L2CAP_INFO_REQ          0x0A
#define L2CAP_INFO_RSP          0x0B

// L2CAP Connection Result codes
#define L2CAP_CR_SUCCESS        0x0000
#define L2CAP_CR_PENDING        0x0001

// L2CAP CID for signalling channel
#define L2CAP_CID_SIGNALLING    0x0001

struct L2CAPChannel {
    uint16_t local_cid;
    uint16_t remote_cid;
    uint16_t psm;
    bool     open;
};

class BluetoothL2CAP {
public:
    BluetoothL2CAP(BluetoothHCI* hci);
    
    // Process incoming ACL data
    void handle_acl_data(const uint8_t* data, size_t length);
    
    // Open an L2CAP connection to a PSM
    void connect(uint16_t psm);
    
    // Send data on a channel
    void send_data(uint16_t cid, const uint8_t* data, size_t length);
    
    // Check if HID Control / Interrupt channel is open
    bool is_hid_control_open() const;
    bool is_hid_interrupt_open() const;
    
    // Get the HID control / interrupt channel CID
    uint16_t get_hid_control_cid() const;
    uint16_t get_hid_interrupt_cid() const;

    // Reset channels on disconnect
    void reset();

private:
    BluetoothHCI* m_hci;
    
    static const int MAX_CHANNELS = 8;
    L2CAPChannel m_channels[MAX_CHANNELS];
    uint16_t m_next_local_cid;
    uint8_t  m_next_ident;

public:
    // Callback: called when HID data arrives on the interrupt channel
    void (*on_hid_data)(const uint8_t* data, size_t length);
    
    // Handle signalling commands (CID 0x0001)
    void handle_signalling(const uint8_t* data, size_t length);
    
    // Handle individual signalling commands
    void handle_conn_req(uint8_t ident, const uint8_t* data);
    void handle_conn_rsp(uint8_t ident, const uint8_t* data);
    void handle_config_req(uint8_t ident, const uint8_t* data);
    void handle_config_rsp(uint8_t ident, const uint8_t* data);
    void handle_info_req(uint8_t ident, const uint8_t* data);
    void handle_info_rsp(uint8_t ident, const uint8_t* data);
    
    // Send a signalling response
    void send_signalling(uint8_t code, uint8_t ident, const uint8_t* data, uint16_t length);
    
    // Send raw ACL data
    void send_acl(uint16_t cid, const uint8_t* data, uint16_t length);
    
    // Find channel by local CID
    L2CAPChannel* find_channel_by_local_cid(uint16_t cid);
    L2CAPChannel* find_channel_by_psm(uint16_t psm);
    L2CAPChannel* alloc_channel(uint16_t psm);
};

extern BluetoothL2CAP* g_bluetooth_l2cap;
