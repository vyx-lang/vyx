#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "hb.h"
#include "hb-ft.h"

#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

typedef struct ZynTextFont {
    FT_Library library;
    FT_Face face;
    hb_face_t* hb_face;
    hb_font_t* hb_font;
    int pixel_size;
} ZynTextFont;

typedef struct ZynTextFontInfo {
    int ascender;
    int descender;
    int height;
    int max_advance;
    int glyph_count;
} ZynTextFontInfo;

typedef struct ZynGlyphBitmap {
    int width;
    int height;
    int bearing_x;
    int bearing_y;
    int advance_x;
    int advance_y;
    int pitch;
    void* pixels;
} ZynGlyphBitmap;

typedef struct ZynShapedGlyph {
    uint32_t glyph_index;
    uint32_t cluster;
    int advance_x;
    int advance_y;
    int offset_x;
    int offset_y;
} ZynShapedGlyph;

typedef struct ZynBidiRun {
    int first_grapheme;
    int length;
    int direction;
    int visual_index;
} ZynBidiRun;

int zyn_text_icu_major_version(void) {
    UVersionInfo version;
    u_getVersion(version);
    return (int)version[0];
}

static int zyn_text_utf8_codepoint_len(const char* text, int len, int pos, uint32_t* out_cp) {
    if (!text || pos < 0 || pos >= len) {
        return 0;
    }
    const unsigned char c0 = (unsigned char)text[pos];
    if (c0 < 0x80) {
        if (out_cp) {
            *out_cp = (uint32_t)c0;
        }
        return 1;
    }
    if ((c0 & 0xe0) == 0xc0 && pos + 1 < len) {
        if (out_cp) {
            *out_cp = (((uint32_t)c0 & 0x1f) << 6) | ((uint32_t)text[pos + 1] & 0x3f);
        }
        return 2;
    }
    if ((c0 & 0xf0) == 0xe0 && pos + 2 < len) {
        if (out_cp) {
            *out_cp = (((uint32_t)c0 & 0x0f) << 12)
                | (((uint32_t)text[pos + 1] & 0x3f) << 6)
                | ((uint32_t)text[pos + 2] & 0x3f);
        }
        return 3;
    }
    if ((c0 & 0xf8) == 0xf0 && pos + 3 < len) {
        if (out_cp) {
            *out_cp = (((uint32_t)c0 & 0x07) << 18)
                | (((uint32_t)text[pos + 1] & 0x3f) << 12)
                | (((uint32_t)text[pos + 2] & 0x3f) << 6)
                | ((uint32_t)text[pos + 3] & 0x3f);
        }
        return 4;
    }
    if (out_cp) {
        *out_cp = 0xfffd;
    }
    return 1;
}

static int zyn_text_utf8_to_u16_index(const char* text, int len, int64_t byte_offset) {
    if (!text || len <= 0 || byte_offset <= 0) {
        return 0;
    }
    if (byte_offset > (int64_t)len) {
        byte_offset = (int64_t)len;
    }
    int pos = 0;
    int u16_index = 0;
    while (pos < len && (int64_t)pos < byte_offset) {
        uint32_t cp = 0;
        const int advance = zyn_text_utf8_codepoint_len(text, len, pos, &cp);
        if (advance <= 0 || (int64_t)(pos + advance) > byte_offset) {
            break;
        }
        u16_index += cp > 0xffff ? 2 : 1;
        pos += advance;
    }
    return u16_index;
}

static int64_t zyn_text_u16_to_utf8_byte_index(const char* text, int len, int u16_index) {
    if (!text || len <= 0 || u16_index <= 0) {
        return 0;
    }
    int pos = 0;
    int cur_u16 = 0;
    while (pos < len && cur_u16 < u16_index) {
        uint32_t cp = 0;
        const int advance = zyn_text_utf8_codepoint_len(text, len, pos, &cp);
        if (advance <= 0) {
            break;
        }
        const int next_u16 = cur_u16 + (cp > 0xffff ? 2 : 1);
        if (next_u16 > u16_index) {
            break;
        }
        cur_u16 = next_u16;
        pos += advance;
    }
    if (pos > len) {
        return (int64_t)len;
    }
    return (int64_t)pos;
}

