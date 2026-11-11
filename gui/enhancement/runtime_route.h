// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include <string>
#include <cwctype>

namespace chiaki_enhancement
{
// Conservative gate for the raw NGX worker. Every requested output is still
// evaluated and verified; a device name never establishes runtime success.
inline unsigned maxFgMultiplier(std::wstring adapter)
{
    for (auto &c : adapter) c = static_cast<wchar_t>(std::towupper(c));
    if (adapter.find(L"GEFORCE") == std::wstring::npos) return 2;
    auto p = adapter.find(L"RTX");
    if (p == std::wstring::npos) return 2;
    p += 3;
    while (p < adapter.size() && std::iswspace(adapter[p])) ++p;
    return p + 4 <= adapter.size() && adapter.compare(p, 2, L"50") == 0 &&
        std::iswdigit(adapter[p + 2]) && std::iswdigit(adapter[p + 3]) &&
        (p + 4 == adapter.size() || !std::iswdigit(adapter[p + 4])) ? 4 : 2;
}
inline bool isRtxAdapter(unsigned vendor, std::wstring adapter)
{
    if (vendor != 0x10de)
        return false;
    for (auto &c : adapter)
        c = static_cast<wchar_t>(std::towupper(c));
    return adapter.find(L"RTX") != std::wstring::npos;
}

// Keep the GUI's package checks and the worker's loader on the same route.
// Only the consumer RTX 20/30 families select the optional SM75/SM86 backend;
// newer and unrecognized adapters retain NVIDIA's native runtime path.
inline bool usesSm86(std::wstring adapter)
{
    for (auto &c : adapter)
        c = static_cast<wchar_t>(std::towupper(c));
    auto position = adapter.find(L"RTX");
    if (position == std::wstring::npos)
        return false;
    position += 3;
    while (position < adapter.size() && std::iswspace(adapter[position]))
        ++position;
    return position + 4 <= adapter.size() &&
           (adapter.compare(position, 2, L"20") == 0 || adapter.compare(position, 2, L"30") == 0) &&
           std::iswdigit(adapter[position + 2]) && std::iswdigit(adapter[position + 3]) &&
           (position + 4 == adapter.size() || !std::iswdigit(adapter[position + 4]));
}
}
