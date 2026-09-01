# drakos

Welcome to **drakos**! 🎮

drakos is an open-source, Linux-based operating system focused on turning standard PCs into a console-like gaming environment! 

> [!WARNING]
> **License Notice**
>
> drakos is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
> You may use, modify, and redistribute the project under the terms of the license.
> See the [`LICENSE`](../blob/main/LICENSE) file for the full license text.
>
> Contributions to drakos are also expected to be compatible with the project's GPL-3.0 license.


## 🎯 Our Vision

drakos aims to turn a standard PC into a **dedicated gaming machine**.

Instead of providing a traditional desktop environment, drakos focuses on a simple, controller-friendly experience designed around launching and playing games.

The goal is to:

* minimize unnecessary background services and overhead
* provide a fast boot-to-game experience
* support standard PC hardware
* provide a controller-first interface
* keep the system lightweight and customizable
* make the system feel more like a gaming console than a traditional desktop PC

drakos is **not intended to replace general-purpose desktop Linux distributions**. It is designed around one thing:

> **Turn on the PC → pick a game → play. 🎮**


## 🏗️ Architecture

drakos is built using established open-source technologies rather than implementing every operating-system component from scratch.
Buildroot is used to build the complete system image, including the Linux kernel, base userspace, filesystem and required packages.

This lets us focus our development effort on the parts that actually make drakos different.



## 🚀 The Journey So Far

drakos originally started as a bare-metal OS project.

The first implementation attempted to provide its own kernel and hardware support from scratch. While this was a great learning experience, it also meant that even basic functionality required implementing enormous amounts of low-level infrastructure.

The project is now moving toward a Linux-based architecture.

This allows us to keep the ambitious goal of creating a gaming-focused operating system while benefiting from Linux's existing hardware support, drivers and ecosystem.

Development is still in its early stages.



## ⚒️ Building

> [!IMPORTANT]
> ℹ️ This section is still under work, development has barely just started.

The system will be built using Buildroot.

The general build process will eventually look something like:

```bash
git clone drakos
cd drakos
```

```bash
make menuconfig
make
```

The resulting image will be usable in a virtual machine and, eventually, on real x86_64 hardware.

Detailed build and development instructions will be added as the project architecture stabilizes.


## 🤝 Contribution

Right now, drakos is a passionate project driven by a very small core team (just 2 now, and we don't know rust at all), and the road ahead is massive. 

If you love low-level programming, graphics engines, or just want to be part of building something crazy and ambitious, **We need your help!**

We are especially looking for contributors who can help with:
* 🎮 Gaming-focused UI and UX
* 🖥️ Graphics and display support
* 🎧 Audio
* 🎮 Controller support
* ⚙️ Linux/Buildroot integration
* 🚀 Boot and startup optimization
* 📦 System configuration and packaging
* 🧪 Testing on real hardware
* 💡 Ideas for the overall system design

Whether you're a seasoned kernel hacker or a developer looking for a fun challenge, drop into our discussions, open an issue, or submit a pull request! 
### **Let's build the ultimate gaming OS together.**