static int zyn_text_direction_for_range(const char* text, int len, int64_t start, int64_t end) {
    int pos = (int)start;
    while (pos < len && (int64_t)pos < end) {
        uint32_t cp = 0;
        const int advance = zyn_text_utf8_codepoint_len(text, len, pos, &cp);
        if (advance <= 0) {
            break;
        }
        const UCharDirection dir = u_charDirection((UChar32)cp);
        if (dir == U_RIGHT_TO_LEFT || dir == U_RIGHT_TO_LEFT_ARABIC
            || dir == U_RIGHT_TO_LEFT_EMBEDDING || dir == U_RIGHT_TO_LEFT_OVERRIDE
            || dir == U_ARABIC_NUMBER) {
            return -1;
        }
        if (dir == U_LEFT_TO_RIGHT || dir == U_LEFT_TO_RIGHT_EMBEDDING
            || dir == U_LEFT_TO_RIGHT_OVERRIDE || dir == U_EUROPEAN_NUMBER) {
            return 1;
        }
        pos += advance;
    }
    return 1;
}

static int zyn_text_range_metrics(const char* text,
                                  int len,
                                  int64_t start,
                                  int64_t end,
                                  int* out_codepoints,
                                  int* out_has_extension,
                                  int* out_has_joiner,
                                  int* out_has_emoji,
                                  int* out_has_rtl) {
    int pos = (int)start;
    int codepoints = 0;
    int has_extension = 0;
    int has_joiner = 0;
    int has_emoji = 0;
    int has_rtl = 0;
    while (pos < len && (int64_t)pos < end) {
        uint32_t cp = 0;
        const int advance = zyn_text_utf8_codepoint_len(text, len, pos, &cp);
        if (advance <= 0) {
            break;
        }
        codepoints += 1;
        if (u_hasBinaryProperty((UChar32)cp, UCHAR_GRAPHEME_EXTEND)
            || u_hasBinaryProperty((UChar32)cp, UCHAR_VARIATION_SELECTOR)
            || u_hasBinaryProperty((UChar32)cp, UCHAR_EMOJI_MODIFIER)) {
            has_extension = 1;
        }
        if (cp == 0x200c || cp == 0x200d) {
            has_joiner = 1;
        }
        if (u_hasBinaryProperty((UChar32)cp, UCHAR_EMOJI)
            || u_hasBinaryProperty((UChar32)cp, UCHAR_EMOJI_PRESENTATION)) {
            has_emoji = 1;
        }
        const UCharDirection dir = u_charDirection((UChar32)cp);
        if (dir == U_RIGHT_TO_LEFT || dir == U_RIGHT_TO_LEFT_ARABIC
            || dir == U_RIGHT_TO_LEFT_EMBEDDING || dir == U_RIGHT_TO_LEFT_OVERRIDE
            || dir == U_ARABIC_NUMBER) {
            has_rtl = 1;
        }
        pos += advance;
    }
    if (out_codepoints) {
        *out_codepoints = codepoints;
    }
    if (out_has_extension) {
        *out_has_extension = has_extension;
    }
    if (out_has_joiner) {
        *out_has_joiner = has_joiner;
    }
    if (out_has_emoji) {
        *out_has_emoji = has_emoji;
    }
    if (out_has_rtl) {
        *out_has_rtl = has_rtl;
    }
    return codepoints;
}

