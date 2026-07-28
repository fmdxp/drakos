/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 */

#include "drivers/bluetooth.hpp"
#include "drivers/bt_l2cap.hpp"
#include "drivers/xhci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"
#include "vfs.hpp"

BluetoothHCI* g_bluetooth_hci = nullptr;

// --- Hex print helpers ---
static void print_hex8(uint8_t val) {
    if (!g_vga) return;
    const char* h = "0123456789ABCDEF";
    char buf[3] = { h[val >> 4], h[val & 0x0F], 0 };
    g_vga->write(buf);
}

static void print_bdaddr(const BDAddr& addr) {
    if (!g_vga) return;
    for (int i = 5; i >= 0; i--) {
        print_hex8(addr.b[i]);
        if (i > 0) g_vga->write(":");
    }
}

// ---- Constructor ----
BluetoothHCI::BluetoothHCI(uint32_t slot_id, XHCI* xhci,
                           uint8_t int_in_ep, uint8_t bulk_out_ep, uint8_t bulk_in_ep,
                           uint16_t int_in_mps, uint16_t bulk_mps, uint16_t vid)
    : m_slot_id(slot_id), m_xhci(xhci),
      m_int_in_ep(int_in_ep), m_bulk_out_ep(bulk_out_ep), m_bulk_in_ep(bulk_in_ep),
      m_int_in_mps(int_in_mps), m_bulk_mps(bulk_mps),
      m_acl_handle(0), m_device_count(0)
{
    is_intel = (vid == 0x8087);
    m_intel_fw_loaded = false;
    m_inquiry_complete = false;
    m_authenticated = false;
    m_auth_requested = false;
    m_acl_in_ready = false;
    for (int i = 0; i < 6; i++) m_local_addr.b[i] = 0;
    for (int i = 0; i < MAX_DEVICES; i++) m_devices[i].valid = false;

    // Allocate DMA buffers
    m_evt_buf_phys = pmm_alloc(1);
    m_evt_buf_virt = reinterpret_cast<uint8_t*>(m_evt_buf_phys + pmm_hhdm_offset());

    m_acl_in_buf_phys = pmm_alloc(1);
    m_acl_in_buf_virt = reinterpret_cast<uint8_t*>(m_acl_in_buf_phys + pmm_hhdm_offset());
}

// ---- Send HCI Command via USB Control Transfer ----
// Bluetooth spec: bmRequestType=0x20, bRequest=0x00, wValue=0, wIndex=0, wLength=param_len+3
// The command packet is: opcode(2) + param_len(1) + params(param_len)
void BluetoothHCI::send_command(uint16_t opcode, const uint8_t* params, uint8_t param_len) {
    uintptr_t cmd_buf_phys = pmm_alloc_page();
    uint8_t* cmd = reinterpret_cast<uint8_t*>(cmd_buf_phys + pmm_hhdm_offset());

    // HCI command packet format
    cmd[0] = opcode & 0xFF;
    cmd[1] = (opcode >> 8) & 0xFF;
    cmd[2] = param_len;
    for (uint8_t i = 0; i < param_len; i++) {
        cmd[3 + i] = params[i];
    }

    uint16_t total_len = 3 + param_len;

    // USB Bluetooth HCI command: Class request, Host-to-Device
    // bmRequestType = 0x20 (Class, Host-to-Device, Interface)
    // bRequest = 0x00
    // wValue = 0x0000
    // wIndex = 0x0000 (interface 0)
    m_xhci->do_control_transfer(m_slot_id, 0x20, 0x00, 0x0000, 0x0000, total_len,
                                reinterpret_cast<void*>(cmd_buf_phys));
                                
    pmm_free_page(cmd_buf_phys);
}

// ---- Prime event read (Interrupt IN) ----
void BluetoothHCI::prime_event_read() {
    m_xhci->submit_interrupt_in(m_slot_id, reinterpret_cast<void*>(m_evt_buf_phys), 256);
}

// ---- Prime ACL read (Bulk IN) ----
void BluetoothHCI::prime_acl_read() {
    m_xhci->submit_bulk_in(m_slot_id, reinterpret_cast<void*>(m_acl_in_buf_phys), 1024, false);
}

