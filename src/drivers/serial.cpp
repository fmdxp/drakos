#include "serial.hpp"


inline uint8_t Serial::inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void Serial::outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}


bool Serial::is_transmit_ready() {
    // Bit 5 of the Line Status Register (LSR) indicates the transmit buffer is empty
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

bool Serial::start() {
    // 1. Disable all interrupts
    outb(COM1_PORT + 1, 0x00);

    // 2. Enable DLAB (Divisor Latch Access Bit) to set the baud rate divisor
    outb(COM1_PORT + 3, 0x80);

    // 3. Set divisor to 3 (low byte) = 38400 baud
    outb(COM1_PORT + 0, 0x03); // Low byte
    outb(COM1_PORT + 1, 0x00); // High byte

    // 4. Set 8N1 mode: 8 data bits, No parity, 1 stop bit. Also clears DLAB.
    outb(COM1_PORT + 3, 0x03);

    // 5. Enable FIFO, clear TX/RX queues, set 14-byte interrupt threshold
    outb(COM1_PORT + 2, 0xC7);

    // 6. Enable RTS and DTR (mark the port as ready)
    outb(COM1_PORT + 4, 0x03);

    // 7. Loopback test: send 0xAE and check if we receive the same byte back
    outb(COM1_PORT + 4, 0x1E); // Set loopback mode
    outb(COM1_PORT + 0, 0xAE); // Send test byte

    if (inb(COM1_PORT + 0) != 0xAE) {
        // Serial port is faulty or not present, detach this module
        return false;
    }

    // 8. Test passed. Disable loopback and switch to normal operation mode.
    outb(COM1_PORT + 4, 0x0F);

    write("COM1 Serial port initialized.\n");
    return true;
}

void Serial::stop() {
    // Disable all interrupts on the port
    outb(COM1_PORT + 1, 0x00);
}

const char* Serial::get_name() const {
    return "COM1 Serial Driver";
}

void Serial::write_char(char c) {
    // Wait until the transmit buffer is ready
    while (!is_transmit_ready());
    outb(COM1_PORT, (uint8_t)c);
}

void Serial::write(const char* str) {
    for (const char* p = str; *p != '\0'; p++) {
        write_char(*p);
    }
}

// Register as a Driver Module (level 3)
REGISTER_MODULE(g_serial, Serial, 3_drv);
