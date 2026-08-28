#include "hooks/pattern_scan.h"

#include <vector>

namespace hooks {

namespace {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcard;
};

bool parse(const char* s, Pattern& out)
{
    for (const char* p = s; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            out.bytes.push_back(0);
            out.wildcard.push_back(true);
            while (*p == '?') ++p;
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = hex(p[0]);
        int lo = p[1] ? hex(p[1]) : -1;
        if (hi < 0 || lo < 0)
            return false;
        out.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        out.wildcard.push_back(false);
        p += 2;
    }
    return !out.bytes.empty();
}

} // namespace

uint8_t* pattern_scan(HMODULE module, const char* ida_pattern)
{
    Pattern pat;
    if (!parse(ida_pattern, pat))
        return nullptr;

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;

        uint8_t* begin = base + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        if (size < pat.bytes.size())
            continue;

        const size_t n = pat.bytes.size();
        for (uint8_t* p = begin; p <= begin + size - n; ++p) {
            size_t j = 0;
            for (; j < n; ++j) {
                if (!pat.wildcard[j] && p[j] != pat.bytes[j])
                    break;
            }
            if (j == n)
                return p;
        }
    }
    return nullptr;
}

} // namespace hooks
