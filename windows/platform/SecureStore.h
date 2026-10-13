#pragma once

#include "md/util/Bytes.h"

#include <optional>

namespace md::platform {

// DPAPI (CryptProtectData) bound to the current Windows user, with an
// application-specific entropy value. Used for pairing keys at rest.
std::optional<Bytes> protect(ByteView plaintext);
std::optional<Bytes> unprotect(ByteView blob);

} // namespace md::platform
