#include "hooks/camera_fov.h"
#include "hooks/camera_hook.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace hooks {
namespace {

// m_fFOV, measured. See camera_fov.h.
constexpr int kFovOffset = 0x108;
constexpr int kObjectSize = 0x118;

std::atomic<float> g_factor{1.0f};
std::atomic<bool>  g_sweeping{false};
std::atomic<bool>  g_refind{false};      // set by a faulting write, acted on in Present
std::atomic<uint64_t> g_sweptBytes{0};

// g_cams is APPENDED TO by the sweep (Present thread) and READ by both Present
// and the projection builder. g_found is the published count: readers only ever
// touch [0, g_found), and a re-search publishes 0 BEFORE it touches the array,
// so a reader sees either the old complete set or nothing. No lock needed.
constexpr int kMaxCams = 16;
void* g_cams[kMaxCams];
std::atomic<int> g_found{0};
int   g_camCount = 0;               // the sweep's private cursor into g_cams
char  g_status[160] = "waiting for gameplay";

void set_status(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

// --- region filtering ----------------------------------------------------
constexpr int kMaxStacks = 256;
uintptr_t g_stackBases[kMaxStacks];
int       g_stackCount = 0;

void collect_thread_stacks()
{
    g_stackCount = 0;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || g_stackCount >= kMaxStacks) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c) && c.Rsp) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery((LPCVOID)c.Rsp, &mbi, sizeof(mbi)))
                    g_stackBases[g_stackCount++] = (uintptr_t)mbi.AllocationBase;
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te) && g_stackCount < kMaxStacks);
    }
    CloseHandle(snap);
}

bool scannable(const MEMORY_BASIC_INFORMATION& mbi)
{
    if (mbi.State != MEM_COMMIT || mbi.Type == MEM_IMAGE) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READWRITE) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    // Write-combined and uncached pages are GPU apertures: upload heaps and
    // resizable-BAR windows. They cannot hold a CPU-side camera object, they run
    // to GIGABYTES on a modern card, and reading them uncached is slow enough to
    // be the frame-rate drag by itself. Skipping them is most of the reason this
    // sweep now reaches its end at all.
    if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    const uintptr_t ab = (uintptr_t)mbi.AllocationBase;
    for (int i = 0; i < g_stackCount; ++i)
        if (g_stackBases[i] == ab) return false;      // a camera on a stack is a
    return true;                                      // temporary, not the object
}

// --- identifying a camera ------------------------------------------------
bool looks_like_projection(const float* m)
{
    if (m[3] != 0.0f || m[7] != 0.0f || m[15] != 0.0f) return false;
    const float w2 = m[11];
    if (!(w2 > 0.9f && w2 < 1.1f) && !(w2 < -0.9f && w2 > -1.1f)) return false;
    if (!(m[0] > 0.05f && m[0] < 100.0f)) return false;
    if (!(m[5] > 0.05f && m[5] < 100.0f)) return false;
    if (m[1] != 0.0f || m[2] != 0.0f || m[4] != 0.0f) return false;
    if (m[6] != 0.0f || m[8] != 0.0f || m[9] != 0.0f) return false;
    return true;
}

bool looks_like_view(const float* m)
{
    for (int r = 0; r < 3; ++r) {
        const float* v = m + r * 4;
        const float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (l < 0.96f || l > 1.04f) return false;
    }
    return true;
}

bool viewproj_consistent(const float* view, const float* proj, const float* vp)
{
    float want[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += view[r * 4 + k] * proj[k * 4 + c];
            want[r * 4 + c] = v;
        }
    float scale = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const float m = want[i] < 0.0f ? -want[i] : want[i];
        if (m > scale) scale = m;
    }
    if (!(scale > 1.0e-6f)) return false;
    for (int i = 0; i < 16; ++i) {
        const float d = want[i] - vp[i];
        const float tol = 1.0e-3f * scale;
        if (d > tol || d < -tol) return false;
    }
    return true;
}

// --- the sweep -----------------------------------------------------------
constexpr size_t kBudgetPerFrame = 8u * 1024u * 1024u;

// ONCE A CAMERA IS FOUND, keep going only far enough past it to pick up the
// double-buffered sibling sitting beside it, then STOP. The user address space
// is 128 TB; carrying on through all of it after we already have what we came
// for is how this ran forever while reporting "2 cameras found so far" -- and
// nothing is written until the sweep ends, so those two sat there unused.
constexpr uintptr_t kSettleBytes = 1u * 1024u * 1024u;

// And if nothing is ever found, END, rather than drag the frame rate down
// indefinitely -- the exact failure the frustum search was removed for.
constexpr uint64_t kGiveUpBytes = 12ull * 1024ull * 1024ull * 1024ull;

