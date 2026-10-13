#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace md::app {

struct TrayMenuState {
    std::wstring status;
    bool lyricsOn = false;
    std::string policyMode;
    bool canControl = false;
};

// Notification-area icon and its context menu. Owns a hidden top-level window
// which also serves as the SMTC bridge window and single-instance target.
class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onOpen;
        std::function<void(const std::string& cmd)> onCommand;
        std::function<TrayMenuState()> menuState;
        // Explorer (re)created the taskbar (TaskbarCreated broadcast; this hidden,
        // unowned top-level window always receives it).
        std::function<void()> onTaskbarCreated;
    };
    static constexpr wchar_t kWindowClass[] = L"MediaDuet.Host";
    static constexpr UINT kMsgActivate = WM_APP + 0x201; // posted by a second instance

    explicit TrayIcon(Callbacks cb);
    ~TrayIcon();
    bool create();
    void setTooltip(const std::wstring& tip);
    void setConnected(bool connected);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void add();
    void showMenu();

    Callbacks cb_;
    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    HICON iconOn_ = nullptr;
    HICON iconOff_ = nullptr;
    bool connected_ = false;
    std::wstring tip_ = L"MediaDuet";
};

} // namespace md::app
