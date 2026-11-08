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


#pragma once

#include <stdint.h>
#include <stddef.h>

// PSF1 format constants
#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04

// Represents a loaded bitmap font (PSF1 format)
struct BitmapFont {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t height; // in pixels (each row is 1 byte, so 8 pixels wide)
    
    // Pointer to the start of the glyph data (starts at byte 4 of the file)
    const uint8_t* glyph_data;
    
    bool valid;

    // Load the font from raw binary data
    void load(const uint8_t* raw_data) {
        magic[0] = raw_data[0];
        magic[1] = raw_data[1];
        mode     = raw_data[2];
        height   = raw_data[3];
        
        // Data starts immediately after the 4-byte header
        glyph_data = raw_data + 4;
        
        // Validate magic numbers
        valid = (magic[0] == PSF1_MAGIC0 && magic[1] == PSF1_MAGIC1);
    }
    
    // Finds a PSF1 Unicode-table entry, falling back to the font's 8-bit slot
    // for codepoints without a Unicode mapping.
    const uint8_t* get_glyph_codepoint(uint32_t codepoint) const {
        uint32_t index = static_cast<uint8_t>('?');

        if ((mode & 0x02) != 0) {
            const uint8_t* table = glyph_data + (256U * height);
            size_t offset = 0;
            bool in_sequence = false;

            for (uint32_t glyph_index = 0; glyph_index < 256; ++glyph_index) {
                in_sequence = false;
                for (;;) {
                    uint16_t entry = static_cast<uint16_t>(table[offset]) |
                                     (static_cast<uint16_t>(table[offset + 1]) << 8);
                    offset += 2;

                    if (entry == 0xFFFF) break;
                    if (entry == 0xFFFE) {
                        in_sequence = true;
                        continue;
                    }
                    if (!in_sequence && entry == codepoint) {
                        index = glyph_index;
                        return glyph_data + (index * height);
                    }
                }
            }
        }

        if (codepoint < 0x80 || ((mode & 0x02) == 0 && codepoint <= 0xFF)) {
            index = codepoint;
        }
        return glyph_data + (index * height);
    }

    // Legacy byte-oriented lookup retained for callers that pass one byte.
    const uint8_t* get_glyph(char c) const {
        return get_glyph_codepoint(static_cast<uint8_t>(c));
    }
    
    uint8_t get_width() const { return 8; }
    uint8_t get_height() const { return height; }
};
