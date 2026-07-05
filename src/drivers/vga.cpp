/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "vga.hpp"
#include "font.hpp"
#include "serial.hpp"
#include "limine_requests.hpp"
#include "memory.hpp"

// We create a static BitmapFont structure
static BitmapFont s_default_font;

static volatile bool fb_lock = false;

static inline void lock_fb() {
    while (__atomic_test_and_set(&fb_lock, __ATOMIC_ACQUIRE)) {}
}

static inline void unlock_fb() {
    __atomic_clear(&fb_lock, __ATOMIC_RELEASE);
}

bool VGA::start() {
    // Check if Limine provided a framebuffer
    if (!g_framebuffer_request.response || g_framebuffer_request.response->framebuffer_count < 1) return false;
    

    auto* fb = g_framebuffer_request.response->framebuffers[0];
    
    // Save framebuffer details
    m_framebuffer = reinterpret_cast<uint32_t*>(fb->address);
    m_width       = fb->width;
    m_height      = fb->height;
    m_pitch       = fb->pitch;

    // Reset cursor
    m_cursor_x = 0;
    m_cursor_y = 0;

    // Default colors: White text on Black background
    m_fg_color = 0xFFFFFFFF;
    m_bg_color = 0x00000000;

    // Load our default Terminus font
    s_default_font.load(_binary_src_fonts_Lat2_Terminus16_psf_start);
    if (!selectBitmapFont(&s_default_font)) return false; // Font failed to load/validate
    
    clear_screen();

    return true;
}

void VGA::stop() {
    // nothing for now
}

const char* VGA::get_name() const {
    return "VGA Framebuffer Console";
}

bool VGA::selectBitmapFont(const BitmapFont* font) {
    if (!font || !font->valid) {
        return false;
    }
    m_font = font;
    return true;
}

void VGA::put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    // Safety check
    if (x >= m_width || y >= m_height) return;

    // pitch is in BYTES, but m_framebuffer is a uint32_t pointer (4 bytes per element).
    // So to find the right row, we cast m_framebuffer to uint8_t* first, add the byte offset (y * pitch),
    // and then cast back to uint32_t* to write the pixel.
    uint8_t* row = reinterpret_cast<uint8_t*>(m_framebuffer) + (y * m_pitch);
    uint32_t* pixel = reinterpret_cast<uint32_t*>(row) + x;
    
    *pixel = color;
}

void VGA::write_char(char c) {
    if (!m_font) return;

    lock_fb();

    const uint32_t w = m_font->get_width();
    const uint32_t h = m_font->get_height();

    // Handle newline
    if (c == '\n') {
        m_cursor_x = 0;
        m_cursor_y += h;
        if (m_cursor_y + h > m_height) scroll();
        unlock_fb();
        return;
    }

    // Handle backspace
    if (c == '\b') {
        if (m_cursor_x >= w) {
            m_cursor_x -= w;

            // Erase character on screen
            for (uint32_t y = 0; y < m_font->get_height(); y++) 
                for (uint32_t x = 0; x < m_font->get_width(); x++) 
                    put_pixel(m_cursor_x + x, m_cursor_y + y, m_bg_color);  
        }

        unlock_fb();
        return;
    }

    // Get the glyph data for this character
    const uint8_t* glyph = m_font->get_glyph(c);

    // Check if we need to wrap to the next line
    if (m_cursor_x + w >= m_width) {
        m_cursor_x = 0;
        m_cursor_y += h;
        if (m_cursor_y + h > m_height) scroll();
    }


    // Draw the glyph pixel by pixel
    for (uint32_t y = 0; y < h; y++) {
        // Each row of the glyph is 1 byte (8 pixels)
        uint8_t row = glyph[y];
        
        for (uint32_t x = 0; x < w; x++) {
            // Check the current bit. MSB (0x80) is the leftmost pixel.
            bool bit = row & (0x80 >> x);
            put_pixel(m_cursor_x + x, m_cursor_y + y, bit ? m_fg_color : m_bg_color);
        }
    }

    // Advance cursor
    m_cursor_x += w;
    unlock_fb();
}

void VGA::write(const char* str) {
    while (*str) write_char(*str++);
}

void VGA::scroll() {
    if (!m_font) return;
    
    uint32_t h = m_font->get_height();
    
    uint8_t* base = reinterpret_cast<uint8_t*>(m_framebuffer);

    // Calculate the number of bytes to move
    // We move everything from the second row of text up to the top
    uint32_t bytes = m_pitch * (m_height - h);
    
    kmemmove(base, base + (m_pitch * h), bytes);
    
    // Clear the last line
    for (uint32_t y = m_height - h; y < m_height; y++) {
        for (uint32_t x = 0; x < m_width; x++) {
            put_pixel(x, y, m_bg_color);
        }
    }
    
    if (m_cursor_y >= h) m_cursor_y -= h;
}


void VGA::draw_cursor() {
    // We don't need it for now, so I removed it (created bug)
}

void VGA::clear_cursor() {
    // ...
}



void VGA::clear_screen() {
    lock_fb();

    for (uint32_t y = 0; y < m_height; y++) {
        for (uint32_t x = 0; x < m_width; x++) {
            put_pixel(x, y, m_bg_color);
        }
    }

    unlock_fb();
}

// Register VGA driver as module (level 3_drv)
REGISTER_MODULE(g_vga, VGA, 3_drv);
