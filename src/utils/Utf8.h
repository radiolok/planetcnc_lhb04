#pragma once

#include <string>

namespace mpgd {

// Conversions between UTF-8 (std::string, used throughout mpgd, the YAML
// config and the logs) and wide strings (hidapi device strings, Win32 paths).
// wchar_t is UTF-16 on Windows and UTF-32 elsewhere; both are handled.
// Invalid input is replaced with U+FFFD rather than dropped or truncated.

std::string utf8FromWide(const wchar_t* w);
std::string utf8FromWide(const std::wstring& w);
std::wstring wideFromUtf8(const std::string& s);

} // namespace mpgd
