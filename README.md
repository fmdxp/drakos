# drakos

Welcome to **drakos**! 🎮

drakos is an open-source, bare-metal operating system built from scratch in C++. 

## 🎯 Our Vision

Modern desktop operating systems are incredibly heavy. They are bogged down by background processes, complex window managers, telemetry, and legacy cruft that gamers simply don't need.

**Our goal is to build an OS that turns any PC into a dedicated gaming console.**

We want an environment where:
- The OS gets out of the way.
- 100% of the hardware power is dedicated to running the game.
- You boot straight into a clean, controller-friendly interface.
- Native USB/Bluetooth gamepad support is built right into the core.
- **Even an old, dusty PC can be revived to feel like a brand-new, lightning-fast game console.**

Think of it as the soul of a traditional game console, but completely open, free, and designed for standard PC hardware.

## 🚀 The Journey So Far

We are building this from the absolute ground up! Right now, the foundation is coming together: we have our own memory managers, hardware interrupt handling, a basic text terminal, and we are currently scanning the PCI bus to start talking to real hardware. 

We are making the hard architectural choices now (like ditching legacy hardware) to ensure a blazing-fast gaming experience later.

## 🤝 We Need Your Help!

Right now, drakos is a passionate project driven by a very small core team (it's mostly just me right now!), and the road ahead is massive. 

If you love low-level programming, graphics engines, or just want to be part of building something crazy and ambitious, **I need your help!**

We are especially looking for contributors who can help with:
- **USB Stack Development (xHCI):** Getting Xbox, PlayStation, and generic Bluetooth controllers to talk directly to our kernel.
- **2D/3D Graphics & Compositing:** Pushing pixels to the screen as fast as possible without a bloated desktop environment.
- **Audio Drivers:** Because gaming without sound isn't gaming.
- **Testing & Ideas:** Running drakos on real hardware, finding bugs, and brainstorming the UI.

Whether you're a seasoned kernel hacker or a C++ developer looking for a fun challenge, drop into our discussions, open an issue, or submit a pull request! Let's build the ultimate gaming OS together.
