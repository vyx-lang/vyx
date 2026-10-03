#include <cstdio>
#include <unicode/ubrk.h>
#include <unicode/uclean.h>
#include <unicode/ucol.h>
#include <unicode/icudataver.h>
#include <unicode/ures.h>
#include <unicode/uversion.h>

int main() {
    UErrorCode error = U_ZERO_ERROR;
    u_init(&error);
    if (U_FAILURE(error)) {
        std::fprintf(stderr, "u_init: %s\n", u_errorName(error));
        return 1;
    }
    UVersionInfo data_version{};
    u_getDataVersion(data_version, &error);
    if (U_FAILURE(error) || data_version[0] != 78 || data_version[1] != 3) {
        std::fprintf(stderr, "ICU data version: %s (%u.%u)\n", u_errorName(error), data_version[0], data_version[1]);
        return 2;
    }
    UResourceBundle* resource = ures_open(nullptr, "zh", &error);
    if (U_FAILURE(error) || !resource || ures_getSize(resource) == 0) {
        std::fprintf(stderr, "Chinese resource bundle: %s\n", u_errorName(error));
        return 3;
    }
    ures_close(resource);
    error = U_ZERO_ERROR;
    UCollator* collator = ucol_open("zh", &error);
    if (U_FAILURE(error) || !collator) {
        std::fprintf(stderr, "Chinese collation: %s\n", u_errorName(error));
        return 4;
    }
    const UChar a[] = {u'a'};
    const UChar b[] = {u'b'};
    if (ucol_strcoll(collator, a, 1, b, 1) != UCOL_LESS) { return 5; }
    ucol_close(collator);
    error = U_ZERO_ERROR;
    const UChar thai[] = {0x0e20, 0x0e32, 0x0e29, 0x0e32, 0x0e44, 0x0e17, 0x0e22};
    UBreakIterator* words = ubrk_open(UBRK_WORD, "th", thai, 7, &error);
    if (U_FAILURE(error) || !words) {
        std::fprintf(stderr, "Thai word data: %s\n", u_errorName(error));
        return 6;
    }
    int boundaries = 0;
    for (int32_t boundary = ubrk_first(words); boundary != UBRK_DONE; boundary = ubrk_next(words)) { ++boundaries; }
    ubrk_close(words);
    if (boundaries < 3) { std::fprintf(stderr, "Thai dictionary did not split the two words\n"); return 7; }
    u_cleanup();
    std::puts("ICU 78.3 full data: locale, collation and Thai dictionary OK");
    return 0;
}
