# 01: The UEFI Bootloader and Graphics Output Protocol

When building an operating system from scratch, the first challenge is getting the hardware to run our code. Instead of dealing with legacy BIOS, **drakos** boots using UEFI (Unified Extensible Firmware Interface). 

This document explains the first pieces of code that run when you turn on a drakos console.

## The Entry Point

Because our OS is written in Rust without the standard library (`#![no_std]`), we don't have a normal `main` function. Instead, we use the `uefi` crate's `#[entry]` macro to define the entry point of our UEFI application.

```rust
#![no_std]
#![no_main]

use uefi::prelude::*;

#[entry]
fn main(image_handle: Handle, mut system_table: SystemTable<Boot>) -> Status {
    // ... bootloader logic goes here ...
    Status::SUCCESS
}
```

The `SystemTable` provides access to all UEFI services, including standard output, memory allocation, and hardware protocols.

## BootInfo: Preparing for the Kernel

The bootloader has a very specific job: set up the hardware environment, gather information about the system, and pass it to the kernel. We encapsulate this information in a struct called `BootInfo`.

```rust
pub struct BootInfo {
    pub framebuffer: FramebufferInfo,
    pub memory_map_size: usize,
    pub version: &'static str,
}
```

### 1. The Graphics Output Protocol (GOP)

To draw graphics (like game frames or a UI) without relying on the firmware, the kernel needs direct access to the video memory, known as the **Framebuffer**. 

In UEFI, we retrieve this using the `GraphicsOutput` protocol. Because some firmware implementations (like QEMU's OVMF) might lock the GOP handle for the text console, we must use `open_protocol` with the `GetProtocol` attribute, which requests non-exclusive access.

```rust
let gop_handles = uefi::boot::find_handles::<GraphicsOutput>().unwrap();
let gop_handle = *gop_handles.first().unwrap();

let mut gop = unsafe {
    uefi::boot::open_protocol::<GraphicsOutput>(
        OpenProtocolParams {
            handle: gop_handle,
            agent: uefi::boot::image_handle(),
            controller: None,
        },
        OpenProtocolAttributes::GetProtocol,
    ).unwrap()
};
```

From this `gop` instance, we can extract the `FramebufferInfo`, which tells the kernel exactly where in physical RAM the video memory lives (`physical_address`), its size, and how to format the pixels (e.g., Blue-Green-Red order).

### 2. The Memory Map

The kernel also needs to know what physical RAM is available for use, and what RAM is reserved by the hardware or firmware. We use the `uefi::boot::memory_map` function to retrieve this map:

```rust
let mmap = uefi::boot::memory_map(uefi::boot::MemoryType::LOADER_DATA).unwrap();
let memory_map_size = mmap.entries().count();
```

## Next Steps

Right now, the bootloader gathers this data and prints it to the screen. To transition into a fully functional OS, the bootloader will eventually need to:
1. Parse the ELF binary format of the drakos kernel.
2. Load the kernel into memory.
3. Call `exit_boot_services()` to permanently take over the hardware from the UEFI firmware.
4. Jump into the kernel's entry point, passing the `BootInfo` struct as an argument.
