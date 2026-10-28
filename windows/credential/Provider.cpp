// A v2 credential provider. All LogonUI/COM callbacks run on the creating apartment;
// the pipe worker posts messages to a message-only window and never calls UI interfaces.
#include <windows.h>
#include <credentialprovider.h>
#include <ntsecapi.h>
#include <wincred.h>
#include <sddl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <propkey.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include "md/unlock/Component.h"
#include "BootStore.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using nlohmann::json;
namespace {
const CLSID kClsid = {0x91835fe0, 0x2b7c, 0x453b, {0x92, 0x6f, 0xb6, 0x77, 0x13, 0xf7, 0xdf, 0x99}};
std::atomic<long> objects{0};
HINSTANCE instance;
constexpr UINT kPipeMessage = WM_APP + 73;
enum Field : DWORD { Title, Status, Resend, Submit, TileImage, FieldCount };
const CREDENTIAL_PROVIDER_FIELD_TYPE fieldTypes[] = {CPFT_LARGE_TEXT, CPFT_SMALL_TEXT, CPFT_COMMAND_LINK, CPFT_SUBMIT_BUTTON, CPFT_TILE_IMAGE};
const wchar_t* fieldLabels[] = {L"MediaDuet", L"", L"Send to phone again", L"Unlock", L"MediaDuet"};

template<class T> void wipe(T& s) { SecureZeroMemory(s.data(), s.size() * sizeof(typename T::value_type)); s.clear(); }
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
bool registered(const std::wstring& sid) {
    HKEY key = nullptr;
    auto path = std::wstring(md::unlock::component::kUsersKey) + L"\\" + sid;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key)) return false;
    RegCloseKey(key); return true;
}

// Authenticate the process holding the server end before sending hello or accepting secrets.
bool trustedServer(HANDLE pipe, DWORD session, const std::wstring& sidText) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    bool ok = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        DWORD n = 0, ts = ~0u;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> b(n);
        PSID sid = nullptr;
        if (GetTokenInformation(token, TokenUser, b.data(), n, &n) &&
            GetTokenInformation(token, TokenSessionId, &ts, sizeof(ts), &n) && ts == session &&
            ConvertStringSidToSidW(sidText.c_str(), &sid)) {
            ok = EqualSid(sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid) != FALSE;
            LocalFree(sid);
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return ok;
}

