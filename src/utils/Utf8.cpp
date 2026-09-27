#include "utils/Utf8.h"

#include <cstdint>
#include <type_traits>

namespace mpgd {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void appendWide(std::wstring& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            return;
        }
    }
    out.push_back(static_cast<wchar_t>(cp));
}

} // namespace

std::string utf8FromWide(const wchar_t* w) {
    std::string out;
    if (!w) return out;
    while (*w) {
        char32_t cp = static_cast<char32_t>(static_cast<std::make_unsigned_t<wchar_t>>(*w++));
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                const char32_t lo = static_cast<char32_t>(
                    static_cast<std::make_unsigned_t<wchar_t>>(*w));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++w;
                } else {
                    cp = kReplacement; // unpaired high surrogate
                }
            }
        }
        appendUtf8(out, cp);
    }
    return out;
}

std::string utf8FromWide(const std::wstring& w) {
    return utf8FromWide(w.c_str());
}

std::wstring wideFromUtf8(const std::string& s) {
    std::wstring out;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        int len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        // clang-format off
        if (c < 0x80)           { len = 1; cp = c;        min = 0; }
        else if ((c >> 5) == 6) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c >> 4) == 14){ len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c >> 3) == 30){ len = 4; cp = c & 0x07; min = 0x10000; }
        else { appendWide(out, kReplacement); ++i; continue; }
        // clang-format on

        if (i + static_cast<size_t>(len) > n) {
            appendWide(out, kReplacement);
            break;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = p[i + static_cast<size_t>(k)];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp < min) {
            // Malformed or overlong: skip only the lead byte and resync.
            appendWide(out, kReplacement);
            ++i;
            continue;
        }
        appendWide(out, cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

} // namespace mpgd
