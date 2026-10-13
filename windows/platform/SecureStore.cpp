#include "SecureStore.h"

#include <windows.h>
#include <dpapi.h>

namespace md::platform {

namespace {
const char kEntropy[] = "MediaDuet-pairing-v1";
}

std::optional<Bytes> protect(ByteView plaintext) {
    DATA_BLOB in{static_cast<DWORD>(plaintext.size()), const_cast<BYTE*>(plaintext.data())};
    DATA_BLOB ent{sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"MediaDuet pairing key", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    Bytes b(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return b;
}

std::optional<Bytes> unprotect(ByteView blob) {
    DATA_BLOB in{static_cast<DWORD>(blob.size()), const_cast<BYTE*>(blob.data())};
    DATA_BLOB ent{sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    Bytes b(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b;
}

} // namespace md::platform