int zyn_text_boundary_analyze_utf8(const char* text,
                                   int len,
                                   int64_t* byte_boundaries,
                                   int* grapheme_codepoint_counts,
                                   int* grapheme_directions,
                                   int max_boundaries,
                                   int max_graphemes,
                                   int* out_boundary_count,
                                   int* out_codepoint_count,
                                   int* out_combining_sequence_count,
                                   int* out_joiner_sequence_count,
                                   int* out_emoji_sequence_count,
                                   int* out_rtl_grapheme_count,
                                   int* out_ltr_grapheme_count,
                                   int* out_rtl_run_count,
                                   int* out_ltr_run_count,
                                   int* out_direction_run_count,
                                   int* out_max_grapheme_codepoints,
                                   int64_t* out_max_grapheme_bytes) {
    if (out_boundary_count) { *out_boundary_count = 0; }
    if (out_codepoint_count) { *out_codepoint_count = 0; }
    if (out_combining_sequence_count) { *out_combining_sequence_count = 0; }
    if (out_joiner_sequence_count) { *out_joiner_sequence_count = 0; }
    if (out_emoji_sequence_count) { *out_emoji_sequence_count = 0; }
    if (out_rtl_grapheme_count) { *out_rtl_grapheme_count = 0; }
    if (out_ltr_grapheme_count) { *out_ltr_grapheme_count = 0; }
    if (out_rtl_run_count) { *out_rtl_run_count = 0; }
    if (out_ltr_run_count) { *out_ltr_run_count = 0; }
    if (out_direction_run_count) { *out_direction_run_count = 0; }
    if (out_max_grapheme_codepoints) { *out_max_grapheme_codepoints = 0; }
    if (out_max_grapheme_bytes) { *out_max_grapheme_bytes = 0; }
    if (!text || len < 0 || !byte_boundaries || !grapheme_codepoint_counts || !grapheme_directions
        || max_boundaries <= 0 || max_graphemes < 0 || !out_boundary_count || !out_codepoint_count
        || !out_combining_sequence_count || !out_joiner_sequence_count || !out_emoji_sequence_count
        || !out_rtl_grapheme_count || !out_ltr_grapheme_count || !out_rtl_run_count
        || !out_ltr_run_count || !out_direction_run_count || !out_max_grapheme_codepoints
        || !out_max_grapheme_bytes) {
        return -20;
    }
    if (max_boundaries < 1) {
        return -21;
    }
    byte_boundaries[0] = 0;
    *out_boundary_count = 1;
    if (len == 0) {
        return 0;
    }

    UErrorCode status = U_ZERO_ERROR;
    int32_t u16_len = 0;
    u_strFromUTF8(NULL, 0, &u16_len, text, len, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        return -22;
    }
    status = U_ZERO_ERROR;
    UChar* u16 = (UChar*)calloc((size_t)u16_len + 1, sizeof(UChar));
    if (!u16) {
        return -23;
    }
    u_strFromUTF8(u16, u16_len + 1, NULL, text, len, &status);
    if (U_FAILURE(status)) {
        free(u16);
        return -24;
    }

    status = U_ZERO_ERROR;
    UBreakIterator* iter = ubrk_open(UBRK_CHARACTER, "", u16, u16_len, &status);
    if (U_FAILURE(status) || !iter) {
        free(u16);
        return -25;
    }

    int boundary_count = 1;
    int grapheme_count = 0;
    int32_t previous_u16 = ubrk_first(iter);
    int64_t previous_byte = 0;
    int last_direction = 0;
    while (previous_u16 != UBRK_DONE) {
        const int32_t next_u16 = ubrk_next(iter);
        if (next_u16 == UBRK_DONE) {
            break;
        }
        if (boundary_count >= max_boundaries || grapheme_count >= max_graphemes) {
            ubrk_close(iter);
            free(u16);
            return -26;
        }
        int64_t next_byte = zyn_text_u16_to_utf8_byte_index(text, len, next_u16);
        if (next_u16 >= u16_len) {
            next_byte = (int64_t)len;
        }
        if (next_byte <= previous_byte) {
            previous_u16 = next_u16;
            continue;
        }
        int codepoints = 0;
        int has_extension = 0;
        int has_joiner = 0;
        int has_emoji = 0;
        int has_rtl = 0;
        zyn_text_range_metrics(text, len, previous_byte, next_byte, &codepoints, &has_extension, &has_joiner, &has_emoji, &has_rtl);
        const int direction = has_rtl ? -1 : zyn_text_direction_for_range(text, len, previous_byte, next_byte);
        byte_boundaries[boundary_count] = next_byte;
        grapheme_codepoint_counts[grapheme_count] = codepoints;
        grapheme_directions[grapheme_count] = direction;
        boundary_count += 1;
        grapheme_count += 1;
        *out_codepoint_count += codepoints;
        if (has_extension && codepoints > 1) { *out_combining_sequence_count += 1; }
        if (has_joiner) { *out_joiner_sequence_count += 1; }
        if (has_emoji && (has_joiner || has_extension || codepoints > 1)) { *out_emoji_sequence_count += 1; }
        if (direction < 0) { *out_rtl_grapheme_count += 1; } else { *out_ltr_grapheme_count += 1; }
        if (last_direction == 0 || last_direction != direction) {
            *out_direction_run_count += 1;
            if (direction < 0) { *out_rtl_run_count += 1; } else { *out_ltr_run_count += 1; }
        }
        last_direction = direction;
        if (codepoints > *out_max_grapheme_codepoints) { *out_max_grapheme_codepoints = codepoints; }
        const int64_t byte_len = next_byte - previous_byte;
        if (byte_len > *out_max_grapheme_bytes) { *out_max_grapheme_bytes = byte_len; }
        previous_u16 = next_u16;
        previous_byte = next_byte;
    }
    if (boundary_count <= 1 || byte_boundaries[boundary_count - 1] != (int64_t)len) {
        if (boundary_count >= max_boundaries || grapheme_count >= max_graphemes) {
            ubrk_close(iter);
            free(u16);
            return -26;
        }
        byte_boundaries[boundary_count] = (int64_t)len;
        grapheme_codepoint_counts[grapheme_count] = 1;
        grapheme_directions[grapheme_count] = 1;
        boundary_count += 1;
        grapheme_count += 1;
        *out_codepoint_count += 1;
        *out_ltr_grapheme_count += 1;
        if (last_direction <= 0) {
            *out_direction_run_count += 1;
            *out_ltr_run_count += 1;
        }
        if (*out_max_grapheme_codepoints < 1) { *out_max_grapheme_codepoints = 1; }
        if (*out_max_grapheme_bytes < ((int64_t)len - previous_byte)) { *out_max_grapheme_bytes = (int64_t)len - previous_byte; }
    }
    *out_boundary_count = boundary_count;
    ubrk_close(iter);
    free(u16);
    return 0;
}

