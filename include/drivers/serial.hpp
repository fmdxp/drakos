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
#include "module.hpp"

// COM1 base I/O port address
#define COM1_PORT 0x3F8

class Serial : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Write a single character to the serial port
    void write_char(char c);

    // Write a null-terminated string to the serial port
    void write(const char* str);

private:
    // Read a byte from the given I/O port
    static inline uint8_t inb(uint16_t port);

    // Write a byte to the given I/O port
    static inline void outb(uint16_t port, uint8_t value);

    // Returns true when the transmit buffer is empty and ready to send
    bool is_transmit_ready();
};

// Global pointer to the serial driver instance (set by REGISTER_MODULE)
extern Serial* g_serial;
