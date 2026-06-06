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
