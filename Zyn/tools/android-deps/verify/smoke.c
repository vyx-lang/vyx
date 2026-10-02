#include <stdio.h>
#include <SDL3/SDL_version.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

extern int zyn_text_icu_major_version(void);

int main(void) {
    FT_Library ft = NULL;
    int major = 0, minor = 0, patch = 0;
    if (FT_Init_FreeType(&ft) != 0) return 10;
    FT_Library_Version(ft, &major, &minor, &patch);
    printf("FreeType %d.%d.%d\n", major, minor, patch);
    if (major != 2 || minor != 14 || patch != 1) return 11;
    if (FT_Done_FreeType(ft) != 0) return 12;
    unsigned hb_major, hb_minor, hb_micro;
    hb_version(&hb_major, &hb_minor, &hb_micro);
    printf("HarfBuzz %u.%u.%u\n", hb_major, hb_minor, hb_micro);
    if (hb_major != 14 || hb_minor != 2 || hb_micro != 0) return 20;
    hb_buffer_t *buffer = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buffer)) return 21;
    hb_buffer_destroy(buffer);
    const int sdl_version = SDL_GetVersion();
    printf("SDL %d\n", sdl_version);
    if (sdl_version != SDL_VERSIONNUM(3, 4, 8)) return 30;
    UVersionInfo icu_version;
    u_getVersion(icu_version);
    printf("ICU %d.%d; bridge %d\n", icu_version[0], icu_version[1], zyn_text_icu_major_version());
    if (icu_version[0] != 78 || icu_version[1] != 3 || zyn_text_icu_major_version() != 78) return 40;
    UErrorCode status = U_ZERO_ERROR;
    UChar text[32];
    int32_t length = 0;
    u_strFromUTF8(text, 32, &length, "a\xcc\x81\xe4\xb8\xad\xf0\x9f\x98\x80", -1, &status);
    if (U_FAILURE(status)) return 41;
    UBreakIterator *graphemes = ubrk_open(UBRK_CHARACTER, "en", text, length, &status);
    if (U_FAILURE(status) || !graphemes) return 42;
    int count = 0;
    ubrk_first(graphemes);
    while (ubrk_next(graphemes) != UBRK_DONE) ++count;
    ubrk_close(graphemes);
    printf("Unicode graphemes %d\n", count);
    if (count != 3) return 43;
    status = U_ZERO_ERROR;
    u_strFromUTF8(text, 32, &length, "abc \xd7\x90\xd7\x91\xd7\x92", -1, &status);
    if (U_FAILURE(status)) return 44;
    UBiDi *bidi = ubidi_open();
    ubidi_setPara(bidi, text, length, UBIDI_DEFAULT_LTR, NULL, &status);
    if (U_FAILURE(status) || ubidi_getDirection(bidi) != UBIDI_MIXED) return 45;
    ubidi_close(bidi);
    puts("zyn Android dependencies: OK");
    return 0;
}
