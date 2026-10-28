// Exercise the production COM implementation without registering a provider, opening
// a real unlock pipe, changing the session lock, or handling any real credentials.
#define MD_UNLOCK_TESTING
#include "../credential/Provider.cpp"
#include <doctest/doctest.h>

namespace {
struct CredentialTestAccess {
    static void receive(Credential& c, const json& j) { c.receive(j); }
    static const std::wstring& password(Credential& c) { return c.password_; }
    static size_t queued(Credential& c) { return c.queue_.size(); }
};
struct ProviderTestAccess {
    static void add(Provider& provider, const ComPtr<Credential>& credential) { provider.credentials_.push_back(credential); }
};
ComPtr<Credential> credential(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario = CPUS_UNLOCK_WORKSTATION) {
    ComPtr<Credential> c;
    c.Attach(new Credential(L"S-1-5-21-1-2-3-1001", L"DESKTOP\\tester", scenario, [] {}));
    return c;
}
void select(Credential& c) { BOOL automatic = TRUE; REQUIRE(c.SetSelected(&automatic) == S_OK); CHECK_FALSE(automatic); }
}

TEST_CASE("unlock provider: only enrolled user status exposes a credential") {
    auto c = credential();
    CHECK_FALSE(c->visible());
    CredentialTestAccess::receive(*c.Get(), {{"type", "status"}, {"state", "ready"}, {"lang", "zh-CN"}});
    CHECK(c->visible());
    PWSTR label = nullptr;
    REQUIRE(c->GetStringValue(Resend, &label) == S_OK);
    CHECK(std::wstring(label) == L"重新发送到手机"); CoTaskMemFree(label);
    for (auto state : {"not_enrolled", "other_user"}) {
        CredentialTestAccess::receive(*c.Get(), {{"type", "status"}, {"state", state}});
        CHECK_FALSE(c->visible());
    }
}

TEST_CASE("unlock provider: a credential is accepted only while selected and cleared when deselected") {
    auto c = credential();
    json reply{{"type", "credential"}, {"password", "test-only-password"}};
    CredentialTestAccess::receive(*c.Get(), reply);
    CHECK(CredentialTestAccess::password(*c.Get()).empty());
    select(*c.Get());
    CredentialTestAccess::receive(*c.Get(), reply);
    CHECK(c->ready());
    c->SetDeselected();
    CHECK_FALSE(c->ready());
    CHECK(CredentialTestAccess::password(*c.Get()).empty());
    CHECK(CredentialTestAccess::queued(*c.Get()) == 2);
}

TEST_CASE("unlock provider: unlock serialization uses bounded relative UTF-16 strings and is one-shot") {
    auto c = credential(); select(*c.Get());
    CredentialTestAccess::receive(*c.Get(), {{"type", "credential"}, {"password", "测试 pa$$wörd"}});
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serial{};
    PWSTR text = nullptr; CREDENTIAL_PROVIDER_STATUS_ICON icon;
    REQUIRE(c->GetSerialization(&response, &serial, &text, &icon) == S_OK);
    REQUIRE(response == CPGSR_RETURN_CREDENTIAL_FINISHED);
    CHECK(serial.clsidCredentialProvider == kClsid);
    REQUIRE(serial.rgbSerialization != nullptr);
    auto logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(serial.rgbSerialization);
    CHECK(logon->Logon.MessageType == KerbWorkstationUnlockLogon);
    auto unpack = [&](UNICODE_STRING value) {
        const auto offset = reinterpret_cast<size_t>(value.Buffer);
        REQUIRE(offset >= sizeof(*logon));
        REQUIRE(offset + value.Length <= serial.cbSerialization);
        return std::wstring(reinterpret_cast<wchar_t*>(serial.rgbSerialization + offset), value.Length / 2);
    };
    CHECK(unpack(logon->Logon.LogonDomainName) == L"DESKTOP");
    CHECK(unpack(logon->Logon.UserName) == L"tester");
    CHECK(unpack(logon->Logon.Password) == L"测试 pa$$wörd");
    SecureZeroMemory(serial.rgbSerialization, serial.cbSerialization); CoTaskMemFree(serial.rgbSerialization);
    CHECK(CredentialTestAccess::password(*c.Get()).empty());
    REQUIRE(c->GetSerialization(&response, &serial, &text, &icon) == S_OK);
    CHECK(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED);
    CHECK(serial.rgbSerialization == nullptr);
}

TEST_CASE("unlock provider: stale and failed statuses discard a pending password") {
    for (auto state : {"stale", "failed", "denied", "not_enrolled", "offline"}) {
        auto c = credential(); select(*c.Get());
        CredentialTestAccess::receive(*c.Get(), {{"type", "credential"}, {"password", "test"}});
        CredentialTestAccess::receive(*c.Get(), {{"type", "status"}, {"state", state}});
        CHECK_FALSE(c->ready());
    }
}

TEST_CASE("unlock provider: unexpected input never produces an automatic credential") {
    auto c = credential(); select(*c.Get());
    for (auto password : {std::string(), std::string(2049, 'x'), std::string("x\0y", 3)}) {
        CredentialTestAccess::receive(*c.Get(), {{"type", "credential"}, {"password", password}});
        CHECK_FALSE(c->ready());
    }
    CredentialTestAccess::receive(*c.Get(), {{"type", "untrusted-message"}, {"password", "test"}});
    CHECK_FALSE(c->ready());
}