// ---- Wait for Command Complete event ----
bool BluetoothHCI::wait_command_complete(uint16_t expected_opcode, uint8_t* out_params, uint8_t max_params) {
    m_evt_buf_virt[0] = 0;
    prime_event_read();
    
    for (int timeout = 0; timeout < 5000000; timeout++) {
        m_xhci->poll_event_ring();
        
        uint8_t evt_code = m_evt_buf_virt[0];
        if (evt_code != 0) {
            bool matched = false;
            bool success = false;
            
            if (evt_code == HCI_EVT_COMMAND_COMPLETE) {
                uint16_t op = m_evt_buf_virt[3] | (m_evt_buf_virt[4] << 8);
                if (op == expected_opcode) {
                    uint8_t status = m_evt_buf_virt[5]; // Status byte
                    if (out_params && max_params > 0) {
                        uint8_t copy_len = m_evt_buf_virt[1] - 3; // Total params minus the 3 fixed bytes
                        if (copy_len > max_params) copy_len = max_params;
                        for (uint8_t i = 0; i < copy_len; i++) {
                            out_params[i] = m_evt_buf_virt[6 + i];
                        }
                    }
                    matched = true;
                    success = (status == 0x00);
                }
            } else if (evt_code == HCI_EVT_COMMAND_STATUS) {
                uint16_t op = m_evt_buf_virt[4] | (m_evt_buf_virt[5] << 8);
                if (op == expected_opcode) {
                    uint8_t status = m_evt_buf_virt[2];
                    matched = true;
                    success = (status == 0x00);
                }
            }
            
            if (matched) {
                if (!success && g_vga) {
                    g_vga->write("BT: Command 0x");
                    print_hex8(expected_opcode >> 8); print_hex8(expected_opcode & 0xFF);
                    g_vga->write(" failed with status 0x");
                    print_hex8(m_evt_buf_virt[evt_code == HCI_EVT_COMMAND_COMPLETE ? 5 : 2]);
                    g_vga->write("\n");
                }
                return success;
            }
            
            // Unrelated event (e.g. Number of Completed Packets)
            // Clear and prime again!
            m_evt_buf_virt[0] = 0;
            prime_event_read();
        }
        
        asm volatile("pause");
    }

    if (g_vga) g_vga->write("BT: Command timeout!\n");
    return false;
}

