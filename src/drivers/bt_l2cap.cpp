/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 */

#include "drivers/bt_l2cap.hpp"
#include "drivers/bluetooth.hpp"
#include "drivers/xhci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"

BluetoothL2CAP* g_bluetooth_l2cap = nullptr;

/* static void print_hex8(uint8_t val) {
    if (!g_vga) return;
    const char* hex = "0123456789ABCDEF";
    char str[3];
    str[0] = hex[(val >> 4) & 0xF];
    str[1] = hex[val & 0xF];
    str[2] = 0;
    g_vga->write(str);
} */

BluetoothL2CAP::BluetoothL2CAP(BluetoothHCI* hci) 
    : m_hci(hci), m_next_local_cid(0x0040), m_next_ident(1), on_hid_data(nullptr)
{
    for (int i = 0; i < MAX_CHANNELS; i++) {
        m_channels[i].local_cid = 0;
        m_channels[i].remote_cid = 0;
        m_channels[i].open = false;
    }
    g_bluetooth_l2cap = this;
}

L2CAPChannel* BluetoothL2CAP::find_channel_by_local_cid(uint16_t cid) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].local_cid == cid) return &m_channels[i];
    }
    return nullptr;
}

L2CAPChannel* BluetoothL2CAP::find_channel_by_psm(uint16_t psm) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].psm == psm) return &m_channels[i];
    }
    return nullptr;
}

L2CAPChannel* BluetoothL2CAP::alloc_channel(uint16_t psm) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].local_cid == 0) {
            m_channels[i].local_cid = m_next_local_cid++;
            m_channels[i].psm = psm;
            m_channels[i].open = false;
            return &m_channels[i];
        }
    }
    return nullptr;
}

// Check if HID control is open
bool BluetoothL2CAP::is_hid_control_open() const {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].psm == L2CAP_PSM_HID_CONTROL && m_channels[i].open) {
            return true;
        }
    }
    return false;
}

// Check if HID interrupt is open
bool BluetoothL2CAP::is_hid_interrupt_open() const {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].psm == L2CAP_PSM_HID_INTERRUPT && m_channels[i].open) {
            return true;
        }
    }
    return false;
}

uint16_t BluetoothL2CAP::get_hid_control_cid() const {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].psm == L2CAP_PSM_HID_CONTROL && m_channels[i].open) {
            return m_channels[i].remote_cid;
        }
    }
    return 0;
}

uint16_t BluetoothL2CAP::get_hid_interrupt_cid() const {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].psm == L2CAP_PSM_HID_INTERRUPT && m_channels[i].open) {
            return m_channels[i].remote_cid;
        }
    }
    return 0;
}

void BluetoothL2CAP::reset() {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        m_channels[i].local_cid = 0;
        m_channels[i].remote_cid = 0;
        m_channels[i].psm = 0;
        m_channels[i].open = false;
    }
    m_next_local_cid = 0x0040;
    m_next_ident = 1;
}

// Send L2CAP Signalling Command
void BluetoothL2CAP::send_signalling(uint8_t code, uint8_t ident, const uint8_t* data, uint16_t length) {
    uint8_t sig_buf[64];
    sig_buf[0] = code;
    sig_buf[1] = ident;
    sig_buf[2] = length & 0xFF;
    sig_buf[3] = (length >> 8) & 0xFF;
    
    for (uint16_t i = 0; i < length; i++) sig_buf[4 + i] = data[i];
    
    send_acl(L2CAP_CID_SIGNALLING, sig_buf, length + 4);
}

// Connect out to a PSM (e.g. 0x11 for HID Control)
void BluetoothL2CAP::connect(uint16_t psm) {
    L2CAPChannel* ch = alloc_channel(psm);
    if (!ch) return;
    
    if (g_vga) g_vga->write("L2CAP: Requesting connection...\n");
    
    uint8_t data[4];
    data[0] = psm & 0xFF;
    data[1] = (psm >> 8) & 0xFF;
    data[2] = ch->local_cid & 0xFF;
    data[3] = (ch->local_cid >> 8) & 0xFF;
    
    send_signalling(L2CAP_CONN_REQ, m_next_ident++, data, 4);
}

