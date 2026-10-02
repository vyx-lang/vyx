#include "icu.hpp"
#include <cstdint>
#include <cstdio>

// Independent use of the upstream public API: no functions from this file are
// exported to Vyx. The Vyx executable links ICU's original import library.
int main() {
    UnicodeString empty;
    if (empty.length() != 0) return 1;
    std::int64_t checksum = 0;
    for (int round = 0; round < 512; ++round) {
        const int count = 32 + round % 97;
        UnicodeString text(1, 128512, count);
        if (text.length() != 2 * count) return 2;
        if (text.getCapacity() < text.length()) return 3;
        if (text.countChar32(0, text.length()) != count) return 4;
        if (text.charAt(0) != 55357) return 5;
        if (text.charAt(1) != 56832) return 6;
        if (text.char32At(0) != 128512) return 7;
        if (text.moveIndex32(0, count) != text.length()) return 8;
        if (text.moveIndex32(text.length(), -count) != 0) return 9;
        UnicodeString copy(text);
        if (copy.length() != text.length()) return 10;
        if (copy.countChar32(0, copy.length()) != count) return 11;
        checksum += copy.char32At(copy.length() - 1);
    }
    if (checksum != 65798144) return 12;
    UnicodeString lower_accent(1, 233, 64), upper_accent(1, 201, 64);
    if (lower_accent.caseCompare(upper_accent, 0) != 0) return 13;
    if (lower_accent.compareCodePointOrder(upper_accent) <= 0) return 14;
    UnicodeString final_sigma(1, 962, 32), upper_sigma(1, 931, 32);
    if (final_sigma.caseCompare(upper_sigma, 0) != 0) return 15;
    UnicodeString upper_i(1, 73, 32), lower_i(1, 105, 32);
    if (upper_i.caseCompare(lower_i, 0) != 0) return 16;
    if (upper_i.caseCompare(lower_i, 1) == 0) return 17;
    UnicodeString mutable_text(1, 97, 128);
    mutable_text.toUpper();
    if (mutable_text.charAt(0) != 65) return 18;
    mutable_text.append(128512);
    if (mutable_text.length() != 130) return 19;
    if (mutable_text.countChar32(0, mutable_text.length()) != 129) return 20;
    mutable_text.toLower();
    if (mutable_text.charAt(0) != 97) return 21;
    UnicodeString saved_copy(mutable_text);
    mutable_text.toUpper();
    if (saved_copy.charAt(0) != 97) return 22;
    UnicodeString sharp_s(1, 223, 2);
    sharp_s.toUpper();
    if (sharp_s.length() != 4) return 23;
    if (sharp_s.charAt(0) != 83 || sharp_s.charAt(3) != 83) return 24;
    std::puts("DCI ICU 78.3 OK");
    return 0;
}