// ---- Init: Reset + Read BD_ADDR + Configure ----
bool BluetoothHCI::init() {
    if (g_vga) g_vga->write("BT: Initializing Bluetooth HCI...\n");

    // Configure the Interrupt IN endpoint for events
    m_xhci->configure_endpoint(m_slot_id, m_int_in_ep, 7, m_int_in_mps, 1);
    
    // Configure the Bulk endpoints for ACL data
    m_xhci->configure_bulk_endpoints(m_slot_id, m_bulk_out_ep, m_bulk_in_ep, m_bulk_mps);

    // Step 1: HCI Reset
    send_command(HCI_OP_RESET, nullptr, 0);
    if (!wait_command_complete(HCI_OP_RESET)) {
        if (g_vga) g_vga->write("BT: HCI Reset FAILED!\n");
        return false;
    }
    if (g_vga) g_vga->write("BT: HCI Reset OK\n");

    if (is_intel) {
        if (g_vga) g_vga->write("BT: Intel chip detected. Reading version...\n");
        uint8_t param = 0xFF;
        send_command(HCI_OP_INTEL_READ_VERSION, &param, 1);
        uint8_t intel_ver[255];
        for (int i = 0; i < 255; i++) intel_ver[i] = 0;
        
        if (wait_command_complete(HCI_OP_INTEL_READ_VERSION, intel_ver, 255)) {
            uint32_t cnvi_top = 0;
            uint32_t cnvr_top = 0;
            uint8_t img_type = 0;
            
            // Try parsing as TLV (Intel Gen 2+)
            int ptr = 0;
            bool is_tlv = false;
            while (ptr < 250) {
                uint8_t type = intel_ver[ptr];
                uint8_t len = intel_ver[ptr + 1];
                if (type == 0x00 && len == 0x00) break; // end of tlvs or empty
                if (type == 0x10 && len >= 4) { // CNVI_TOP
                    cnvi_top = intel_ver[ptr + 2] | (intel_ver[ptr + 3] << 8) | (intel_ver[ptr + 4] << 16) | (intel_ver[ptr + 5] << 24);
                    is_tlv = true;
                } else if (type == 0x11 && len >= 4) { // CNVR_TOP
                    cnvr_top = intel_ver[ptr + 2] | (intel_ver[ptr + 3] << 8) | (intel_ver[ptr + 4] << 16) | (intel_ver[ptr + 5] << 24);
                    is_tlv = true;
                } else if (type == 0x1c && len >= 1) { // IMAGE_TYPE
                    img_type = intel_ver[ptr + 2];
                    is_tlv = true;
                }
                if (len == 0) break; // prevent infinite loop
                ptr += 2 + len;
            }
            
            
            // img_type: 0x01 = bootloader (needs fw), 0x03 = operational (already loaded)
            bool needs_fw = false;
            if (is_tlv) {
                needs_fw = (img_type == 0x01);
                if (!needs_fw) {
                    if (g_vga) {
                        g_vga->write("BT: Intel device already operational (img_type=");
                        print_hex8(img_type);
                        g_vga->write("), skipping firmware download.\n");
                    }
                    m_intel_fw_loaded = true;
                }
            } else {
                // Legacy: always attempt firmware
                needs_fw = true;
            }
            
            if (needs_fw) {
                char path[64] = "/sata/";
                int idx = 6;
            
            if (is_tlv) {
                uint16_t c1_t = cnvi_top & 0xFFF;
                uint16_t c1_s = (cnvi_top >> 24) & 0xF;
                uint16_t c1_p = (c1_t << 4) | c1_s;
                uint16_t c1 = ((c1_p & 0xFF) << 8) | (c1_p >> 8);
                
                uint16_t c2_t = cnvr_top & 0xFFF;
                uint16_t c2_s = (cnvr_top >> 24) & 0xF;
                uint16_t c2_p = (c2_t << 4) | c2_s;
                uint16_t c2 = ((c2_p & 0xFF) << 8) | (c2_p >> 8);
                
                const char* hex = "0123456789ABCDEF"; // Uppercase for 8.3
                path[idx++] = hex[(c1 >> 12) & 0xF];
                path[idx++] = hex[(c1 >> 8) & 0xF];
                path[idx++] = hex[(c1 >> 4) & 0xF];
                path[idx++] = hex[c1 & 0xF];
                path[idx++] = hex[(c2 >> 12) & 0xF];
                path[idx++] = hex[(c2 >> 8) & 0xF];
                path[idx++] = hex[(c2 >> 4) & 0xF];
                path[idx++] = hex[c2 & 0xF];
            } else {
                // Legacy formatting (Intel Gen 1)
                uint8_t hw_variant = intel_ver[1];
                uint8_t fw_variant = intel_ver[3];
                
                if (hw_variant >= 100) { path[idx++] = '0' + (hw_variant / 100); path[idx++] = '0' + ((hw_variant / 10) % 10); path[idx++] = '0' + (hw_variant % 10); }
                else if (hw_variant >= 10) { path[idx++] = '0' + (hw_variant / 10); path[idx++] = '0' + (hw_variant % 10); }
                else { path[idx++] = '0' + hw_variant; }
                
                if (fw_variant >= 100) { path[idx++] = '0' + (fw_variant / 100); path[idx++] = '0' + ((fw_variant / 10) % 10); path[idx++] = '0' + (fw_variant % 10); }
                else if (fw_variant >= 10) { path[idx++] = '0' + (fw_variant / 10); path[idx++] = '0' + (fw_variant % 10); }
                else { path[idx++] = '0' + fw_variant; }
            }
            
            const char* ext = ".SFI";
            for(int i=0; i<5; i++) path[idx++] = ext[i]; // includes null terminator
                
                if (g_vga) { g_vga->write("BT: Loading "); g_vga->write(path); g_vga->write("...\n"); }
            
            int fd = vfs_open(path);
            if (fd >= 0) {
                int64_t fsize = vfs_filesize(fd);
                if (fsize > 0 && fsize < 1024*1024) { // max 1MB
                    uint8_t* fw_virt = new uint8_t[fsize];
                    
                    // Read with retry (AHCI may have transient errors)
                    int bytes_read = 0;
                    for (int attempt = 0; attempt < 3; attempt++) {
                        vfs_seek(fd, 0, 0); // SEEK_SET
                        bytes_read = vfs_read(fd, fw_virt, fsize);
                        if (bytes_read == (int)fsize) break;
                        if (g_vga) g_vga->write("BT: Partial read, retrying...\n");
                    }
                    
                    if (bytes_read != (int)fsize) {
                        if (g_vga) {
                            g_vga->write("BT: Failed to read firmware! Got ");
                            char nbuf[12]; int ni = 0;
                            int v = bytes_read;
                            if (v == 0) { nbuf[ni++] = '0'; }
                            else { char tmp[10]; int ti = 0; while(v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while(ti > 0) nbuf[ni++] = tmp[--ti]; }
                            nbuf[ni] = 0;
                            g_vga->write(nbuf);
                            g_vga->write(" bytes\n");
                        }
                    } else {
                    
                    if (g_vga) g_vga->write("BT: Sending firmware chunks...\n");
                    
                    if (is_tlv) {
                        auto send_fw_chunk = [&](uint8_t type, uint32_t off, uint32_t len) {
                            uint32_t sent = 0;
                            while (sent < len) {
                                uint32_t to_send = len - sent;
                                if (to_send > 252) to_send = 252;
                                uint8_t chunk[253];
                                chunk[0] = type;
                                for(uint32_t i=0; i<to_send; i++) chunk[1+i] = fw_virt[off + sent + i];
                                send_command(HCI_OP_INTEL_FW_DOWNLOAD, chunk, to_send + 1);
                                if (!wait_command_complete(HCI_OP_INTEL_FW_DOWNLOAD)) {
                                    if (g_vga) {
                                        g_vga->write("BT: FC09 fail type=");
                                        print_hex8(type);
                                        g_vga->write(" evt[0]=");
                                        print_hex8(m_evt_buf_virt[0]);
                                        g_vga->write(" evt[5]=");
                                        print_hex8(m_evt_buf_virt[5]);
                                        g_vga->write("\n");
                                    }
                                    return false;
                                }
                                sent += to_send;
                            }
                            return true;
                        };
                        
                        uint32_t css_header_ver = *(uint32_t*)(fw_virt + 8);
                        uint32_t offset = 0;
                        bool ok = true;
                        if (css_header_ver == 0x00010000) { // RSA
                            if (g_vga) g_vga->write("BT: Sending RSA CSS header...\n");
                            if (!send_fw_chunk(0x00, 0, 128)) { ok = false; }
                            if (ok) { if (g_vga) g_vga->write("BT: Sending RSA PKey...\n"); if (!send_fw_chunk(0x03, 128, 256)) ok = false; }
                            if (ok) { if (g_vga) g_vga->write("BT: Sending RSA Sign...\n"); if (!send_fw_chunk(0x02, 388, 256)) ok = false; }
                            offset = 644;
                            if (fsize > 644 && fw_virt[644] == 0x06) {
                                if (g_vga) g_vga->write("BT: ECDSA header detected, skipping 320 bytes\n");
                                offset = 964;
                            }
                        }
                        
                        if (ok) {
                            if (g_vga) g_vga->write("BT: Sending payload commands...\n");
                            uint32_t frag_len = 0;
                            uint32_t chunk_count = 0;
                            while (offset + frag_len < (uint32_t)fsize) {
                                uint32_t p = offset + frag_len;
                                if (p + 2 >= (uint32_t)fsize) break;
                                uint8_t plen = fw_virt[p + 2];
                                frag_len += 3 + plen;
                                
                                if (frag_len % 4 == 0) {
                                    if (!send_fw_chunk(0x01, offset, frag_len)) { ok = false; break; }
                                    offset += frag_len;
                                    frag_len = 0;
                                    chunk_count++;
                                }
                            }
                            if (g_vga) {
                                g_vga->write("BT: Sent ");
                                char nb[8]; int ni=0; uint32_t v=chunk_count;
                                if(v==0) nb[ni++]='0'; else { char t[8]; int ti=0; while(v>0){t[ti++]='0'+(v%10);v/=10;} while(ti>0) nb[ni++]=t[--ti]; }
                                nb[ni]=0; g_vga->write(nb);
                                g_vga->write(" payload chunks\n");
                            }
                        }
                        if (!ok && g_vga) g_vga->write("BT: TLV Chunk download failed!\n");
                    } else {
                        // Legacy Intel firmware download
                        uint32_t offset = 0;
                        while (offset < fsize) {
                            uint32_t left = fsize - offset;
                            uint8_t payload_len = (left > 252) ? 252 : left;
                            
                            uint8_t chunk[253];
                            if (offset == 0) chunk[0] = 0x00; // First
                            else if (offset + payload_len >= fsize) chunk[0] = 0x02; // Last
                            else chunk[0] = 0x01; // Middle
                            
                            for (uint8_t i = 0; i < payload_len; i++) chunk[1 + i] = fw_virt[offset + i];
                            
                            send_command(HCI_OP_INTEL_FW_DOWNLOAD, chunk, payload_len + 1);
                            if (!wait_command_complete(HCI_OP_INTEL_FW_DOWNLOAD)) {
                                if (g_vga) g_vga->write("BT: Chunk download failed!\n");
                                break;
                            }
                            offset += payload_len;
                        }
                    }
                    
                    if (g_vga) g_vga->write("BT: Firmware uploaded, resetting...\n");
                    uint8_t reset_params[] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x04, 0x00 };
                    send_command(HCI_OP_INTEL_RESET, reset_params, 8);
                    
                    // Intel reset doesn't return command complete, we should just wait 
                    // a bit for the device to reset internally.
                    for (int wait = 0; wait < 10000000; wait++) asm volatile("pause");
                    
                    m_intel_fw_loaded = true;
                    } // close else block
                    
                    delete[] fw_virt;
                }
                vfs_close(fd);
            }
        }
    }
    }

    // Step 2: Read BD_ADDR (our local Bluetooth address)
    send_command(HCI_OP_READ_BD_ADDR, nullptr, 0);
    uint8_t addr_params[6];
    if (!wait_command_complete(HCI_OP_READ_BD_ADDR, addr_params, 6)) {
        if (g_vga) g_vga->write("BT: Read BD_ADDR FAILED!\n");
        return false;
    }
    for (int i = 0; i < 6; i++) m_local_addr.b[i] = addr_params[i];
    
    if (g_vga) {
        g_vga->write("BT: Local Address: ");
        print_bdaddr(m_local_addr);
        g_vga->write("\n");
    }

    // Step 3: Enable Simple Secure Pairing (SSP) & Authentication
    uint8_t ssp_param = 0x01; // Enable
    send_command(HCI_OP_WRITE_SSP_MODE, &ssp_param, 1);
    wait_command_complete(HCI_OP_WRITE_SSP_MODE);

    uint8_t auth_enable = 0x01; // Enable Authentication
    send_command(HCI_OP_WRITE_AUTH_ENABLE, &auth_enable, 1);
    wait_command_complete(HCI_OP_WRITE_AUTH_ENABLE);

    // Step 3b: Write Class of Device (Desktop Computer = 0x000104)
    uint8_t cod[3] = { 0x04, 0x01, 0x00 };
    send_command(HCI_OP_WRITE_CLASS_OF_DEV, cod, 3);
    wait_command_complete(HCI_OP_WRITE_CLASS_OF_DEV);

    // Step 4: Set Inquiry Mode to RSSI (mode 1) so we get signal strength
    uint8_t inq_mode = 0x01; // Standard Inquiry Result with RSSI
    send_command(HCI_OP_WRITE_INQUIRY_MODE, &inq_mode, 1);
    wait_command_complete(HCI_OP_WRITE_INQUIRY_MODE);

    // Step 5: Write Scan Enable (Page Scan + Inquiry Scan)
    uint8_t scan_enable = 0x03; // Both inquiry and page scan
    send_command(HCI_OP_WRITE_SCAN_ENABLE, &scan_enable, 1);
    wait_command_complete(HCI_OP_WRITE_SCAN_ENABLE);

    if (g_vga) g_vga->write("BT: HCI initialized successfully!\n");
    
    // Prime the event ring so the main loop can start receiving events (like Inquiry Complete)
    m_evt_buf_virt[0] = 0;
    prime_event_read();
    
    g_bluetooth_hci = this;
    return true;
}