// Incoming ACL data from HCI
void BluetoothL2CAP::handle_acl_data(const uint8_t* data, size_t length) {
    if (length < 8) return;
    
    // ACL Header: Handle(12 bits)+Flags(4 bits), Total_Len(16 bits)
    uint16_t handle_flags = data[0] | (data[1] << 8);
    // uint16_t acl_len = data[2] | (data[3] << 8);
    
    uint16_t handle = handle_flags & 0x0FFF;
    if (handle != m_hci->get_acl_handle()) return; // Not for our device
    
    // L2CAP Header: PDU_Len(16 bits), Channel_ID(16 bits)
    // uint16_t pdu_len = data[4] | (data[5] << 8);
    uint16_t cid = data[6] | (data[7] << 8);
    
    const uint8_t* payload = data + 8;
    size_t payload_len = length - 8;

    if (cid == L2CAP_CID_SIGNALLING) {
        handle_signalling(payload, payload_len);
    } else {
        // Data on a dynamic channel
        L2CAPChannel* ch = find_channel_by_local_cid(cid);
        if (ch && ch->psm == L2CAP_PSM_HID_INTERRUPT && on_hid_data) {
            on_hid_data(payload, payload_len);
        }
    }
}

// Handle Signalling Packets
void BluetoothL2CAP::handle_signalling(const uint8_t* data, size_t length) {
    if (length < 4) return;
    
    uint8_t code = data[0];
    uint8_t ident = data[1];
    // uint16_t len = data[2] | (data[3] << 8);
    const uint8_t* payload = data + 4;

    switch (code) {
        case L2CAP_CONN_REQ: handle_conn_req(ident, payload); break;
        case L2CAP_CONN_RSP: handle_conn_rsp(ident, payload); break;
        case L2CAP_CONFIG_REQ: handle_config_req(ident, payload); break;
        case L2CAP_CONFIG_RSP: handle_config_rsp(ident, payload); break;
        case L2CAP_INFO_REQ: handle_info_req(ident, payload); break;
        case L2CAP_INFO_RSP: handle_info_rsp(ident, payload); break;
    }
}

void BluetoothL2CAP::handle_conn_req(uint8_t ident, const uint8_t* data) {
    uint16_t psm = data[0] | (data[1] << 8);
    uint16_t scid = data[2] | (data[3] << 8);
    
    L2CAPChannel* ch = find_channel_by_psm(psm);
    if (!ch) ch = alloc_channel(psm);
    
    if (ch) {
        ch->remote_cid = scid;
        
        // Accept connection
        uint8_t rsp[8];
        rsp[0] = ch->local_cid & 0xFF;
        rsp[1] = (ch->local_cid >> 8) & 0xFF;
        rsp[2] = scid & 0xFF;
        rsp[3] = (scid >> 8) & 0xFF;
        rsp[4] = L2CAP_CR_SUCCESS;
        rsp[5] = 0x00;
        rsp[6] = 0x00; // No further info
        rsp[7] = 0x00;
        
        send_signalling(L2CAP_CONN_RSP, ident, rsp, 8);
        
        // Immediately send Config Req
        uint8_t cfg[4];
        cfg[0] = ch->remote_cid & 0xFF;
        cfg[1] = (ch->remote_cid >> 8) & 0xFF;
        cfg[2] = 0x00; // Flags
        cfg[3] = 0x00;
        send_signalling(L2CAP_CONFIG_REQ, m_next_ident++, cfg, 4);
    }
}

void BluetoothL2CAP::handle_conn_rsp(uint8_t ident, const uint8_t* data) {
    (void)ident;
    uint16_t dcid = data[0] | (data[1] << 8);
    uint16_t scid = data[2] | (data[3] << 8);
    uint16_t result = data[4] | (data[5] << 8);
    
    L2CAPChannel* ch = find_channel_by_local_cid(scid);
    if (ch) {
        if (result == L2CAP_CR_SUCCESS) {
            ch->remote_cid = dcid;
            
            // Send Config Req
            uint8_t cfg[4];
            cfg[0] = ch->remote_cid & 0xFF;
            cfg[1] = (ch->remote_cid >> 8) & 0xFF;
            cfg[2] = 0x00; // Flags
            cfg[3] = 0x00;
            send_signalling(L2CAP_CONFIG_REQ, m_next_ident++, cfg, 4);
        } else if (result == L2CAP_CR_PENDING) {
            ch->remote_cid = dcid;
            uint8_t info_req[2] = { 0x02, 0x00 }; // InfoType 0x0002 (Extended Features Mask)
            send_signalling(L2CAP_INFO_REQ, m_next_ident++, info_req, 2);
        }
    }
}

