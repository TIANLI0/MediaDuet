#include "NeteaseMemory.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace md::platform {

namespace netease = media::netease;

namespace {

bool readRaw(HANDLE h, uint64_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

template <class T>
std::optional<T> readAs(HANDLE h, uint64_t addr) {
    T v{};
    if (!readRaw(h, addr, &v, sizeof v)) return std::nullopt;
    return v;
}

struct Module {
    uint64_t base = 0, size = 0;
};

std::optional<Module> findModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
    MODULEENTRY32W me{sizeof me};
    std::optional<Module> r;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, name) == 0) {
            r = Module{reinterpret_cast<uint64_t>(me.modBaseAddr), me.modBaseSize};
            break;
        }
    }
    CloseHandle(snap);
    return r;
}

// .text of the module as [address, size).
std::optional<std::pair<uint64_t, uint32_t>> textSection(HANDLE h, const Module& m) {
    auto peOff = readAs<uint32_t>(h, m.base + 0x3C);
    if (!peOff || *peOff > 0x1000) return std::nullopt;
    uint64_t pe = m.base + *peOff;
    auto count = readAs<uint16_t>(h, pe + 6);
    auto optSize = readAs<uint16_t>(h, pe + 20);
    if (!count || !optSize) return std::nullopt;
    for (uint16_t i = 0; i < *count && i < 96; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!readRaw(h, pe + 24 + *optSize + i * sizeof sh, &sh, sizeof sh)) return std::nullopt;
        if (std::memcmp(sh.Name, ".text", 6) == 0) {
            if (sh.VirtualAddress + static_cast<uint64_t>(sh.Misc.VirtualSize) > m.size) return std::nullopt;
            return std::make_pair(m.base + sh.VirtualAddress, static_cast<uint32_t>(sh.Misc.VirtualSize));
        }
    }
    return std::nullopt;
}

// Address of the first match of `sig` in the code, resolved to the static it addresses.
std::optional<uint64_t> resolve(HANDLE h, uint64_t text, uint32_t size, const netease::Signature& sig) {
    netease::BytePattern p(sig.pattern);
    if (p.size() == 0) return std::nullopt;
    // 1 MiB windows overlapping by the pattern length, so the host never holds the whole 27 MB.
    constexpr uint32_t kChunk = 1u << 20;
    std::vector<uint8_t> buf(kChunk + p.size());
    for (uint32_t off = 0; off < size; off += kChunk) {
        uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(kChunk + p.size() - 1, size - off));
        if (!readRaw(h, text + off, buf.data(), n)) return std::nullopt;
        if (auto at = p.find(buf.data(), n)) {
            int32_t disp = 0;
            std::memcpy(&disp, buf.data() + *at + sig.dispOffset, sizeof disp);
            return netease::ripTarget(text + off + *at + sig.dispOffset, disp);
        }
    }
    return std::nullopt;
}

} // namespace

std::unique_ptr<NeteaseMemory> NeteaseMemory::open(DWORD pid, std::string* why) {
    auto fail = [&](const char* reason) -> std::unique_ptr<NeteaseMemory> {
        if (why) *why = reason;
        return nullptr;
    };
    auto mod = findModule(pid, L"cloudmusic.dll");
    if (!mod) return fail("cloudmusic.dll not loaded");
    HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!h) return fail("cannot open the process for reading");
    auto text = textSection(h, *mod);
    auto player = text ? resolve(h, text->first, text->second, netease::kPlayerSignature) : std::nullopt;
    auto schedule = text ? resolve(h, text->first, text->second, netease::kScheduleSignature) : std::nullopt;
    auto inModule = [&](std::optional<uint64_t> a) { return a && *a >= mod->base && *a < mod->base + mod->size; };
    if (!inModule(player) || !inModule(schedule)) {
        CloseHandle(h);
        return fail("player not found in this NetEase build");
    }
    std::unique_ptr<NeteaseMemory> m(new NeteaseMemory(h, *player, *schedule));
    if (!m->read()) return fail("player state not readable in this NetEase build");
    return m;
}

NeteaseMemory::~NeteaseMemory() { CloseHandle(process_); }

std::optional<netease::PlayerReading> NeteaseMemory::read() const {
    netease::PlayerReading r;
    auto status = readAs<int32_t>(process_, player_ + netease::kStatusOffset);
    auto duration = readAs<double>(process_, player_ + netease::kDurationOffset);
    auto position = readAs<double>(process_, schedule_);
    auto info = readAs<uint64_t>(process_, player_ + netease::kPlayInfoOffset);
    if (!status || !duration || !position || !info) return std::nullopt;
    r.status = *status;
    r.durationSec = *duration;
    r.positionSec = *position;
    if (*info) {
        // MSVC std::string: 16-byte inline buffer or pointer, then size and capacity.
        uint64_t str = *info + netease::kPlayInfoIdOffset;
        auto len = readAs<uint64_t>(process_, str + 16);
        auto cap = readAs<uint64_t>(process_, str + 24);
        if (len && cap && *len <= netease::kMaxIdLength && *len <= *cap) {
            char raw[netease::kMaxIdLength] = {};
            uint64_t from = str;
            if (*cap > 15) {
                auto p = readAs<uint64_t>(process_, str);
                from = p ? *p : 0;
            }
            if (from && readRaw(process_, from, raw, static_cast<size_t>(*len)))
                r.songId = netease::parseSongId(std::string_view(raw, static_cast<size_t>(*len)));
        }
    }
    if (!netease::plausible(r)) return std::nullopt;
    return r;
}

} // namespace md::platform