static int zyn_text_run_index_for_grapheme(const ZynBidiRun* runs, int run_count, int grapheme) {
    if (!runs || grapheme < 0) {
        return -1;
    }
    for (int i = 0; i < run_count; ++i) {
        const int first = runs[i].first_grapheme;
        const int last = first + runs[i].length;
        if (grapheme >= first && grapheme < last) {
            return i;
        }
    }
    return -1;
}

int zyn_text_bidi_analyze_utf8(const char* text,
                               int len,
                               const int64_t* byte_boundaries,
                               int boundary_count,
                               int* visual_to_logical_graphemes,
                               int* logical_to_visual_graphemes,
                               int* run_first_graphemes,
                               int* run_lengths,
                               int* run_directions,
                               int* visual_run_to_logical_runs,
                               int* logical_run_to_visual_runs,
                               int max_runs,
                               int* out_run_count,
                               int* out_paragraph_direction) {
    if (out_run_count) {
        *out_run_count = 0;
    }
    if (out_paragraph_direction) {
        *out_paragraph_direction = 1;
    }
    if (!text || len < 0 || !byte_boundaries || boundary_count <= 0
        || !visual_to_logical_graphemes || !logical_to_visual_graphemes
        || !run_first_graphemes || !run_lengths || !run_directions
        || !visual_run_to_logical_runs || !logical_run_to_visual_runs
        || max_runs < 0 || !out_run_count || !out_paragraph_direction) {
        return -1;
    }

    const int grapheme_count = boundary_count - 1;
    if (grapheme_count <= 0) {
        return 0;
    }
    if (max_runs < grapheme_count) {
        return -2;
    }

    UErrorCode status = U_ZERO_ERROR;
    int32_t u16_len = 0;
    u_strFromUTF8(NULL, 0, &u16_len, text, len, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        return -3;
    }
    status = U_ZERO_ERROR;
    UChar* u16 = (UChar*)calloc((size_t)u16_len + 1, sizeof(UChar));
    if (!u16) {
        return -4;
    }
    u_strFromUTF8(u16, u16_len + 1, NULL, text, len, &status);
    if (U_FAILURE(status)) {
        free(u16);
        return -5;
    }

    int* u16_boundaries = (int*)calloc((size_t)boundary_count, sizeof(int));
    ZynBidiRun* visual_runs = (ZynBidiRun*)calloc((size_t)grapheme_count, sizeof(ZynBidiRun));
    ZynBidiRun* logical_runs = (ZynBidiRun*)calloc((size_t)grapheme_count, sizeof(ZynBidiRun));
    if (!u16_boundaries || !visual_runs || !logical_runs) {
        free(logical_runs);
        free(visual_runs);
        free(u16_boundaries);
        free(u16);
        return -6;
    }

    for (int i = 0; i < boundary_count; ++i) {
        u16_boundaries[i] = zyn_text_utf8_to_u16_index(text, len, byte_boundaries[i]);
    }
    u16_boundaries[boundary_count - 1] = u16_len;

    UBiDi* bidi = ubidi_openSized(u16_len, 0, &status);
    if (U_FAILURE(status) || !bidi) {
        free(logical_runs);
        free(visual_runs);
        free(u16_boundaries);
        free(u16);
        return -7;
    }
    status = U_ZERO_ERROR;
    ubidi_setPara(bidi, u16, u16_len, UBIDI_DEFAULT_LTR, NULL, &status);
    if (U_FAILURE(status)) {
        ubidi_close(bidi);
        free(logical_runs);
        free(visual_runs);
        free(u16_boundaries);
        free(u16);
        return -8;
    }

    const UBiDiLevel paragraph_level = ubidi_getParaLevel(bidi);
    *out_paragraph_direction = (paragraph_level & 1) ? -1 : 1;

    status = U_ZERO_ERROR;
    const int visual_run_count = ubidi_countRuns(bidi, &status);
    if (U_FAILURE(status) || visual_run_count < 0 || visual_run_count > max_runs) {
        ubidi_close(bidi);
        free(logical_runs);
        free(visual_runs);
        free(u16_boundaries);
        free(u16);
        return -9;
    }

    int visual_index = 0;
    int used_visual_runs = 0;
    for (int run_i = 0; run_i < visual_run_count; ++run_i) {
        int32_t logical_start = 0;
        int32_t run_len = 0;
        const UBiDiDirection run_dir_raw = ubidi_getVisualRun(bidi, run_i, &logical_start, &run_len);
        const int logical_end = logical_start + run_len;
        int first_g = -1;
        int last_g = -1;
        for (int g = 0; g < grapheme_count; ++g) {
            if (u16_boundaries[g + 1] > logical_start && u16_boundaries[g] < logical_end) {
                if (first_g < 0) {
                    first_g = g;
                }
                last_g = g;
            }
        }
        if (first_g < 0 || last_g < first_g) {
            continue;
        }
        const int direction = run_dir_raw == UBIDI_RTL ? -1 : 1;
        visual_runs[used_visual_runs].first_grapheme = first_g;
        visual_runs[used_visual_runs].length = last_g - first_g + 1;
        visual_runs[used_visual_runs].direction = direction;
        visual_runs[used_visual_runs].visual_index = used_visual_runs;
        used_visual_runs += 1;

        if (direction < 0) {
            for (int g = last_g; g >= first_g; --g) {
                visual_to_logical_graphemes[visual_index] = g;
                logical_to_visual_graphemes[g] = visual_index;
                visual_index += 1;
            }
        } else {
            for (int g = first_g; g <= last_g; ++g) {
                visual_to_logical_graphemes[visual_index] = g;
                logical_to_visual_graphemes[g] = visual_index;
                visual_index += 1;
            }
        }
    }

    int logical_run_count = 0;
    while (logical_run_count < used_visual_runs) {
        int best = -1;
        for (int i = 0; i < used_visual_runs; ++i) {
            int already_used = 0;
            for (int j = 0; j < logical_run_count; ++j) {
                if (logical_runs[j].visual_index == i) {
                    already_used = 1;
                    break;
                }
            }
            if (!already_used && (best < 0 || visual_runs[i].first_grapheme < visual_runs[best].first_grapheme)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        logical_runs[logical_run_count] = visual_runs[best];
        logical_runs[logical_run_count].visual_index = best;
        logical_run_count += 1;
    }

    for (int i = 0; i < logical_run_count; ++i) {
        run_first_graphemes[i] = logical_runs[i].first_grapheme;
        run_lengths[i] = logical_runs[i].length;
        run_directions[i] = logical_runs[i].direction;
        logical_run_to_visual_runs[i] = logical_runs[i].visual_index;
    }
    for (int visual_run_i = 0; visual_run_i < used_visual_runs; ++visual_run_i) {
        const int logical_run_i = zyn_text_run_index_for_grapheme(logical_runs, logical_run_count, visual_runs[visual_run_i].first_grapheme);
        visual_run_to_logical_runs[visual_run_i] = logical_run_i;
    }

    *out_run_count = logical_run_count;
    ubidi_close(bidi);
    free(logical_runs);
    free(visual_runs);
    free(u16_boundaries);
    free(u16);
    return visual_index == grapheme_count ? 0 : -10;
}

static int zyn_text_fill_info(ZynTextFont* font, ZynTextFontInfo* out_info) {
    if (!font || !font->face || !out_info) {
        return -1;
    }
    out_info->ascender = (int)(font->face->size->metrics.ascender >> 6);
    out_info->descender = (int)(font->face->size->metrics.descender >> 6);
    out_info->height = (int)(font->face->size->metrics.height >> 6);
    out_info->max_advance = (int)(font->face->size->metrics.max_advance >> 6);
    out_info->glyph_count = (int)font->face->num_glyphs;
    return 0;
}

void* zyn_text_font_open(const char* path, int pixel_size, ZynTextFontInfo* out_info) {
    if (!path || pixel_size <= 0) {
        return NULL;
    }

    ZynTextFont* font = (ZynTextFont*)calloc(1, sizeof(ZynTextFont));
    if (!font) {
        return NULL;
    }
    font->pixel_size = pixel_size;

    if (FT_Init_FreeType(&font->library) != 0) {
        free(font);
        return NULL;
    }
    if (FT_New_Face(font->library, path, 0, &font->face) != 0) {
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)pixel_size) != 0) {
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }

    font->hb_face = hb_face_create_from_file_or_fail(path, 0);
    if (!font->hb_face) {
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }

    font->hb_font = hb_font_create(font->hb_face);
    if (!font->hb_font) {
        hb_face_destroy(font->hb_face);
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    hb_font_set_scale(font->hb_font, pixel_size * 64, pixel_size * 64);

    if (zyn_text_fill_info(font, out_info) != 0) {
        hb_font_destroy(font->hb_font);
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        free(font);
        return NULL;
    }
    return font;
}

void zyn_text_font_close(void* handle) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font) {
        return;
    }
    if (font->hb_font) {
        hb_font_destroy(font->hb_font);
    }
    if (font->hb_face) {
        hb_face_destroy(font->hb_face);
    }
    if (font->face) {
        FT_Done_Face(font->face);
    }
    if (font->library) {
        FT_Done_FreeType(font->library);
    }
    free(font);
}

