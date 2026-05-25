# drakos

**drakos** is a high-performance, bare-metal operating system written from scratch in Rust, specifically engineered for gaming consoles. 

It is designed with a single uncompromising goal: to run games as fast and efficiently as possible, eliminating the bloat and overhead found in traditional desktop operating systems. While it does not aim to replace general-purpose OSs like Linux or Windows for daily computing, it strives to be the ultimate platform for dedicated gaming machines.

## Key Features
- **Pure Gaming Focus:** Zero background bloat. Every CPU cycle and byte of RAM is prioritized for the active game.
- **Advanced Security:** Implementation of hardware key encryption to prevent system tampering and protect game assets from unauthorized extraction.
- **High-End Graphics & Peripherals:** First-class support for controllers and cutting-edge graphics processing.
- **Real-Time Capabilities:** Built-in support for real-time processing, networking, and precision timing (UTC).
- **Proton Compatibility (Planned):** Investigating a compatibility layer to support a wide range of existing games out of the box.

## Architecture
The architectural design of drakos is currently evolving. We are heavily exploring the trade-offs between a monolithic kernel versus a microkernel approach. Similarly, the system API is still being defined—balancing the flexibility of a completely custom API against the compatibility benefits of POSIX (which greatly simplifies porting games and layers like Proton). 

Currently, the system boots directly from UEFI using a custom bootloader written in Rust.

## Target Audience
drakos is built for gamers who demand the absolute best performance from their hardware, and for developers who want to push the boundaries of what a dedicated gaming OS can achieve.

## Documentation
The development of drakos is heavily documented. We aim to explain every single piece of the system as it is built. 

Please refer to the `docs/` directory for detailed, step-by-step explanations of the internal workings of drakos:
- [01: The UEFI Bootloader and Graphics Output Protocol](docs/01_bootloader.md)

## Building and Running
To compile and test drakos locally in QEMU, run the included PowerShell script:
```powershell
.\run.ps1
```
*(Note: You will need Rust, Cargo, and QEMU installed on your host machine).*

*(Note: Yes, I'm a lazy bum and I made gemini write the docs and the readme, so "We" is intended as "I" for now...).*
