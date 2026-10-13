// Playback helpers for the UI (plain C++, no WinRT: unit-tested headless, see tests/).
//
// The lyric clock itself is the host's md::lyrics::LyricsClock (see LyricTiming.h); this header
// holds the UI-only optimistic state:
// SeekHold       - keeps an optimistic seek target until the host confirms it, so a state report
//                  that was already in flight does not throw the progress bar back for a moment.
// OptimisticFlag - optimistic play/pause: shows the requested state until the host confirms it or
//                  the request times out (rollback).
// FormatOffsetSeconds - "+0.3", "\x2212" "0.25", "0" for the lyrics offset control.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mdui
{
    class SeekHold
    {
    public:
        static constexpr double kConfirmToleranceMs = 1200.0;
        static constexpr double kTimeoutMs = 3000.0;

        void Begin(double targetMs, double nowMs)
        {
            m_active = true;
            m_target = targetMs;
            m_start = nowMs;
        }
        void Clear() { m_active = false; }
        bool Active() const { return m_active; }

        // Where the seek target is now (it keeps running while playing).
        double Expected(double nowMs, bool playing, double rate) const
        {
            return m_target + (playing ? std::max(0.0, nowMs - m_start) * (rate > 0 ? rate : 1.0) : 0.0);
        }

        // Called with the position the host reports (extrapolated to nowMs). Returns true while the
        // report should be replaced by Expected(); confirms or expires the hold otherwise.
        bool Hold(double hostPositionMs, double nowMs, bool playing, double rate)
        {
            if (!m_active) return false;
            if (nowMs - m_start > kTimeoutMs || hostPositionMs < 0)
            {
                m_active = false;
                return false;
            }
            if (std::abs(hostPositionMs - Expected(nowMs, playing, rate)) <= kConfirmToleranceMs)
            {
                m_active = false;   // the host has applied the seek
                return false;
            }
            return true;
        }

    private:
        bool m_active{ false };
        double m_target{ 0 };
        double m_start{ 0 };
    };

    class OptimisticFlag
    {
    public:
        static constexpr double kTimeoutMs = 2500.0;

        void Begin(bool wanted, double nowMs)
        {
            m_active = true;
            m_wanted = wanted;
            m_until = nowMs + kTimeoutMs;
        }
        void Clear() { m_active = false; }
        bool Active() const { return m_active; }
        double Deadline() const { return m_until; }

        // The value to show given what the host reports. Confirmation or timeout ends the override.
        bool Resolve(bool hostValue, double nowMs)
        {
            if (!m_active) return hostValue;
            if (hostValue == m_wanted || nowMs >= m_until)
            {
                m_active = false;
                return hostValue;
            }
            return m_wanted;
        }

    private:
        bool m_active{ false };
        bool m_wanted{ false };
        double m_until{ 0 };
    };

    // Signed seconds for the lyrics offset ("+0.3", "\x2212" "1.25", "0"): one decimal for whole
    // tenths, two otherwise.
    inline std::wstring FormatOffsetSeconds(int64_t ms)
    {
        if (ms == 0) return L"0";
        int64_t a = ms < 0 ? -ms : ms;
        wchar_t buf[32];
        if (a % 100 == 0) swprintf(buf, 32, L"%lld.%lld", static_cast<long long>(a / 1000), static_cast<long long>((a % 1000) / 100));
        else swprintf(buf, 32, L"%lld.%02lld", static_cast<long long>(a / 1000), static_cast<long long>(((a % 1000) + 5) / 10 % 100));
        // Rounding 0.995 s up must carry into the seconds.
        if (a % 100 != 0 && ((a % 1000) + 5) / 10 >= 100) swprintf(buf, 32, L"%lld.00", static_cast<long long>(a / 1000 + 1));
        return std::wstring(ms < 0 ? L"\x2212" : L"+") + buf;
    }
}
