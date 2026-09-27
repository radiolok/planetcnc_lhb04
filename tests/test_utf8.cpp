#include "test_framework.h"
#include "utils/Utf8.h"

#include <string>

using namespace mpgd;

static void test_ascii_roundtrip() {
    CHECK(utf8FromWide(L"XHC LHB04") == "XHC LHB04");
    CHECK(wideFromUtf8("C:/PlanetCNC/lib.dll") == L"C:/PlanetCNC/lib.dll");
    CHECK(utf8FromWide(static_cast<const wchar_t*>(nullptr)).empty());
}

static void test_cyrillic_roundtrip() {
    // "Станок" in UTF-8.
    const std::string u8 = "\xD0\xA1\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xBA";
    const std::wstring w = wideFromUtf8(u8);
    CHECK_EQ(w.size(), 6u);
    CHECK(w[0] == static_cast<wchar_t>(0x0421));
    CHECK(utf8FromWide(w) == u8);
}

static void test_astral_roundtrip() {
    // U+1F600 needs a surrogate pair where wchar_t is 16-bit.
    const std::string u8 = "\xF0\x9F\x98\x80";
    const std::wstring w = wideFromUtf8(u8);
    CHECK_EQ(w.size(), sizeof(wchar_t) == 2 ? 2u : 1u);
    CHECK(utf8FromWide(w) == u8);
}

static void test_invalid_utf8_replaced() {
    // Lone continuation byte, then a truncated 3-byte sequence.
    const std::wstring w =
        wideFromUtf8(std::string("a\x80"
                                 "b\xE2\x82",
                                 5));
    CHECK(w.size() >= 3);
    CHECK(w[0] == L'a');
    CHECK(w[1] == static_cast<wchar_t>(0xFFFD));
    CHECK(w[2] == L'b');
    // Overlong encoding of '/' is rejected.
    CHECK(wideFromUtf8("\xC0\xAF")[0] == static_cast<wchar_t>(0xFFFD));
}

int main() {
    test_ascii_roundtrip();
    test_cyrillic_roundtrip();
    test_astral_roundtrip();
    test_invalid_utf8_replaced();
    return tfw::summary("test_utf8");
}
