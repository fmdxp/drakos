use uefi::proto::console::gop::PixelFormat;

/// Information about the display framebuffer
#[derive(Debug)]
pub struct FramebufferInfo {
    /// Physical address of the framebuffer in memory
    pub physical_address: u64,
    /// Size of the framebuffer in bytes
    pub size: usize,
    /// Horizontal resolution in pixels
    pub width: usize,
    /// Vertical resolution in pixels
    pub height: usize,
    /// Number of pixels per row (including padding)
    pub stride: usize,
    /// Pixel color format
    pub pixel_format: PixelFormat,
}

/// The information structure passed from the bootloader to the kernel
#[derive(Debug)]
pub struct BootInfo {
    pub framebuffer: FramebufferInfo,
    /// Size of the UEFI memory map (for now we just store the size as requested)
    pub memory_map_size: usize,
    /// Version of the bootloader
    pub version: &'static str,
}