TEST_CASE("unlock provider: COM factory rejects aggregation and unsupported scenarios") {
    ComPtr<ICredentialProvider> provider;
    REQUIRE(DllGetClassObject(kClsid, IID_PPV_ARGS(&provider)) == E_NOINTERFACE);
    ComPtr<IClassFactory> factory;
    REQUIRE(DllGetClassObject(kClsid, IID_PPV_ARGS(&factory)) == S_OK);
    REQUIRE(factory->CreateInstance(nullptr, IID_PPV_ARGS(&provider)) == S_OK);
    CHECK(provider->SetUsageScenario(CPUS_CREDUI, 0) == E_NOTIMPL);
    CHECK(provider->SetUsageScenario(CPUS_CHANGE_PASSWORD, 0) == E_NOTIMPL);
    CHECK(provider->SetUsageScenario(CPUS_UNLOCK_WORKSTATION, 0) == S_OK);
    DWORD count, index; BOOL automatic;
    REQUIRE(provider->GetCredentialCount(&count, &index, &automatic) == S_OK);
    CHECK(count == 0); CHECK_FALSE(automatic);
    CHECK(index == CREDENTIAL_PROVIDER_NO_DEFAULT);
    CHECK(DllCanUnloadNow() == S_FALSE);
    provider.Reset(); factory.Reset();
    CHECK(DllCanUnloadNow() == S_OK);
}

TEST_CASE("unlock provider: pipe server must have the tile user's SID and session") {
    CHECK_FALSE(trustedServer(INVALID_HANDLE_VALUE, 0, L"S-1-5-18"));
    auto name = L"\\\\.\\pipe\\MediaDuet.Unlock.Test." + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, 4096, 4096, 0, nullptr);
    REQUIRE(server != INVALID_HANDLE_VALUE);
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    REQUIRE(client != INVALID_HANDLE_VALUE);
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> bytes(n);
    REQUIRE(GetTokenInformation(token, TokenUser, bytes.data(), n, &n));
    PWSTR sid = nullptr;
    REQUIRE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid));
    DWORD session = 0; REQUIRE(ProcessIdToSessionId(GetCurrentProcessId(), &session));
    CHECK(trustedServer(client, session, sid));
    CHECK_FALSE(trustedServer(client, session + 1, sid));
    CHECK_FALSE(trustedServer(client, session, L"S-1-5-21-1-2-3-9999"));
    LocalFree(sid); CloseHandle(token); CloseHandle(client); CloseHandle(server);
}

TEST_CASE("unlock provider: default focus never bypasses phone approval") {
    ComPtr<Provider> provider; provider.Attach(new Provider);
    auto c = credential();
    CredentialTestAccess::receive(*c.Get(), {{"type", "status"}, {"state", "ready"}});
    ProviderTestAccess::add(*provider.Get(), c);
    DWORD count, index; BOOL automatic;
    REQUIRE(provider->GetCredentialCount(&count, &index, &automatic) == S_OK);
    CHECK(count == 1); CHECK(index == 0); CHECK_FALSE(automatic);
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serial{};
    PWSTR text = nullptr; CREDENTIAL_PROVIDER_STATUS_ICON icon;
    REQUIRE(c->GetSerialization(&response, &serial, &text, &icon) == S_OK);
    CHECK(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED); CHECK(serial.rgbSerialization == nullptr);
    select(*c.Get());
    CredentialTestAccess::receive(*c.Get(), {{"type", "credential"}, {"password", "synthetic"}});
    REQUIRE(provider->GetCredentialCount(&count, &index, &automatic) == S_OK);
    CHECK(index == 0); CHECK(automatic);
}

TEST_CASE("unlock provider: unified logon scenario protects the local account password") {
    auto c = credential(CPUS_LOGON); select(*c.Get());
    CredentialTestAccess::receive(*c.Get(), {{"type", "credential"}, {"password", "synthetic-password"}});
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serial{};
    PWSTR text = nullptr; CREDENTIAL_PROVIDER_STATUS_ICON icon;
    REQUIRE(c->GetSerialization(&response, &serial, &text, &icon) == S_OK);
    REQUIRE(response == CPGSR_RETURN_CREDENTIAL_FINISHED);
    auto logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(serial.rgbSerialization);
    CHECK(logon->Logon.MessageType == KerbInteractiveLogon);
    auto value = logon->Logon.Password;
    auto p = reinterpret_cast<wchar_t*>(serial.rgbSerialization + reinterpret_cast<size_t>(value.Buffer));
    std::wstring password(p, value.Length / 2);
    CHECK(password != L"synthetic-password");
    DWORD count = 0;
    CredUnprotectW(FALSE, password.data(), static_cast<DWORD>(password.size() + 1), nullptr, &count);
    REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    std::wstring plain(count, 0);
    REQUIRE(CredUnprotectW(FALSE, password.data(), static_cast<DWORD>(password.size() + 1), plain.data(), &count));
    CHECK(std::wstring(plain.c_str()) == L"synthetic-password");
    wipe(plain); wipe(password);
    SecureZeroMemory(serial.rgbSerialization, serial.cbSerialization); CoTaskMemFree(serial.rgbSerialization);
}