// ---- Start Inquiry (scan for nearby BT devices) ----
void BluetoothHCI::start_inquiry() {
    if (g_vga) g_vga->write("BT: Starting Inquiry... (searching for controllers)\n");

    m_device_count = 0;
    m_inquiry_complete = false;

    // Inquiry params: LAP (3 bytes) = 0x9E8B33 (GIAC), Inquiry Length, Num Responses
    uint8_t params[5];
    params[0] = 0x33; // LAP[0] (GIAC = General Inquiry Access Code)
    params[1] = 0x8B; // LAP[1]
    params[2] = 0x9E; // LAP[2]
    params[3] = 0x08; // Inquiry Length (8 * 1.28s = ~10 seconds)
    params[4] = 0xFF; // Num Responses (255)

    send_command(HCI_OP_INQUIRY, params, 5);
}

// ---- Cancel Inquiry ----
void BluetoothHCI::cancel_inquiry() {
    send_command(HCI_OP_INQUIRY_CANCEL, nullptr, 0);
}

// ---- Request Authentication ----
void BluetoothHCI::request_authentication() {
    if (m_auth_requested) return;
    m_auth_requested = true;
    m_authenticated = false;
    uint8_t params[2];
    params[0] = m_acl_handle & 0xFF;
    params[1] = (m_acl_handle >> 8) & 0xFF;
    send_command(HCI_OP_AUTH_REQUESTED, params, 2);
}