class Credential final : public ICredentialProviderCredential2 {
public:
    Credential(std::wstring sid, std::wstring user, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
               std::function<void()> changed) : sid_(std::move(sid)), user_(std::move(user)), scenario_(scenario), changed_(std::move(changed)) {
        ++objects;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        WNDCLASSW wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = instance; wc.lpszClassName = L"MediaDuet.Unlock.Events";
        RegisterClassW(&wc);
        window_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
#ifndef MD_UNLOCK_TESTING
        if (window_ && stop_) worker_ = std::thread([this] { run(); });
#endif
    }
    ~Credential() {
        stop();
        if (events_) events_->Release();
        if (window_) {
            MSG msg;
            while (PeekMessageW(&msg, window_, kPipeMessage, kPipeMessage, PM_REMOVE)) {
                auto p = reinterpret_cast<json*>(msg.lParam);
                if (p->contains("password") && (*p)["password"].is_string()) wipe((*p)["password"].get_ref<std::string&>());
                delete p;
            }
            DestroyWindow(window_);
        }
        if (stop_) CloseHandle(stop_);
        wipe(password_); --objects;
    }
    void stop() { changed_ = {}; SetEvent(stop_); if (worker_.joinable()) worker_.join(); }
    bool visible() const { return visible_; }
    bool ready() const { return selected_ && !password_.empty(); }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ICredentialProviderCredential || iid == IID_ICredentialProviderCredential2)
            *out = static_cast<ICredentialProviderCredential2*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { auto n = --refs_; if (!n) delete this; return n; }
    IFACEMETHODIMP Advise(ICredentialProviderCredentialEvents* e) override {
        if (events_) events_->Release(); events_ = e; if (e) e->AddRef(); return S_OK;
    }
    IFACEMETHODIMP UnAdvise() override { if (events_) events_->Release(); events_ = nullptr; return S_OK; }
    IFACEMETHODIMP SetSelected(BOOL* automatic) override {
        *automatic = ready() ? TRUE : FALSE;
        if (!selected_) { selected_ = true; send({{"type", "request"}}); }
        return S_OK;
    }
    IFACEMETHODIMP SetDeselected() override {
        selected_ = false; wipe(password_); send({{"type", "cancel"}}); return S_OK;
    }
    IFACEMETHODIMP GetFieldState(DWORD id, CREDENTIAL_PROVIDER_FIELD_STATE* state,
                                 CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* interactive) override {
        if (id >= FieldCount) return E_INVALIDARG;
        *state = id == Title || id == TileImage ? CPFS_DISPLAY_IN_BOTH : CPFS_DISPLAY_IN_SELECTED_TILE;
        *interactive = CPFIS_NONE; return S_OK;
    }
    IFACEMETHODIMP GetStringValue(DWORD id, PWSTR* out) override {
        if (id >= FieldCount) return E_INVALIDARG;
        const wchar_t* value = fieldLabels[id];
        if (id == Status) value = status_.c_str();
        if (id == Resend && zh_) value = L"重新发送到手机";
        if (id == Submit && zh_) value = L"解锁";
        return SHStrDupW(value, out);
    }
    IFACEMETHODIMP GetBitmapValue(DWORD id, HBITMAP* out) override {
        *out = nullptr;
        if (id != TileImage) return E_INVALIDARG;
        HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 128, 128, 0));
        if (!icon) return HRESULT_FROM_WIN32(GetLastError());
        ICONINFO info{};
        bool ok = GetIconInfo(icon, &info) != FALSE;
        DestroyIcon(icon);
        if (!ok) return E_FAIL;
        if (info.hbmMask) DeleteObject(info.hbmMask);
        *out = info.hbmColor; return *out ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP GetCheckboxValue(DWORD, BOOL*, PWSTR*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetSubmitButtonValue(DWORD id, DWORD* adjacent) override {
        if (id != Submit) return E_INVALIDARG; *adjacent = Status; return S_OK;
    }
    IFACEMETHODIMP GetComboBoxValueCount(DWORD, DWORD*, DWORD*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetComboBoxValueAt(DWORD, DWORD, PWSTR*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(DWORD, PCWSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetCheckboxValue(DWORD, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetComboBoxSelectedValue(DWORD, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP CommandLinkClicked(DWORD id) override {
        if (id != Resend) return E_INVALIDARG;
        wipe(password_); send({{"type", "request"}}); return S_OK;
    }
    IFACEMETHODIMP GetUserSid(PWSTR* out) override { return SHStrDupW(sid_.c_str(), out); }
    IFACEMETHODIMP GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* response,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serial, PWSTR* text, CREDENTIAL_PROVIDER_STATUS_ICON* icon) override {
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED; *text = nullptr; *icon = CPSI_NONE; *serial = {};
        if (password_.empty()) { send({{"type", "request"}}); return S_OK; }
        HRESULT hr = pack(serial);
        wipe(password_);
        if (SUCCEEDED(hr)) { *response = CPGSR_RETURN_CREDENTIAL_FINISHED; serial->clsidCredentialProvider = kClsid; }
        return hr;
    }
    IFACEMETHODIMP ReportResult(NTSTATUS status, NTSTATUS substatus, PWSTR* text,
                                 CREDENTIAL_PROVIDER_STATUS_ICON* icon) override {
        *text = nullptr; *icon = CPSI_NONE;
        bool ok = status >= 0;
        const auto primary = static_cast<uint32_t>(status);
        auto failure = primary == 0xc000006d || primary == 0xc000006a ? status : substatus ? substatus : status;
        send({{"type", "result"}, {"ok", ok}, {"status", static_cast<int64_t>(failure)}});
        wipe(password_);
        return S_OK;
    }
private:
    friend struct CredentialTestAccess;
    HRESULT pack(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serial) {
        HANDLE lsa = nullptr;
        if (LsaConnectUntrusted(&lsa) != 0) return E_FAIL;
        char name[] = "Negotiate";
        LSA_STRING package{static_cast<USHORT>(sizeof(name) - 1), static_cast<USHORT>(sizeof(name)), name};
        ULONG auth = 0;
        NTSTATUS st = LsaLookupAuthenticationPackage(lsa, &package, &auth);
        LsaDeregisterLogonProcess(lsa);
        if (st != 0) return HRESULT_FROM_WIN32(LsaNtStatusToWinError(st));
        serial->ulAuthenticationPackage = auth;

        // Identity-provider accounts need their qualified identity packed by CredUI.
        if (user_.starts_with(L"MicrosoftAccount\\") || user_.starts_with(L"AzureAD\\")) {
            DWORD size = 0;
            DWORD flags = CRED_PACK_PROTECTED_CREDENTIALS | CRED_PACK_ID_PROVIDER_CREDENTIALS;
            CredPackAuthenticationBufferW(flags, user_.data(), password_.data(), nullptr, &size);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return E_FAIL;
            serial->rgbSerialization = static_cast<BYTE*>(CoTaskMemAlloc(size));
            if (!serial->rgbSerialization) return E_OUTOFMEMORY;
            if (!CredPackAuthenticationBufferW(flags, user_.data(), password_.data(), serial->rgbSerialization, &size)) {
                SecureZeroMemory(serial->rgbSerialization, size); CoTaskMemFree(serial->rgbSerialization);
                serial->rgbSerialization = nullptr; return HRESULT_FROM_WIN32(GetLastError());
            }
            serial->cbSerialization = size; return S_OK;
        }
        auto slash = user_.find(L'\\');
        if (slash == std::wstring::npos) return E_INVALIDARG;
        std::wstring domain = user_.substr(0, slash), user = user_.substr(slash + 1);
        std::wstring protectedPassword;
        if (scenario_ == CPUS_LOGON) {
            DWORD count = 0;
            CRED_PROTECTION_TYPE protection;
            CredProtectW(FALSE, password_.data(), static_cast<DWORD>(password_.size() + 1), nullptr, &count, &protection);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return E_FAIL;
            protectedPassword.resize(count);
            if (!CredProtectW(FALSE, password_.data(), static_cast<DWORD>(password_.size() + 1),
                              protectedPassword.data(), &count, &protection)) { wipe(protectedPassword); return E_FAIL; }
            if (!protectedPassword.empty() && protectedPassword.back() == 0) protectedPassword.pop_back();
        }
        const auto& packedPassword = scenario_ == CPUS_LOGON ? protectedPassword : password_;
        // The native unlock buffer uses offsets into the allocation, never process pointers.
        const size_t bytes = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) +
            (domain.size() + user.size() + packedPassword.size()) * sizeof(wchar_t);
        if (domain.size() > 32767 || user.size() > 32767 || packedPassword.size() > 32767) { wipe(protectedPassword); return E_INVALIDARG; }
        auto buffer = static_cast<BYTE*>(CoTaskMemAlloc(bytes));
        if (!buffer) { wipe(protectedPassword); return E_OUTOFMEMORY; }
        ZeroMemory(buffer, bytes);
        auto logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(buffer);
        logon->Logon.MessageType = scenario_ == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon;
        size_t offset = sizeof(*logon);
        auto put = [&](UNICODE_STRING& dest, const std::wstring& value) {
            dest.Length = dest.MaximumLength = static_cast<USHORT>(value.size() * sizeof(wchar_t));
            memcpy(buffer + offset, value.data(), dest.Length);
            dest.Buffer = reinterpret_cast<PWSTR>(offset); offset += dest.Length;
        };
        put(logon->Logon.LogonDomainName, domain); put(logon->Logon.UserName, user); put(logon->Logon.Password, packedPassword);
        wipe(protectedPassword);
        serial->rgbSerialization = buffer; serial->cbSerialization = static_cast<ULONG>(bytes);
        return S_OK;
    }
    static LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Credential*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Credential*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == kPipeMessage && self) {
            std::unique_ptr<json> data(reinterpret_cast<json*>(lp));
            try { self->receive(*data); } catch (...) { /* Malformed host response. */ }
            if (data->contains("password") && (*data)["password"].is_string()) wipe((*data)["password"].get_ref<std::string&>());
            return 0;
        }
        if (msg == WM_TIMER && wp == 1 && self) {
            KillTimer(window, 1); wipe(self->password_); return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }
    void receive(const json& j) {
        const auto type = j.value("type", std::string());
        if (type == "reconnected") { if (selected_) send({{"type", "request"}}); return; }
        if (type == "credential") {
            if (!selected_) return;
            auto s = j.value("password", std::string());
            if (s.size() <= 2048 && s.find('\0') == std::string::npos) password_ = wide(s);
            wipe(s);
            SetTimer(window_, 1, 15000, nullptr);
            if (changed_) changed_();
            return;
        }
        if (type != "status") return;
        auto state = j.value("state", std::string());
        bool wasVisible = visible_;
        visible_ = state == "offline" || state == "ready" || state == "waiting" || state == "denied" ||
                   state == "failed" || state == "stale" || state == "unlocking";
        zh_ = j.value("lang", std::string()) == "zh-CN";
        auto phone = wide(j.value("phone", std::string()));
        if (state == "waiting") status_ = zh_ ? L"请在手机上确认解锁" : L"Confirm the unlock on your phone";
        else if (state == "offline") status_ = zh_ ? L"等待手机连接…" : L"Waiting for your phone to connect…";
        else if (state == "denied") status_ = zh_ ? L"已取消。可以重新发送到手机。" : L"Cancelled. You can send the request again.";
        else if (state == "stale") status_ = zh_ ? L"请在 MediaDuet 中重新设置手机解锁。" : L"Set up phone unlock again in MediaDuet.";
        else if (state == "unlocking") status_ = zh_ ? L"正在解锁…" : L"Unlocking…";
        else if (state == "failed") {
            auto code = j.value("code", std::string());
            status_ = code == "timeout" ? (zh_ ? L"等待确认超时，请重试。" : L"Confirmation timed out. Try again.") :
                (zh_ ? L"无法解锁，请重试或使用其他登录方式。" : L"Unable to unlock. Try again or use another sign-in option.");
        } else status_ = (zh_ ? L"使用手机解锁：" : L"Unlock with your phone: ") + phone;
        if (state != "unlocking") wipe(password_);
        if (events_) {
            events_->SetFieldString(this, Status, status_.c_str());
            events_->SetFieldString(this, Resend, zh_ ? L"重新发送到手机" : L"Send to phone again");
            events_->SetFieldString(this, Submit, zh_ ? L"解锁" : L"Unlock");
        }
        if (wasVisible != visible_ && changed_) changed_();
    }
    void send(const json& j) {
        std::lock_guard lock(queueMutex_);
        if (queue_.size() < 16) queue_.push_back(j.dump() + "\n");
    }
    bool io(HANDLE pipe, bool write, char* data, DWORD size, DWORD& transferred) {
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = write ? WriteFile(pipe, data, size, &transferred, &ov) : ReadFile(pipe, data, size, &transferred, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = {stop_, ov.hEvent};
            if (WaitForMultipleObjects(2, waits, FALSE, 1000) != WAIT_OBJECT_0 + 1) CancelIoEx(pipe, &ov);
            ok = GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        }
        CloseHandle(ov.hEvent); return ok != FALSE;
    }
    void publish(json data) {
        auto p = new json(std::move(data));
        if (!PostMessageW(window_, kPipeMessage, 0, reinterpret_cast<LPARAM>(p))) {
            if (p->contains("password") && (*p)["password"].is_string()) wipe((*p)["password"].get_ref<std::string&>());
            delete p;
        }
    }
    void run() {
        DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
        auto name = std::wstring(md::unlock::component::kPipePrefix) + std::to_wstring(session);
        auto bootName = std::wstring(md::unlock::component::kBootPipePrefix) + sid_;
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            bool bootConnection = false;
            if (pipe == INVALID_HANDLE_VALUE) {
                pipe = CreateFileW(bootName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
                bootConnection = pipe != INVALID_HANDLE_VALUE;
            }
            if (pipe == INVALID_HANDLE_VALUE) { if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) break; continue; }
            if (!(bootConnection ? md::unlock::isBootServicePipe(pipe) : trustedServer(pipe, session, sid_))) {
                CloseHandle(pipe); if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) break; continue;
            }
            {
                std::lock_guard lock(queueMutex_);
                queue_.clear();
                queue_.push_back(json{{"type", "hello"}, {"v", 1}, {"sid", utf8(sid_)}}.dump() + "\n");
            }
            // After reconnect the apartment decides whether this tile is still selected.
            publish({{"type", "reconnected"}});
            std::string pending;
            bool connected = true;
            while (connected && WaitForSingleObject(stop_, 50) != WAIT_OBJECT_0) {
                std::vector<std::string> batch;
                { std::lock_guard lock(queueMutex_); batch.swap(queue_); }
                for (auto& line : batch) {
                    DWORD n = 0;
                    if (!io(pipe, true, line.data(), static_cast<DWORD>(line.size()), n) || n != line.size()) { connected = false; break; }
                }
                DWORD available = 0;
                if (!connected || !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) continue;
                char bytes[4096]; DWORD n = 0;
                if (!io(pipe, false, bytes, sizeof(bytes), n) || !n) break;
                pending.append(bytes, n); SecureZeroMemory(bytes, sizeof(bytes));
                size_t pos;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    auto line = pending.substr(0, pos);
                    auto data = json::parse(line, nullptr, false);
                    wipe(line);
                    SecureZeroMemory(pending.data(), pos + 1); pending.erase(0, pos + 1);
                    if (data.is_object()) {
                        // A tray host can start while this boot pipe is open
                        // (including Windows restart sign-in restoration). Retry
                        // the preferred per-session pipe when service gating ends.
                        if (bootConnection && data.contains("type") && data["type"] == "status" &&
                            data.contains("state") && data["state"] == "not_enrolled") connected = false;
                        publish(std::move(data));
                        if (!connected) break;
                    }
                }
                if (pending.size() > 16384) break;
            }
            wipe(pending); CloseHandle(pipe);
            publish({{"type", "status"}, {"state", "not_enrolled"}});
            if (WaitForSingleObject(stop_, bootConnection ? 1000 : 250) == WAIT_OBJECT_0) break;
        }
    }
    std::atomic<ULONG> refs_{1};
    std::wstring sid_, user_, password_, status_;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario_;
    std::function<void()> changed_;
    ICredentialProviderCredentialEvents* events_ = nullptr;
    bool selected_ = false, visible_ = false, zh_ = false;
    HWND window_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::mutex queueMutex_;
    std::vector<std::string> queue_;
};