uintptr_t g_cursor = 0;
uintptr_t g_stopAt = 0;             // nonzero once the first camera is found

// THE CAMERA OBJECT ONLY EXISTS IN GAMEPLAY. Searching on the menu or in the
// garage cannot succeed -- it just walks 12 GB and gives up, and because a
// finished sweep never restarts itself that one wasted attempt used to leave the
// FOV undeclared for the whole session. So the sweep waits for the drive camera
// to be ticking, and gets one attempt per visit to gameplay: leaving and coming
// back re-arms it.
//
// A COCKPIT/ORBIT SWITCH RE-ARMS IT TOO, but only while nothing has been found.
// That switch is the one moment in gameplay when the engine is demonstrably
// building camera state, and a sweep that came up empty a second after the
// level loaded may simply have been early. Gated on having nothing, because a
// re-arm once the set is good would throw away a working answer and spend a
// second of frame time re-deriving it every time you looked out of the cab.
constexpr DWORD kGameplaySettleMs = 1000;   // let the camera be built first
DWORD g_liveSince = 0;              // Present thread only
bool  g_attempted = false;          // a sweep has been run for this visit
int   g_lastCockpit = -1;           // -1 = not yet read; else 0/1 from the flag

void finish_sweep(const char* why)
{
    g_sweeping.store(false, std::memory_order_relaxed);
    g_found.store(g_camCount, std::memory_order_release);
    VRLOG("CAMERA FOV: sweep %s -- %d camera(s), %llu MB scanned", why, g_camCount,
          (unsigned long long)(g_sweptBytes.load(std::memory_order_relaxed) >> 20));
}

void scan_slice()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t maxAddr = (uintptr_t)si.lpMaximumApplicationAddress;
    size_t budget = kBudgetPerFrame;

    while (g_cursor < maxAddr && budget > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)g_cursor, &mbi, sizeof(mbi))) break;
        const uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd  = regionBase + mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
        if (!scannable(mbi)) { g_cursor = regionEnd; continue; }

        const uintptr_t from = (g_cursor > regionBase) ? g_cursor : regionBase;
        size_t take = (size_t)(regionEnd - from);
        if (take > budget) take = budget;

        __try {
            const float* p = (const float*)from;
            const size_t floats = take / sizeof(float);
            const size_t need = kObjectSize / sizeof(float);
            for (size_t i = 0; i + need <= floats; i += 4) {
                const float* obj = p + i;
                if (!looks_like_projection(obj + 0x10)) continue;
                if (!looks_like_view(obj)) continue;
                if (!viewproj_consistent(obj, obj + 0x10, obj + 0x30)) continue;
                if (g_camCount >= kMaxCams) break;
                const float fovRad = obj[kFovOffset / 4];
                // A camera whose FOV field is not a sane angle is not one we can
                // drive, whatever else it looks like.
                if (!(fovRad > 0.05f && fovRad < 3.10f)) continue;
                g_cams[g_camCount++] = (void*)obj;
                g_found.store(g_camCount, std::memory_order_release);
                if (!g_stopAt) g_stopAt = (uintptr_t)obj + kSettleBytes;
                VRLOG("CAMERA FOV: camera at %p, m_fFOV %.5f rad (%.2f deg), "
                      "near %.2f far %.2f", (void*)obj, fovRad,
                      fovRad * 57.2957795f, obj[0x45], obj[0x44]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_cursor = regionEnd;
            continue;
        }

        budget -= take;                       // advance by what was READ, never
        g_sweptBytes.fetch_add(take, std::memory_order_relaxed);  // by the region
        g_cursor = from + take;                                   // size

        if (g_stopAt && g_cursor >= g_stopAt) {
            finish_sweep("stopped at the first find");
            return;
        }
    }

    if (g_cursor >= maxAddr) { finish_sweep("complete"); return; }

    if (g_sweptBytes.load(std::memory_order_relaxed) >= kGiveUpBytes) {
        g_cursor = maxAddr;
        finish_sweep("gave up");
    }
}

// The address is a heap allocation under ASLR, so it is good for this launch
// only -- and not even for all of it, because the object can be freed and its
// block handed to something else. That does NOT fault; it just means a float
// going into a stranger at +0x108. So every write re-checks the signature
// first. Cheap: a handful of compares and nine multiplies.
bool still_a_camera(const float* obj)
{
    if (!looks_like_projection(obj + 0x10)) return false;
    if (!looks_like_view(obj)) return false;
    const float f = obj[kFovOffset / 4];
    return f > 0.05f && f < 3.10f;
}

