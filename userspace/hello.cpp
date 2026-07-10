// Simple User Space Application
// We don't have a standard library or syscalls yet, so we just do something
// that proves we are executing. We'll loop, or purposefully cause a #GP.

extern "C" void _start() {
    // We are in Ring 3. If we execute HLT, we get a #GP!
    // We will do some math first to prove we can execute code.
    volatile int x = 0;
    for (int i = 0; i < 100; i++) {
        x += i;
    }

    // Now cause a #GP to prove we are in Ring 3
    asm volatile("hlt");
    
    while(1) {}
}