// ---- Set Connection Encryption ----
void BluetoothHCI::set_connection_encryption(bool enable) {
    if (m_acl_handle == 0) return;
    uint8_t params[3];
    params[0] = m_acl_handle & 0xFF;
    params[1] = (m_acl_handle >> 8) & 0xFF;
    params[2] = enable ? 0x01 : 0x00;
    send_command(HCI_OP_SET_CONN_ENCRYPT, params, 3);
}

// ---- Poll for HCI events and ACL data ----
bool BluetoothHCI::poll_events() {
    m_xhci->poll_event_ring();
    
    bool did_work = false;
    
    // Check HCI event buffer (Interrupt IN)
    uint8_t evt_code = m_evt_buf_virt[0];
    if (evt_code != 0) {
        handle_event(m_evt_buf_virt, m_int_in_mps);
        
        // Clear full buffer and re-prime
        for (uint16_t i = 0; i < 256; i++) m_evt_buf_virt[i] = 0;
        prime_event_read();
        did_work = true;
    }
    
    // Check ACL data buffer (Bulk IN)
    if (m_acl_handle != 0 && m_acl_in_ready) {
        m_acl_in_ready = false;
        // Forward to L2CAP
        if (g_bluetooth_l2cap) {
            // ACL packet length is in bytes 2-3 (after handle+flags)
            uint16_t acl_len = m_acl_in_buf_virt[2] | (m_acl_in_buf_virt[3] << 8);
            g_bluetooth_l2cap->handle_acl_data(m_acl_in_buf_virt, acl_len + 4);
        }
        
        // Clear and re-prime
        for (uint16_t i = 0; i < m_bulk_mps; i++) m_acl_in_buf_virt[i] = 0;
        prime_acl_read();
        did_work = true;
    }
    
    return did_work;
}