int zyn_text_shape_count(void* handle, const char* text, int len) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font || !font->hb_font || !text || len < 0) {
        return -1;
    }
    hb_buffer_t* buffer = hb_buffer_create();
    if (!buffer) {
        return -2;
    }
    hb_buffer_add_utf8(buffer, text, len, 0, len);
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_direction(buffer, zyn_text_direction_for_range(text, len, 0, len) < 0 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(font->hb_font, buffer, NULL, 0);
    unsigned int glyph_count = hb_buffer_get_length(buffer);
    hb_buffer_destroy(buffer);
    return (int)glyph_count;
}

int zyn_text_shape(void* handle, const char* text, int len, ZynShapedGlyph* out_glyphs, int max_glyphs) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font || !font->hb_font || !text || len < 0 || !out_glyphs || max_glyphs < 0) {
        return -1;
    }
    hb_buffer_t* buffer = hb_buffer_create();
    if (!buffer) {
        return -2;
    }
    hb_buffer_add_utf8(buffer, text, len, 0, len);
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_direction(buffer, zyn_text_direction_for_range(text, len, 0, len) < 0 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(font->hb_font, buffer, NULL, 0);

    unsigned int glyph_count = hb_buffer_get_length(buffer);
    if (glyph_count > (unsigned int)max_glyphs) {
        hb_buffer_destroy(buffer);
        return -3;
    }

    hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, NULL);
    hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);
    if ((!infos || !positions) && glyph_count > 0) {
        hb_buffer_destroy(buffer);
        return -4;
    }

    for (unsigned int i = 0; i < glyph_count; ++i) {
        unsigned int src = i;
        out_glyphs[i].glyph_index = infos[src].codepoint;
        out_glyphs[i].cluster = infos[src].cluster;
        out_glyphs[i].advance_x = (int)(positions[src].x_advance / 64);
        out_glyphs[i].advance_y = (int)(positions[src].y_advance / 64);
        out_glyphs[i].offset_x = (int)(positions[src].x_offset / 64);
        out_glyphs[i].offset_y = (int)(positions[src].y_offset / 64);
    }
    hb_buffer_destroy(buffer);
    return (int)glyph_count;
}

