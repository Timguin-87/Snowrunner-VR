#include "hooks/pattern_scan.h"
#include "common/log.h"

#include <atomic>
#include <cstring>
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

IMAGE_NT_HEADERS* nt_headers(HMODULE module)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    if (!base) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    return nt;
}

// The primary RUNTIME_FUNCTION for `p`: the one covering it, with any chain of
// UNW_FLAG_CHAININFO links followed back to the function's real entry. A
// chained entry stores its parent's RUNTIME_FUNCTION right after its own
// unwind codes (padded to an even count).
PRUNTIME_FUNCTION primary_function(const void* p, DWORD64& imageBase)
{
    imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(p), &imageBase, nullptr);
    for (int guard = 0; rf && guard < 32; ++guard) {
        const uint8_t* ui = reinterpret_cast<const uint8_t*>(imageBase + rf->UnwindData);
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO))
            return rf;
        const uint8_t codes = ui[2];
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(
            const_cast<uint8_t*>(ui) + 4 + 2 * ((codes + 1) & ~1));
    }
    return rf;
}

std::atomic<bool> g_dumpGameCode{false};

} // namespace

int pattern_scan_all(HMODULE module, const char* ida_pattern, uint8_t** out, int cap)
{
    Pattern pat;
    if (!parse(ida_pattern, pat))
        return 0;
    IMAGE_NT_HEADERS* nt = nt_headers(module);
    if (!nt)
        return 0;
    auto* base = reinterpret_cast<uint8_t*>(module);

    // Anchor on the first concrete byte and let memchr do the walking: 35 MB
    // of .text a byte at a time, once per signature, is a noticeable stall at
    // startup; memchr is a fraction of it.
    const size_t n = pat.bytes.size();
    size_t anchor = 0;
    while (anchor < n && pat.wildcard[anchor]) ++anchor;
    if (anchor == n)
        return 0;   // all wildcards matches everything; never a real signature

    int found = 0;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* begin = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < n)
            continue;
        uint8_t* const last = begin + size - n;   // last valid match start

        uint8_t* p = begin;
        while (p <= last) {
            auto* hit = static_cast<uint8_t*>(
                memchr(p + anchor, pat.bytes[anchor], (size_t)(last - p) + 1));
            if (!hit)
                break;
            uint8_t* cand = hit - anchor;
            size_t j = 0;
            for (; j < n; ++j)
                if (!pat.wildcard[j] && cand[j] != pat.bytes[j])
                    break;
            if (j == n) {
                if (found < cap && out) out[found] = cand;
                ++found;
            }
            p = cand + 1;
        }
    }
    return found;
}

bool is_function_entry(const void* p)
{
    if (!p) return false;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(p), &imageBase, nullptr);
    const DWORD64 a = reinterpret_cast<DWORD64>(p);
    if (rf) {
        if (imageBase + rf->BeginAddress != a)
            return false;   // inside a function: the stale-offset case
        const uint8_t* ui = reinterpret_cast<const uint8_t*>(imageBase + rf->UnwindData);
        return !((ui[0] >> 3) & UNW_FLAG_CHAININFO);
    }
    // No unwind entry: a leaf function, or not code at all.
    if (a & 0xF) return false;
    __try {
        const uint8_t before = reinterpret_cast<const uint8_t*>(p)[-1];
        return before == 0xCC || before == 0xC3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool function_range(const void* p, uint8_t*& begin, uint8_t*& end)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = primary_function(p, imageBase);
    if (!rf) return false;
    begin = reinterpret_cast<uint8_t*>(imageBase + rf->BeginAddress);
    end   = reinterpret_cast<uint8_t*>(imageBase + rf->EndAddress);
    return true;
}

std::vector<uint8_t*> find_calls_to(HMODULE module, const void* callee)
{
    std::vector<uint8_t*> out;
    IMAGE_NT_HEADERS* nt = nt_headers(module);
    if (!nt || !callee)
        return out;
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto want = reinterpret_cast<intptr_t>(callee);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* p = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < 5)
            continue;
        uint8_t* const last = p + size - 5;
        while (p <= last) {
            p = static_cast<uint8_t*>(memchr(p, 0xE8, (size_t)(last - p) + 1));
            if (!p)
                break;
            int32_t disp;
            memcpy(&disp, p + 1, sizeof(disp));
            if (reinterpret_cast<intptr_t>(p) + 5 + disp == want)
                out.push_back(p);
            ++p;
        }
    }
    return out;
}

uint8_t* rel32_target(const uint8_t* insn, int dispOffset, int insnLen)
{
    int32_t disp = 0;
    memcpy(&disp, insn + dispOffset, sizeof(disp));
    return const_cast<uint8_t*>(insn) + insnLen + disp;
}

std::vector<uint8_t*> find_rip_refs(HMODULE module, const uint8_t* opcode, int opLen,
                                    const void* target)
{
    std::vector<uint8_t*> out;
    IMAGE_NT_HEADERS* nt = nt_headers(module);
    if (!nt || !opcode || opLen < 1 || !target)
        return out;
    auto* base = reinterpret_cast<uint8_t*>(module);
    const size_t insnLen = (size_t)opLen + 4;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* p = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < insnLen)
            continue;
        uint8_t* const last = p + size - insnLen;
        while (p <= last) {
            p = static_cast<uint8_t*>(memchr(p, opcode[0], (size_t)(last - p) + 1));
            if (!p)
                break;
            if (memcmp(p, opcode, (size_t)opLen) == 0 &&
                rel32_target(p, opLen, (int)insnLen) == target)
                out.push_back(p);
            ++p;
        }
    }
    return out;
}

