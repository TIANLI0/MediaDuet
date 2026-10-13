#pragma once

// doctest stringification for project enums (used in failure messages).

#include "md/link/Transport.h"
#include "md/media/MediaTypes.h"
#include "md/policy/PlaybackArbiter.h"

#include <doctest/doctest.h>

#include <optional>

namespace doctest {
template <>
struct StringMaker<md::policy::PState> {
    static String convert(md::policy::PState v) { return md::policy::toString(v); }
};
template <>
struct StringMaker<md::media::CommandAction> {
    static String convert(md::media::CommandAction v) { return md::media::toString(v); }
};
template <>
struct StringMaker<md::media::PlaybackStatus> {
    static String convert(md::media::PlaybackStatus v) { return md::media::toString(v); }
};
template <>
struct StringMaker<md::media::Origin> {
    static String convert(md::media::Origin v) { return md::media::toString(v); }
};
template <>
struct StringMaker<md::link::TransportKind> {
    static String convert(md::link::TransportKind v) { return md::link::toString(v); }
};
template <>
struct StringMaker<std::optional<md::link::TransportKind>> {
    static String convert(const std::optional<md::link::TransportKind>& v) {
        return v ? md::link::toString(*v) : "none";
    }
};
} // namespace doctest