uint32_t zyn_text_glyph_index(void* handle, uint32_t codepoint) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font || !font->face) {
        return 0;
    }
    return (uint32_t)FT_Get_Char_Index(font->face, (FT_ULong)codepoint);
}

int zyn_text_load_glyph_index(void* handle, uint32_t glyph_index, ZynGlyphBitmap* out_glyph) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font || !font->face || !out_glyph) {
        return -1;
    }
    memset(out_glyph, 0, sizeof(ZynGlyphBitmap));

    if (FT_Load_Glyph(font->face, (FT_UInt)glyph_index, FT_LOAD_RENDER) != 0) {
        return -2;
    }

    FT_GlyphSlot slot = font->face->glyph;
    FT_Bitmap* bitmap = &slot->bitmap;
    out_glyph->width = (int)bitmap->width;
    out_glyph->height = (int)bitmap->rows;
    out_glyph->bearing_x = slot->bitmap_left;
    out_glyph->bearing_y = slot->bitmap_top;
    out_glyph->advance_x = (int)(slot->advance.x >> 6);
    out_glyph->advance_y = (int)(slot->advance.y >> 6);
    out_glyph->pitch = (int)bitmap->pitch;

    int pitch_abs = out_glyph->pitch < 0 ? -out_glyph->pitch : out_glyph->pitch;
    int byte_count = pitch_abs * out_glyph->height;
    if (byte_count > 0) {
        out_glyph->pixels = malloc((size_t)byte_count);
        if (!out_glyph->pixels) {
            memset(out_glyph, 0, sizeof(ZynGlyphBitmap));
            return -3;
        }
        memcpy(out_glyph->pixels, bitmap->buffer, (size_t)byte_count);
    }
    return 0;
}

