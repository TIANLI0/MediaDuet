#include "Diagnostics.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace md::app {

static uint64_t ft(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

Diagnostics::Sample Diagnostics::sample() {
    Sample s;
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    uint64_t cpu = ft(k) + ft(u), wall = ft(now);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (lastWall_ && wall > lastWall_)
        s.cpuPercent = 100.0 * static_cast<double>(cpu - lastCpu_) / static_cast<double>(wall - lastWall_) /
                       static_cast<double>(si.dwNumberOfProcessors);
    lastCpu_ = cpu;
    lastWall_ = wall;
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.workingSetMB = pmc.WorkingSetSize / 1048576.0;
        s.privateMB = pmc.PrivateUsage / 1048576.0;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof(te)};
        DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid) ++s.threads;
        CloseHandle(snap);
    }
    return s;
}

} // namespace md::app
