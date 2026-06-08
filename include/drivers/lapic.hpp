#pragma once

#include <stdint.h>
#include "module.hpp"

// Local APIC Register Offsets
#define LAPIC_ID            0x0020
#define LAPIC_VERSION       0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_EOI           0x00B0
#define LAPIC_SVR           0x00F0
#define LAPIC_ESR           0x0280
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TMR       0x0320
#define LAPIC_LVT_PERF      0x0340
#define LAPIC_LVT_LINT0     0x0350
#define LAPIC_LVT_LINT1     0x0360
#define LAPIC_LVT_ERR       0x0370
#define LAPIC_TMRINITCNT    0x0380
#define LAPIC_TMRCURRCNT    0x0390
#define LAPIC_TMRDIV        0x03E0

class LocalAPIC : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Acknowledge an interrupt
    void eoi();

    // Read a Local APIC register
    uint32_t read(uint32_t reg);

    // Write a Local APIC register
    void write(uint32_t reg, uint32_t value);

private:
    uintptr_t m_lapic_virt = 0;
};

extern LocalAPIC* g_lapic;