class Provider final : public ICredentialProvider, public ICredentialProviderSetUserArray {
public:
    Provider() { ++objects; }
    ~Provider() { clear(); --objects; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ICredentialProvider) *out = static_cast<ICredentialProvider*>(this);
        else if (iid == IID_ICredentialProviderSetUserArray) *out = static_cast<ICredentialProviderSetUserArray*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { auto n = --refs_; if (!n) delete this; return n; }
    IFACEMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, DWORD flags) override {
        if ((scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) || (flags & CREDUIWIN_GENERIC)) return E_NOTIMPL;
        scenario_ = scenario; return S_OK;
    }
    IFACEMETHODIMP SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Advise(ICredentialProviderEvents* events, UINT_PTR context) override { events_ = events; context_ = context; return S_OK; }
    IFACEMETHODIMP UnAdvise() override { events_.Reset(); return S_OK; }
    IFACEMETHODIMP GetFieldDescriptorCount(DWORD* count) override { *count = FieldCount; return S_OK; }
    IFACEMETHODIMP GetFieldDescriptorAt(DWORD index, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** out) override {
        *out = nullptr;
        if (index >= FieldCount) return E_INVALIDARG;
        auto f = static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
        if (!f) return E_OUTOFMEMORY;
        *f = {}; f->dwFieldID = index; f->cpft = fieldTypes[index];
        if (index == TileImage) f->guidFieldType = CPFG_CREDENTIAL_PROVIDER_LOGO;
        HRESULT hr = SHStrDupW(fieldLabels[index], &f->pszLabel);
        if (FAILED(hr)) CoTaskMemFree(f); else *out = f;
        return hr;
    }
    IFACEMETHODIMP GetCredentialCount(DWORD* count, DWORD* defaultIndex, BOOL* automatic) override {
        *count = 0; *defaultIndex = CREDENTIAL_PROVIDER_NO_DEFAULT; *automatic = FALSE;
        for (auto& c : credentials_) if (c->visible()) {
            // Supply a focusable default before approval. Windows can then reuse
            // its last successful provider; automatic submission still requires K.
            if (*count == 0) *defaultIndex = 0;
            if (c->ready()) { *defaultIndex = *count; *automatic = TRUE; }
            ++*count;
        }
        return S_OK;
    }
    IFACEMETHODIMP GetCredentialAt(DWORD index, ICredentialProviderCredential** out) override {
        *out = nullptr;
        for (auto& c : credentials_) if (c->visible()) {
            if (!index--) { *out = c.Get(); (*out)->AddRef(); return S_OK; }
        }
        return E_INVALIDARG;
    }
    IFACEMETHODIMP SetUserArray(ICredentialProviderUserArray* users) override {
        clear();
        DWORD count = 0;
        if (!users || FAILED(users->GetCount(&count))) return E_INVALIDARG;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<ICredentialProviderUser> user;
            if (FAILED(users->GetAt(i, &user))) continue;
            PWSTR sid = nullptr, name = nullptr;
            user->GetSid(&sid); user->GetStringValue(PKEY_Identity_QualifiedUserName, &name);
            if (sid && name && registered(sid)) {
                ComPtr<Credential> c;
                c.Attach(new Credential(sid, name, scenario_, [this] { if (events_) events_->CredentialsChanged(context_); }));
                credentials_.push_back(std::move(c));
            }
            CoTaskMemFree(sid); CoTaskMemFree(name);
        }
        return S_OK;
    }
private:
    friend struct ProviderTestAccess;
    void clear() { for (auto& c : credentials_) c->stop(); credentials_.clear(); }
    std::atomic<ULONG> refs_{1};
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario_ = CPUS_LOGON;
    ComPtr<ICredentialProviderEvents> events_;
    UINT_PTR context_ = 0;
    std::vector<ComPtr<Credential>> credentials_;
};
class Factory final : public IClassFactory {
public:
    Factory() { ++objects; }
    ~Factory() { --objects; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { auto n = --refs_; if (!n) delete this; return n; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto provider = new Provider;
        HRESULT hr = provider->QueryInterface(iid, out); provider->Release(); return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override { if (lock) ++objects; else --objects; return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};
}
extern "C" BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) instance = dll;
    return TRUE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    if (objects != 0) return S_FALSE;
    UnregisterClassW(L"MediaDuet.Unlock.Events", instance);
    return S_OK;
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new Factory;
    HRESULT hr = factory->QueryInterface(iid, out); factory->Release(); return hr;
}
