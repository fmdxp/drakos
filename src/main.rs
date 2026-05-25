#![no_std]
#![no_main]

mod boot_info;

use boot_info::{BootInfo, FramebufferInfo};
use log::info;
use uefi::mem::memory_map::MemoryMap;
use uefi::boot::{OpenProtocolAttributes, OpenProtocolParams};
use uefi::prelude::*;
use uefi::proto::console::gop::GraphicsOutput;

#[entry]
fn main(_image_handle: Handle, mut system_table: SystemTable<Boot>) -> Status {
    // Initialize logging services and the panic handler provided by the uefi crate
    uefi::helpers::init().unwrap();

    // Clear the screen
    system_table.stdout().clear().unwrap();

    info!("Booting drakos...");

    let boot_services = system_table.boot_services();

    info!("Locating Graphics Output Protocol...");
    let gop_handles = match uefi::boot::find_handles::<GraphicsOutput>() {
        Ok(handles) => handles,
        Err(e) => {
            info!("Failed to find GOP handles: {:?}", e);
            boot_services.stall(10_000_000);
            return Status::ABORTED;
        }
    };

    let gop_handle = *gop_handles.first().unwrap();
    // Use GetProtocol (non-exclusive) because OVMF's console driver already holds the GOP handle.
    let mut gop = unsafe {
        match uefi::boot::open_protocol::<GraphicsOutput>(
            OpenProtocolParams {
                handle: gop_handle,
                agent: uefi::boot::image_handle(),
                controller: None,
            },
            OpenProtocolAttributes::GetProtocol,
        ) {
            Ok(gop) => gop,
            Err(e) => {
                info!("Failed to open GOP protocol: {:?}", e);
                boot_services.stall(10_000_000);
                return Status::ABORTED;
            }
        }
    };

    let mode_info = gop.current_mode_info();
    let mut fb = gop.frame_buffer();
    let fb_info = FramebufferInfo {
        physical_address: fb.as_mut_ptr() as u64,
        size: fb.size(),
        width: mode_info.resolution().0,
        height: mode_info.resolution().1,
        stride: mode_info.stride(),
        pixel_format: mode_info.pixel_format(),
    };

    let mmap = uefi::boot::memory_map(uefi::boot::MemoryType::LOADER_DATA).unwrap();
    let memory_map_size = mmap.entries().count();

    let boot_info = BootInfo {
        framebuffer: fb_info,
        memory_map_size,
        version: "0.1.0",
    };

    info!("BootInfo successfully gathered!");
    info!("Framebuffer Address: {:#x}", boot_info.framebuffer.physical_address);
    info!("Framebuffer Size: {} bytes", boot_info.framebuffer.size);
    info!("Resolution: {}x{}", boot_info.framebuffer.width, boot_info.framebuffer.height);
    info!("Stride: {}", boot_info.framebuffer.stride);
    info!("Pixel Format: {:?}", boot_info.framebuffer.pixel_format);
    info!("Memory Map Entries: {}", boot_info.memory_map_size);
    info!("Version: {}", boot_info.version);
    info!("Holding before exit_boot_services to let you read the info.");

    // Wait for 15 seconds to allow the user to read the message
    boot_services.stall(15_000_000);

    Status::SUCCESS
}