void BluetoothHCI::handle_event(const uint8_t* data, size_t length) {
    uint8_t evt_code = data[0];

    switch (evt_code) {
        case HCI_EVT_INQUIRY_COMPLETE:
            m_inquiry_complete = true;
            break;

        case HCI_EVT_INQUIRY_RESULT:
            handle_inquiry_result(data, length);
            break;
            
        case HCI_EVT_INQUIRY_RESULT_RSSI:
            handle_inquiry_result_rssi(data, length);
            break;
            
        case HCI_EVT_EXT_INQUIRY_RESULT:
            handle_ext_inquiry_result(data, length);
            break;

        case HCI_EVT_CONNECTION_REQUEST:
            handle_connection_request(data);
            break;

        case HCI_EVT_CONNECTION_COMPLETE:
            handle_connection_complete(data);
            break;

        case 0x05: { // Disconnection Complete
            uint16_t handle = (data[3] | (data[4] << 8)) & 0x0FFF;
            uint8_t reason = data[5];
            if (handle == m_acl_handle) {
                m_acl_handle = 0;
            }
            if (g_vga) {
                g_vga->write("BT: Disconnection Complete handle=0x");
                print_hex8(handle >> 8);
                print_hex8(handle & 0xFF);
                g_vga->write(" reason=0x");
                print_hex8(reason);
                g_vga->write("\n");
            }
            break;
        }

        case HCI_EVT_LINK_KEY_REQUEST:
            handle_link_key_request(data);
            break;

        case HCI_EVT_PIN_CODE_REQUEST:
            handle_pin_code_request(data);
            break;

        case HCI_EVT_IO_CAPABILITY_REQUEST:
            handle_io_capability_request(data);
            break;

        case HCI_EVT_IO_CAPABILITY_RESPONSE:
            break;

        case HCI_EVT_USER_CONFIRM_REQUEST:
            handle_user_confirm_request(data);
            break;

        case HCI_EVT_SSP_COMPLETE: {
            uint8_t ssp_status = data[2];
            if (ssp_status == 0x00) {
                m_authenticated = true;
                set_connection_encryption(true);
            }
            break;
        }

        case 0x12: // Role Change Event
            break;

        case 0x08: { // Encryption Change
            uint8_t enc_status = data[2];
            if (enc_status == 0x00) m_authenticated = true;
            break;
        }

        case 0x06: { // Auth Complete
            uint8_t auth_status = data[2];
            if (auth_status == 0x00) {
                m_authenticated = true;
                set_connection_encryption(true);
            }
            break;
        }

        case 0x18: // Link Key Notification
            break;

        case HCI_EVT_COMMAND_STATUS: {
            uint8_t status = data[2];
            uint16_t op = data[4] | (data[5] << 8);
            if (status != 0 && g_vga) {
                g_vga->write("BT: Command Status error for opcode ");
                print_hex8(op >> 8);
                print_hex8(op & 0xFF);
                g_vga->write(" (Status: 0x");
                print_hex8(status);
                g_vga->write(")\n");
            }
            break;
        }

        case HCI_EVT_COMMAND_COMPLETE: {
            // Command_Complete format: evt_code(1), param_len(1), num_hci_pkts(1), opcode(2), status(1), ...
            uint16_t cc_opcode = data[3] | (data[4] << 8);
            if (cc_opcode == HCI_OP_INQUIRY_CANCEL) {
                // After Inquiry_Cancel completes, mark inquiry as done
                m_inquiry_complete = true;
                if (g_vga) g_vga->write("BT: Inquiry canceled OK\n");
            }
            break;
        }

        case 0x13: // Number of Completed Packets (flow control)
        case 0x1B: // Max Slots Change
        case 0x20: // Page Scan Repetition Mode Change
        case 0xFF: // Vendor-specific event (Intel diagnostic)
            // All silently ignored
            break;

        default:
            if (g_vga) {
                g_vga->write("BT: Unknown event 0x");
                print_hex8(evt_code);
                g_vga->write("\n");
            }
            break;
    }
}

