/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

class DualSenseDriver;

class HIDTransport {
public:
    virtual ~HIDTransport() = default;

    // Called by the driver to indicate it's ready to receive data
    virtual void start_listening(DualSenseDriver* driver) = 0;

    // Send an output report to the device (e.g. for rumble/LEDs)
    virtual void send_output_report(uint8_t report_id, const uint8_t* data, size_t length) = 0;
};
