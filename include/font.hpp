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
    
    // Retrieves a pointer to the 8-bit wide array representing the given char
    const uint8_t* get_glyph(char c) const {
        // In a standard PSF font, the first 256 glyphs map to extended ASCII.
        // The index is just the unsigned char value.
        uint32_t index = static_cast<uint8_t>(c);
        
        // Return a pointer directly into the embedded glyph data
        return glyph_data + (index * height);
    }
    
    uint8_t get_width() const { return 8; }
    uint8_t get_height() const { return height; }
};