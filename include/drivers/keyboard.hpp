#pragma once

#include "module.hpp"

class Keyboard : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;
};

extern Keyboard* g_keyboard;