int zyn_text_load_glyph(void* handle, uint32_t codepoint, ZynGlyphBitmap* out_glyph) {
    ZynTextFont* font = (ZynTextFont*)handle;
    if (!font || !font->face || !out_glyph) {
        return -1;
    }
    memset(out_glyph, 0, sizeof(ZynGlyphBitmap));

    if (FT_Load_Char(font->face, (FT_ULong)codepoint, FT_LOAD_RENDER) != 0) {
        return -2;
    }

    FT_GlyphSlot slot = font->face->glyph;
    FT_Bitmap* bitmap = &slot->bitmap;
    out_glyph->width = (int)bitmap->width;
    out_glyph->height = (int)bitmap->rows;
    out_glyph->bearing_x = slot->bitmap_left;
    out_glyph->bearing_y = slot->bitmap_top;
    out_glyph->advance_x = (int)(slot->advance.x >> 6);
    out_glyph->advance_y = (int)(slot->advance.y >> 6);
    out_glyph->pitch = (int)bitmap->pitch;

    int pitch_abs = out_glyph->pitch < 0 ? -out_glyph->pitch : out_glyph->pitch;
    int byte_count = pitch_abs * out_glyph->height;
    if (byte_count > 0) {
        out_glyph->pixels = malloc((size_t)byte_count);
        if (!out_glyph->pixels) {
            memset(out_glyph, 0, sizeof(ZynGlyphBitmap));
            return -3;
        }
        memcpy(out_glyph->pixels, bitmap->buffer, (size_t)byte_count);
    }
    return 0;
}

void zyn_text_glyph_free(ZynGlyphBitmap* glyph) {
    if (!glyph) {
        return;
    }
    if (glyph->pixels) {
        free(glyph->pixels);
    }
    memset(glyph, 0, sizeof(ZynGlyphBitmap));
}
