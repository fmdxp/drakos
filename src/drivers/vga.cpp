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

// We create a static BitmapFont structure
static BitmapFont s_default_font;

bool VGA::start() {
    // Check if Limine provided a framebuffer
    if (!g_framebuffer_request.response || g_framebuffer_request.response->framebuffer_count < 1) {
        return false;
    }

    struct limine_framebuffer *fb = g_framebuffer_request.response->framebuffers[0];
    
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
    if (!selectBitmapFont(&s_default_font)) {
        return false; // Font failed to load/validate
    }

    // Clear the screen to black
    for (size_t y = 0; y < m_height; y++) {
        for (size_t x = 0; x < m_width; x++) {
            put_pixel(x, y, m_bg_color);
        }
    }

    // Draw the initial cursor
    draw_cursor();

    return true;
}

void VGA::stop() {
    // Usually nothing to do for VGA stop in a basic kernel
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

    // We are about to change cursor position, clear the old one
    clear_cursor();

    // Handle newline
    if (c == '\n') {
        m_cursor_x = 0;
        m_cursor_y += m_font->get_height();
        if (m_cursor_y + m_font->get_height() > m_height) scroll();
        draw_cursor();
        return;
    }

    // Handle backspace
    if (c == '\b') {
        if (m_cursor_x >= m_font->get_width()) {
            m_cursor_x -= m_font->get_width();
            // Erase character on screen
            for (uint32_t cy = 0; cy < m_font->get_height(); cy++) {
                for (uint32_t cx = 0; cx < m_font->get_width(); cx++) {
                    put_pixel(m_cursor_x + cx, m_cursor_y + cy, m_bg_color);
                }
            }
        }
        draw_cursor();
        return;
    }

    // Get the glyph data for this character
    const uint8_t* glyph = m_font->get_glyph(c);
    uint8_t glyph_width = m_font->get_width();
    uint8_t glyph_height = m_font->get_height();

    // Check if we need to wrap to the next line
    if (m_cursor_x + glyph_width >= m_width) {
        m_cursor_x = 0;
        m_cursor_y += glyph_height;
        if (m_cursor_y + glyph_height > m_height) scroll();
    }

    // TODO: Handle scrolling if m_cursor_y + glyph_height >= m_height

    // Draw the glyph pixel by pixel
    for (uint32_t cy = 0; cy < glyph_height; cy++) {
        // Each row of the glyph is 1 byte (8 pixels)
        uint8_t row = glyph[cy];
        
        for (uint32_t cx = 0; cx < glyph_width; cx++) {
            // Check the current bit. MSB (0x80) is the leftmost pixel.
            bool bit_set = (row & (0x80 >> cx)) != 0;
            
            uint32_t color = bit_set ? m_fg_color : m_bg_color;
            put_pixel(m_cursor_x + cx, m_cursor_y + cy, color);
        }
    }

    // Advance cursor
    m_cursor_x += glyph_width;
    
    // Check if wrapping is needed immediately after advancing
    if (m_cursor_x + glyph_width >= m_width) {
        m_cursor_x = 0;
        m_cursor_y += glyph_height;
        if (m_cursor_y + glyph_height > m_height) scroll();
    }

    draw_cursor();
}

void VGA::write(const char* str) {
    while (*str) {
        write_char(*str++);
    }
}

void VGA::scroll() {
    if (!m_font) return;
    
    uint32_t font_h = m_font->get_height();
    
    // Calculate the number of bytes to move
    // We move everything from the second row of text up to the top
    uint32_t bytes_to_move = m_pitch * (m_height - font_h);
    
    // Copy the memory up
    uint8_t* dest = reinterpret_cast<uint8_t*>(m_framebuffer);
    uint8_t* src = dest + (m_pitch * font_h);
    for (uint32_t i = 0; i < bytes_to_move; i++) {
        dest[i] = src[i];
    }
    
    // Clear the last line
    for (uint32_t y = m_height - font_h; y < m_height; y++) {
        for (uint32_t x = 0; x < m_width; x++) {
            put_pixel(x, y, m_bg_color);
        }
    }
    
    // Update cursor position
    m_cursor_y -= font_h;
}

void VGA::draw_cursor() {
    if (!m_font) return;
    
    // Draw a solid block using foreground color
    uint32_t w = m_font->get_width();
    uint32_t h = m_font->get_height();
    
    for (uint32_t cy = 0; cy < h; cy++) {
        for (uint32_t cx = 0; cx < w; cx++) {
            put_pixel(m_cursor_x + cx, m_cursor_y + cy, m_fg_color);
        }
    }
}

void VGA::clear_cursor() {
    if (!m_font) return;
    
    // Clear the block using background color
    uint32_t w = m_font->get_width();
    uint32_t h = m_font->get_height();
    
    for (uint32_t cy = 0; cy < h; cy++) {
        for (uint32_t cx = 0; cx < w; cx++) {
            put_pixel(m_cursor_x + cx, m_cursor_y + cy, m_bg_color);
        }
    }
}

// Register VGA driver as module (level 3_drv)
REGISTER_MODULE(g_vga, VGA, 3_drv);
