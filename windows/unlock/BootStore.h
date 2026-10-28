#pragma once
#include "md/unlock/BootRecord.h"
#include <windows.h>
#include <filesystem>

namespace md::unlock {
std::filesystem::path bootDataDir();
bool secureBootDirectory(const std::filesystem::path& path);
bool writeBootRecord(const BootRecord& record);
std::optional<BootRecord> readBootRecord(const std::filesystem::path& path);
bool eraseBootRecord(const std::string& sid);
bool bootEnrollmentRevoked(const std::wstring& sid);
// Temporary handoff to the UAC helper, protected for the machine AND ACL-limited
// to this user, SYSTEM and administrators. Contains no plaintext Windows password.
std::optional<std::filesystem::path> stageBootRecord(const BootRecord& record, const std::filesystem::path& directory);
std::optional<Bytes> protectBootBytes(ByteView plaintext);
std::optional<Bytes> unprotectBootBytes(ByteView ciphertext);
// CP and normal-user control clients authenticate the service against SCM's PID.
bool isBootServiceProcess(ULONG pid);
bool isBootServicePipe(HANDLE pipe);
std::optional<nlohmann::json> bootControl(const std::wstring& sid, const nlohmann::json& request);
}
