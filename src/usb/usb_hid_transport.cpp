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

#include "usb_hid_transport.hpp"
#include "xhci.hpp"
#include "dualsense.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

USBHIDTransport* g_usb_hid_transport = nullptr;

USBHIDTransport::USBHIDTransport(uint32_t slot_id, uint8_t ep_num, uint16_t max_packet_size, XHCI* xhci)
    : m_slot_id(slot_id), m_ep_num(ep_num), m_max_packet_size(max_packet_size), m_xhci(xhci), m_driver(nullptr)
{
    m_report_buf_phys = pmm_alloc(1);
    m_report_buf_virt = reinterpret_cast<uint8_t*>(m_report_buf_phys + pmm_hhdm_offset());

    // Configure the Interrupt IN endpoint on xHCI
    xhci->configure_endpoint(m_slot_id, m_ep_num, 7, m_max_packet_size, 4);

    // Register as the active USB HID transport
    g_usb_hid_transport = this;
}

void USBHIDTransport::start_listening(DualSenseDriver* driver) {
    m_driver = driver;
    prime_interrupt();
}

void USBHIDTransport::prime_interrupt() {
    m_xhci->submit_interrupt_in(m_slot_id, reinterpret_cast<void*>(m_report_buf_phys), m_max_packet_size);
}

void USBHIDTransport::on_interrupt() {
    if (m_driver) {
        m_driver->on_report_received(m_report_buf_virt, m_max_packet_size);
    }
    prime_interrupt();
}

void USBHIDTransport::send_output_report(uint8_t report_id, const uint8_t* data, size_t length) {
    uintptr_t out_buf_phys = pmm_alloc(1);
    uint8_t* out = reinterpret_cast<uint8_t*>(out_buf_phys + pmm_hhdm_offset());
    for (size_t i = 0; i < length + 1; i++) out[i] = 0;
    
    out[0] = report_id;
    for (size_t i = 0; i < length; i++) {
        out[i + 1] = data[i];
    }
    
    // Find the HID interface number to send to (usually 3 for DualSense over USB)
    uint8_t hid_intf = 3;
    
    // Send SET_REPORT (Output). Type=0x21, Request=0x09, Value=0x0200 | report_id
    m_xhci->do_control_transfer(
        m_slot_id, 
        0x21, 
        0x09, 
        0x0200 | report_id, 
        hid_intf, 
        length + 1, 
        reinterpret_cast<void*>(out_buf_phys)
    );
}
