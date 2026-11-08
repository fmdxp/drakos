#include "font.hpp"

#include <stdio.h>
#include <stdlib.h>

static bool check(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s font.psf\n", argv[0]);
        return 2;
    }

    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        perror("fopen");
        return 2;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    if (length < 4 + 256 * 16) {
        fprintf(stderr, "font file is too short\n");
        fclose(file);
        return 2;
    }

    uint8_t* bytes = static_cast<uint8_t*>(malloc(static_cast<size_t>(length)));
    if (!bytes || fread(bytes, 1, static_cast<size_t>(length), file) != static_cast<size_t>(length)) {
        fprintf(stderr, "failed to read font file\n");
        free(bytes);
        fclose(file);
        return 2;
    }
    fclose(file);

    BitmapFont font;
    font.load(bytes);
    bool ok = check(font.valid, "PSF1 font loads") &&
              check(font.get_glyph_codepoint('-') == font.glyph_data + (45 * font.height),
                    "ASCII hyphen uses glyph 45") &&
              check(font.get_glyph_codepoint(0x2013) == font.glyph_data + (45 * font.height),
                    "en dash maps through the PSF Unicode table") &&
              check(font.get_glyph_codepoint(0x2014) == font.glyph_data + (228 * font.height),
                    "em dash maps through the PSF Unicode table") &&
              check(font.get_glyph_codepoint(0x80) == font.glyph_data + (63 * font.height),
                    "unsupported 8-bit codepoint falls back to ASCII question mark") &&
              check(font.get_glyph_codepoint(0x10FFFF) == font.glyph_data + (63 * font.height),
                    "unsupported Unicode falls back to ASCII question mark");

    free(bytes);
    if (!ok) return 1;
    puts("PSF Unicode glyph tests passed.");
    return 0;
}
