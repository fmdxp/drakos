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

#include "drivers/bluetooth.hpp"
#include "drivers/bt_l2cap.hpp"

class DualSenseDriver;

enum class BTState {
    IDLE,
    INQUIRY,
    CANCELING_INQUIRY,
    CONNECTING,
    AUTHENTICATING,
    L2CAP_CONNECTING_CTRL,
    L2CAP_CONNECTING_INTR,
    CONNECTED
};

class BluetoothManager {
public:
    BluetoothManager(BluetoothHCI* hci, BluetoothL2CAP* l2cap);
    
    bool update(); // Called periodically to run the state machine

private:
    BluetoothHCI*   m_hci;
    BluetoothL2CAP* m_l2cap;
    BTState         m_state;
    uint32_t        m_timer;
    const BTDevice* m_target_dev;
    
    DualSenseDriver* m_dualsense;
    
    void handle_idle();
    void handle_inquiry();
    void handle_canceling_inquiry();
    void handle_connecting();
    void handle_authenticating();
    void handle_l2cap_connecting_ctrl();
    void handle_l2cap_connecting_intr();
    void handle_connected();
    
    static void on_hid_data_received(const uint8_t* data, size_t length);
};

extern BluetoothManager* g_bluetooth_manager;