std::vector<uint8_t*> find_cstrings(HMODULE module, const char* s)
{
    std::vector<uint8_t*> out;
    IMAGE_NT_HEADERS* nt = nt_headers(module);
    const size_t len = s ? strlen(s) : 0;
    if (!nt || !len)
        return out;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD ch = section->Characteristics;
        if (!(ch & IMAGE_SCN_CNT_INITIALIZED_DATA) || (ch & IMAGE_SCN_MEM_EXECUTE) ||
            (ch & IMAGE_SCN_MEM_WRITE))
            continue;
        uint8_t* const begin = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < len + 1)
            continue;
        uint8_t* const last = begin + size - (len + 1);   // room for the NUL
        uint8_t* p = begin;
        while (p <= last) {
            p = static_cast<uint8_t*>(memchr(p, (unsigned char)s[0], (size_t)(last - p) + 1));
            if (!p)
                break;
            if (memcmp(p, s, len + 1) == 0 && (p == begin || p[-1] == 0))
                out.push_back(p);
            ++p;
        }
    }
    return out;
}

void** import_slot(HMODULE module, const char* dll, const char* fn)
{
    IMAGE_NT_HEADERS* nt = nt_headers(module);
    if (!nt || !fn)
        return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(module);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size)
        return nullptr;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (dll && _stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0)
            continue;
        // OriginalFirstThunk holds the NAMES, FirstThunk holds the live
        // pointers. A bound import has no OFT, in which case the names are not
        // recoverable and this import is left alone rather than guessed at.
        if (!imp->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
                continue;
            auto* imn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imn->Name), fn) == 0)
                return reinterpret_cast<void**>(&slots[i].u1.Function);
        }
    }
    return nullptr;
}

uint32_t exe_timestamp()
{
    IMAGE_NT_HEADERS* nt = nt_headers(GetModuleHandleW(nullptr));
    return nt ? nt->FileHeader.TimeDateStamp : 0;
}

uint32_t exe_image_size()
{
    IMAGE_NT_HEADERS* nt = nt_headers(GetModuleHandleW(nullptr));
    return nt ? nt->OptionalHeader.SizeOfImage : 0;
}

void log_game_build()
{
    VRLOG("game build: SnowRunner.exe timestamp 0x%08X, image size 0x%X",
          exe_timestamp(), exe_image_size());
}

void set_dump_game_code(bool on) { g_dumpGameCode.store(on); }
bool dump_game_code() { return g_dumpGameCode.load(); }

void maybe_dump_game_image()
{
    if (!g_dumpGameCode.load())
        return;
    HMODULE exe = GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS* nt = nt_headers(exe);
    if (!nt) return;

    // Beside this DLL, i.e. beside the log.
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&maybe_dump_game_image), &self);
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 48) return;
    for (wchar_t* q = path + len; q > path; --q)
        if (q[-1] == L'\\') { *q = L'\0'; break; }
    wchar_t name[64];
    swprintf_s(name, L"snowrunner_image_%08X.bin", nt->FileHeader.TimeDateStamp);
    wcscat_s(path, name);

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        VRLOG("DumpGameCode: %ls already exists for this build -- not rewritten", name);
        return;
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        VRLOG("DumpGameCode: could not create %ls (err=%lu)", name, GetLastError());
        return;
    }

    // Region by region, so a page that cannot be read (there should be none in
    // a mapped image, but this is someone else's process) becomes zeros at the
    // right offset instead of ending the dump or shifting everything after it.
    auto* base = reinterpret_cast<uint8_t*>(exe);
    const size_t total = nt->OptionalHeader.SizeOfImage;
    std::vector<uint8_t> zeros(0x10000, 0);
    size_t off = 0, unreadable = 0;
    bool ok = true;
    while (ok && off < total) {
        MEMORY_BASIC_INFORMATION mbi{};
        size_t chunk = (total - off < 0x10000) ? total - off : 0x10000;
        bool readable = false;
        if (VirtualQuery(base + off, &mbi, sizeof(mbi))) {
            const size_t regionLeft =
                (reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) - (base + off);
            if (regionLeft < chunk) chunk = regionLeft;
            const DWORD prot = mbi.Protect & 0xFF;
            readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                       prot != PAGE_NOACCESS && prot != 0;
        }
        // WriteFile probes the source buffer in the kernel and FAILS on an
        // unreadable one rather than faulting here, so a page that changed
        // protection between VirtualQuery and now costs a retry with zeros.
        DWORD written = 0;
        if (readable && WriteFile(h, base + off, (DWORD)chunk, &written, nullptr) &&
            written == chunk) {
            off += chunk;
            continue;
        }
        unreadable += chunk;
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)off;   // undo a partial write
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
             WriteFile(h, zeros.data(), (DWORD)chunk, &written, nullptr) && written == chunk;
        off += chunk;
    }
    CloseHandle(h);
    if (ok)
        VRLOG("DumpGameCode: wrote %ls (%zu bytes, %zu unreadable as zeros) -- "
              "set DumpGameCode=false again once it has been collected", name, total, unreadable);
    else {
        DeleteFileW(path);
        VRLOG("DumpGameCode: write FAILED at offset 0x%zX -- partial file removed", off);
    }
}

} // namespace hooks
