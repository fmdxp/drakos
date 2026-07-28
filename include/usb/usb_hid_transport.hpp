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
#include "hid_transport.hpp"

class XHCI;

class USBHIDTransport : public HIDTransport {
public:
    USBHIDTransport(uint32_t slot_id, uint8_t ep_num, uint16_t max_packet_size, XHCI* xhci);
    
    void start_listening(DualSenseDriver* driver) override;
    void send_output_report(uint8_t report_id, const uint8_t* data, size_t length) override;
    
    // Called by the XHCI ISR
    void on_interrupt();

private:
    uint32_t m_slot_id;
    uint8_t  m_ep_num;
    uint16_t m_max_packet_size;
    XHCI*    m_xhci;
    DualSenseDriver* m_driver;
    
    uintptr_t m_report_buf_phys;
    uint8_t*  m_report_buf_virt;

    void prime_interrupt();
};

extern USBHIDTransport* g_usb_hid_transport;