void write_fov()
{
    const int n = g_found.load(std::memory_order_acquire);
    if (n <= 0) return;
    const float deg = xr::render_hfov_deg() * g_factor.load(std::memory_order_relaxed);
    if (!(deg > 5.0f && deg < 175.0f)) return;
    const float rad = deg / 57.2957795f;

    bool stale = false;
    for (int i = 0; i < n && !stale; ++i) {
        __try {
            if (still_a_camera((const float*)g_cams[i]))
                *(float*)((char*)g_cams[i] + kFovOffset) = rad;
            else
                stale = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stale = true;      // unmapped outright
        }
    }
    if (stale) {
        // A REQUEST only: this also runs on the projection-builder thread, and
        // the sweep state belongs to Present. Logged once per occurrence, not
        // once per frame until Present gets round to it.
        if (!g_refind.exchange(true, std::memory_order_relaxed))
            VRLOG("CAMERA FOV: camera object no longer valid -- re-searching");
    }
}

} // namespace

float camera_fov_factor()  { return g_factor.load(std::memory_order_relaxed); }
const char* camera_fov_status() { return g_status; }

float camera_fov_result_deg()
{
    return xr::render_hfov_deg() * g_factor.load(std::memory_order_relaxed);
}

void set_camera_fov_factor(float f)
{
    if (f < 0.5f) f = 0.5f;
    if (f > 2.5f) f = 2.5f;
    g_factor.store(f, std::memory_order_relaxed);
}

void camera_fov_on_present()
{
    if (g_sweeping.load(std::memory_order_relaxed)) {
        scan_slice();
        if (g_sweeping.load(std::memory_order_relaxed)) {
            set_status("searching -- %llu MB, %d camera(s) so far",
                       (unsigned long long)(g_sweptBytes.load(std::memory_order_relaxed) >> 20),
                       g_found.load(std::memory_order_relaxed));
            return;
        }
        // The sweep ended inside this very frame -- fall through and start
        // writing now instead of idling until the next one.
    }

    if (g_refind.exchange(false, std::memory_order_relaxed)) {
        g_found.store(0, std::memory_order_release);
        g_camCount = 0;
        g_attempted = false;
    }

    // See kGameplaySettleMs. No drive camera means no camera object to find or
    // to write, so hold everything -- including the set we already have, which
    // may well still be valid when gameplay resumes.
    const bool live = hooks::drive_camera_live();
    if (!live) {
        g_liveSince = 0;
        g_attempted = false;
        g_lastCockpit = -1;   // a stale reading would fake a switch on return
        const int n = g_found.load(std::memory_order_relaxed);
        if (n) set_status("holding %d camera(s) -- idle until gameplay resumes", n);
        else   set_status("waiting for gameplay -- no drive camera yet");
        return;
    }
    if (!g_liveSince) g_liveSince = GetTickCount();

    // The cockpit/exterior switch, watched only while we have nothing. Reading
    // it every frame either way would be a wasted __try; the -1 "unknown" case
    // is simply not a transition, so a build that ever loses the flag degrades
    // to the old one-attempt-per-visit behaviour rather than re-sweeping
    // forever.
    const int nowCockpit = hooks::drive_camera_cockpit();
    if (!g_found.load(std::memory_order_relaxed) && nowCockpit >= 0) {
        if (g_lastCockpit >= 0 && nowCockpit != g_lastCockpit && g_attempted) {
            g_attempted = false;
            g_liveSince = GetTickCount();   // let the new camera settle first
            VRLOG("CAMERA FOV: camera switched to %s with nothing found -- "
                  "re-arming the search", nowCockpit ? "cockpit" : "exterior");
        }
        g_lastCockpit = nowCockpit;
    }

    if (!g_found.load(std::memory_order_relaxed)) {
        if (GetTickCount() - g_liveSince < kGameplaySettleMs) {
            set_status("gameplay started -- searching shortly");
            return;
        }
        if (!g_attempted) {
            g_attempted = true;
            g_cursor = 0;
            g_stopAt = 0;
            g_sweptBytes.store(0, std::memory_order_relaxed);
            collect_thread_stacks();
            g_sweeping.store(true, std::memory_order_relaxed);
            VRLOG("CAMERA FOV: sweep started (drive camera is live)");
            return;
        }
        set_status("no camera object found (%llu MB swept) -- retries on a "
                   "camera switch or the next visit to gameplay",
                   (unsigned long long)(g_sweptBytes.load(std::memory_order_relaxed) >> 20));
        return;
    }

    write_fov();
    set_status("writing %.1f deg into %d camera(s)",
               xr::render_hfov_deg() * g_factor.load(std::memory_order_relaxed),
               g_found.load(std::memory_order_relaxed));
}

void camera_fov_reassert()
{
    if (g_sweeping.load(std::memory_order_relaxed)) return;
    // Same gate as Present: outside gameplay the object may already be freed,
    // and writing through it would only fault its way to a re-search we cannot
    // satisfy yet.
    if (!hooks::drive_camera_live()) return;
    write_fov();
}

} // namespace hooks