// ---- Handle Inquiry Result (standard) ----
void BluetoothHCI::handle_inquiry_result(const uint8_t* data, size_t) {
    uint8_t num_responses = data[2];
    const uint8_t* ptr = data + 3;

    for (uint8_t i = 0; i < num_responses && m_device_count < MAX_DEVICES; i++) {
        BTDevice& dev = m_devices[m_device_count];
        
        // BD_ADDR (6 bytes)
        for (int j = 0; j < 6; j++) dev.addr.b[j] = ptr[j];
        ptr += 6;
        
        dev.page_scan_rep_mode = ptr[0];
        ptr += 1 + 2; // Skip page_scan_period_mode + reserved
        
        dev.class_of_device = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
        ptr += 3 + 2; // Skip clock_offset
        
        dev.rssi = 0;
        dev.name[0] = 0;
        dev.valid = true;

        // Check if it's a gamepad (CoD: Major Class = 0x05 Peripheral, Minor includes gamepad)
        uint8_t major_class = (dev.class_of_device >> 8) & 0x1F;
        uint8_t minor_class = (dev.class_of_device >> 2) & 0x3F;
        dev.is_dualsense = (major_class == 0x05 && (minor_class & 0x08)); // Gamepad bit
        
        if (g_vga) {
            g_vga->write("BT: Found device ");
            print_bdaddr(dev.addr);
            g_vga->write(dev.is_dualsense ? " [GAMEPAD]\n" : "\n");
        }
        
        m_device_count++;
    }
}

// ---- Handle Inquiry Result with RSSI ----
void BluetoothHCI::handle_inquiry_result_rssi(const uint8_t* data, size_t) {
    uint8_t num_responses = data[2];
    const uint8_t* ptr = data + 3;

    for (uint8_t i = 0; i < num_responses && m_device_count < MAX_DEVICES; i++) {
        BTDevice& dev = m_devices[m_device_count];
        
        for (int j = 0; j < 6; j++) dev.addr.b[j] = ptr[j];
        ptr += 6;
        
        dev.page_scan_rep_mode = ptr[0];
        ptr += 1 + 1; // page_scan_rep_mode + reserved
        
        dev.class_of_device = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
        ptr += 3 + 2; // Skip clock_offset
        
        dev.rssi = (int8_t)ptr[0];
        ptr += 1;
        
        dev.name[0] = 0;
        dev.valid = true;
        
        uint8_t major_class = (dev.class_of_device >> 8) & 0x1F;
        uint8_t minor_class = (dev.class_of_device >> 2) & 0x3F;
        dev.is_dualsense = (major_class == 0x05 && minor_class == 0x02);
        
        if (g_vga) {
            g_vga->write("BT: Found device ");
            print_bdaddr(dev.addr);
            if (dev.is_dualsense) g_vga->write(" [GAMEPAD]");
            g_vga->write("\n");
        }
        
        m_device_count++;
    }
}

// ---- Handle Extended Inquiry Result ----
void BluetoothHCI::handle_ext_inquiry_result(const uint8_t* data, size_t) {
    // Extended Inquiry Result always has exactly 1 response
    if (m_device_count >= MAX_DEVICES) return;
    
    BTDevice& dev = m_devices[m_device_count];
    const uint8_t* ptr = data + 3; // Skip event_code, param_len, num_responses(=1)
    
    for (int j = 0; j < 6; j++) dev.addr.b[j] = ptr[j];
    ptr += 6;
    
    dev.page_scan_rep_mode = ptr[0];
    ptr += 1 + 1; // page_scan_rep_mode + reserved
    
    dev.class_of_device = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
    ptr += 3 + 2; // clock_offset
    
    dev.rssi = (int8_t)ptr[0];
    ptr += 1;
    
    // Extended Inquiry Response (EIR) data follows - 240 bytes
    // Parse for device name (type 0x09 = Complete Local Name or 0x08 = Shortened)
    dev.name[0] = 0;
    const uint8_t* eir = ptr;
    const uint8_t* eir_end = eir + 240;
    while (eir < eir_end && eir[0] != 0) {
        uint8_t field_len = eir[0];
        uint8_t field_type = eir[1];
        
        if ((field_type == 0x09 || field_type == 0x08) && field_len > 1) {
            uint8_t name_len = field_len - 1;
            if (name_len > 248) name_len = 248;
            for (uint8_t k = 0; k < name_len; k++) {
                dev.name[k] = (char)eir[2 + k];
            }
            dev.name[name_len] = 0;
            break;
        }
        eir += field_len + 1;
    }
    
    dev.valid = true;
    
    uint8_t major_class = (dev.class_of_device >> 8) & 0x1F;
    uint8_t minor_class = (dev.class_of_device >> 2) & 0x3F;
    dev.is_dualsense = (major_class == 0x05 && minor_class == 0x02);
    
    if (g_vga) {
        g_vga->write("BT: Found ");
        if (dev.name[0]) {
            g_vga->write("\"");
            g_vga->write(dev.name);
            g_vga->write("\" ");
        }
        print_bdaddr(dev.addr);
        if (dev.is_dualsense) g_vga->write(" [GAMEPAD]");
        g_vga->write("\n");
    }
    
    m_device_count++;
}