void BluetoothL2CAP::handle_config_req(uint8_t ident, const uint8_t* data) {
    uint16_t dcid = data[0] | (data[1] << 8);
    // uint16_t flags = data[2] | (data[3] << 8);
    
    L2CAPChannel* ch = find_channel_by_local_cid(dcid);
    if (ch) {
        // Accept configuration
        uint8_t rsp[6];
        rsp[0] = ch->remote_cid & 0xFF;
        rsp[1] = (ch->remote_cid >> 8) & 0xFF;
        rsp[2] = 0x00; // Flags
        rsp[3] = 0x00;
        rsp[4] = 0x00; // Success
        rsp[5] = 0x00;
        
        send_signalling(L2CAP_CONFIG_RSP, ident, rsp, 6);
        ch->open = true;
    }
}

void BluetoothL2CAP::handle_config_rsp(uint8_t ident, const uint8_t* data) {
    (void)ident;
    uint16_t scid = data[0] | (data[1] << 8);
    uint16_t result = data[4] | (data[5] << 8);
    
    L2CAPChannel* ch = find_channel_by_local_cid(scid);
    if (ch && result == 0x0000) {
        ch->open = true;
    }
}

void BluetoothL2CAP::handle_info_req(uint8_t ident, const uint8_t* data) {
    uint16_t info_type = data[0] | (data[1] << 8);
    
    // Respond Not Supported
    uint8_t rsp[4];
    rsp[0] = info_type & 0xFF;
    rsp[1] = (info_type >> 8) & 0xFF;
    rsp[2] = 0x01; // Not Supported
    rsp[3] = 0x00;
    
    send_signalling(L2CAP_INFO_RSP, ident, rsp, 4);
}

void BluetoothL2CAP::handle_info_rsp(uint8_t ident, const uint8_t* data) {
    (void)ident;
    (void)data;
    
    // Now that Info Rsp has completed for our channel, send Config Req!
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (m_channels[i].local_cid != 0 && m_channels[i].remote_cid != 0 && !m_channels[i].open) {
            uint8_t cfg[4];
            cfg[0] = m_channels[i].remote_cid & 0xFF;
            cfg[1] = (m_channels[i].remote_cid >> 8) & 0xFF;
            cfg[2] = 0x00; // Flags
            cfg[3] = 0x00;
            send_signalling(L2CAP_CONFIG_REQ, m_next_ident++, cfg, 4);
        }
    }
}

// Send ACL data to HCI
void BluetoothL2CAP::send_acl(uint16_t cid, const uint8_t* data, uint16_t length) {
    uint16_t handle = m_hci->get_acl_handle();
    if (handle == 0) return;
    
    uint16_t flags = 0x0; // First automatically flushable packet (PB = 00b)
    
    uintptr_t buf_phys = pmm_alloc(1);
    uint8_t* buf = reinterpret_cast<uint8_t*>(buf_phys + pmm_hhdm_offset());
    
    // ACL Header
    buf[0] = handle & 0xFF;
    buf[1] = ((handle >> 8) & 0x0F) | (flags << 4);
    uint16_t acl_len = length + 4; // L2CAP Header + payload
    buf[2] = acl_len & 0xFF;
    buf[3] = (acl_len >> 8) & 0xFF;
    
    // L2CAP Header
    buf[4] = length & 0xFF;
    buf[5] = (length >> 8) & 0xFF;
    buf[6] = cid & 0xFF;
    buf[7] = (cid >> 8) & 0xFF;
    
    for (uint16_t i = 0; i < length; i++) buf[8 + i] = data[i];
    
    // Send via Bulk OUT
    m_hci->get_xhci()->submit_bulk_out(m_hci->get_slot_id(), reinterpret_cast<void*>(buf_phys), acl_len + 4);
    
    pmm_free_page(buf_phys);
}

void BluetoothL2CAP::send_data(uint16_t cid, const uint8_t* data, size_t length) {
    send_acl(cid, data, length);
}