// ---- Connect to a device ----
void BluetoothHCI::connect_device(const BTDevice* dev) {
    if (!dev || !dev->valid) return;
    
    if (g_vga) {
        g_vga->write("BT: Connecting to ");
        print_bdaddr(dev->addr);
        g_vga->write("...\n");
    }

    uint8_t params[13];
    // BD_ADDR (6 bytes)
    for (int i = 0; i < 6; i++) params[i] = dev->addr.b[i];
    // Packet Type: DM1, DH1, DM3, DH3, DM5, DH5
    params[6] = 0x18; // CC18 = allow all packet types
    params[7] = 0xCC;
    // Page Scan Repetition Mode
    params[8] = dev->page_scan_rep_mode;
    // Reserved
    params[9] = 0x00;
    // Clock Offset (valid bit is bit 15)
    params[10] = 0x00;
    params[11] = 0x00;
    // Allow Role Switch
    params[12] = 0x01;

    send_command(HCI_OP_CREATE_CONNECTION, params, 13);
    // Connection Complete event will arrive asynchronously
}

// ---- Handle Connection Complete ----
void BluetoothHCI::handle_connection_complete(const uint8_t* data) {
    uint8_t status = data[2];
    uint16_t handle = data[3] | (data[4] << 8);
    handle &= 0x0FFF; // Only lower 12 bits

    if (status == 0x00) {
        m_acl_handle = handle;
        m_auth_requested = false;
        if (g_vga) {
            g_vga->write("BT: Connection established! Handle=0x");
            print_hex8(handle >> 8);
            print_hex8(handle & 0xFF);
            g_vga->write("\n");
        }
        // Start listening for ACL data
        prime_acl_read();
    } else {
        if (g_vga) {
            g_vga->write("BT: Connection FAILED, status=0x");
            print_hex8(status);
            g_vga->write("\n");
        }
    }
}

// ---- Handle Connection Request ----
void BluetoothHCI::handle_connection_request(const uint8_t* data) {
    BDAddr addr;
    for (int i = 0; i < 6; i++) addr.b[i] = data[2 + i];
    
    if (g_vga) {
        g_vga->write("BT: Incoming connection from ");
        print_bdaddr(addr);
        g_vga->write(" - Accepting\n");
    }

    // Accept connection request
    uint8_t params[7];
    for (int i = 0; i < 6; i++) params[i] = addr.b[i];
    params[6] = 0x00; // Role: Master
    send_command(HCI_OP_ACCEPT_CONN_REQ, params, 7);
}

// ---- Handle Link Key Request (respond with negative - we don't have one stored) ----
void BluetoothHCI::handle_link_key_request(const uint8_t* data) {
    BDAddr addr;
    for (int i = 0; i < 6; i++) addr.b[i] = data[2 + i];
    
    // Send Link Key Negative Reply (automatically triggers SSP/PIN pairing)
    send_command(HCI_OP_LINK_KEY_NEG_REPLY, addr.b, 6);
}

// ---- Handle PIN Code Request ----
void BluetoothHCI::handle_pin_code_request(const uint8_t* data) {
    BDAddr addr;
    for (int i = 0; i < 6; i++) addr.b[i] = data[2 + i];
    
    uint8_t params[23];
    for (int i = 0; i < 6; i++) params[i] = addr.b[i];
    params[6] = 4; // PIN length
    params[7] = '0';
    params[8] = '0';
    params[9] = '0';
    params[10] = '0';
    for (int i = 11; i < 23; i++) params[i] = 0;
    
    send_command(HCI_OP_PIN_CODE_REPLY, params, 23);
}

// ---- Handle IO Capability Request (SSP pairing) ----
void BluetoothHCI::handle_io_capability_request(const uint8_t* data) {
    BDAddr addr;
    for (int i = 0; i < 6; i++) addr.b[i] = data[2 + i];
    
    // IO_Capability_Reply: addr(6) + IO_Capability(1) + OOB_Data(1) + Auth_Requirements(1)
    uint8_t params[9];
    for (int i = 0; i < 6; i++) params[i] = addr.b[i];
    params[6] = 0x03; // NoInputNoOutput
    params[7] = 0x00; // No OOB data
    params[8] = 0x04; // MITM Not Required - General Bonding
    send_command(HCI_OP_IO_CAPABILITY_REPLY, params, 9);
}

// ---- Handle User Confirmation Request (auto-confirm for "Just Works" pairing) ----
void BluetoothHCI::handle_user_confirm_request(const uint8_t* data) {
    BDAddr addr;
    for (int i = 0; i < 6; i++) addr.b[i] = data[2 + i];
    
    // User_Confirmation_Reply
    send_command(HCI_OP_USER_CONFIRM_REPLY, addr.b, 6);
}
