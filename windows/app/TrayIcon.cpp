#include "TrayIcon.h"

#include "Strings.h"
#include "resources/resource.h"

#include <shellapi.h>

namespace md::app {

namespace {
constexpr UINT kTrayMsg = WM_APP + 0x200;
constexpr UINT kTrayId = 1;
enum : UINT {
    IdOpen = 100,
    IdCompact,
    IdLyrics,
    IdPlayPause,
    IdNext,
    IdPrev,
    IdSettings,
    IdExit,
    IdModeBase = 200
};
const char* kModes[] = {"off", "windows_first", "android_first", "latest", "custom"};
} // namespace

TrayIcon::TrayIcon(Callbacks cb) : cb_(std::move(cb)) {}

TrayIcon::~TrayIcon() {
    if (hwnd_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    if (iconOn_) DestroyIcon(iconOn_);
    if (iconOff_) DestroyIcon(iconOff_);
}

bool TrayIcon::create() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // A real (hidden) top-level window: required by SMTC interop and TaskbarCreated.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"MediaDuet", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    iconOn_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_TRAY_ON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    iconOff_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_TRAY_OFF), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    add();
    return true;
}

void TrayIcon::add() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = connected_ ? iconOn_ : iconOff_;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayIcon::setTooltip(const std::wstring& tip) {
    if (tip == tip_) return;
    tip_ = tip;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::setConnected(bool c) {
    if (c == connected_) return;
    connected_ = c;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON;
    nid.hIcon = c ? iconOn_ : iconOff_;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::showMenu() {
    TrayMenuState st = cb_.menuState ? cb_.menuState() : TrayMenuState{};
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, st.status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IdOpen, tr("open"));
    AppendMenuW(m, MF_STRING, IdCompact, tr("compact"));
    AppendMenuW(m, MF_STRING | (st.lyricsOn ? MF_CHECKED : 0), IdLyrics, tr("lyrics"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    UINT ctl = st.canControl ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(m, ctl, IdPlayPause, tr("playpause"));
    AppendMenuW(m, ctl, IdNext, tr("next"));
    AppendMenuW(m, ctl, IdPrev, tr("previous"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU modes = CreatePopupMenu();
    for (UINT i = 0; i < std::size(kModes); ++i) {
        std::string key = std::string("mode.") + kModes[i];
        AppendMenuW(modes, MF_STRING | (st.policyMode == kModes[i] ? MF_CHECKED : 0), IdModeBase + i, tr(key));
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), tr("arbitration"));
    AppendMenuW(m, MF_STRING, IdSettings, tr("settings"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IdExit, tr("exit"));
    SetMenuDefaultItem(m, IdOpen, FALSE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (!cb_.onCommand) return;
    switch (cmd) {
    case IdOpen: cb_.onCommand("open"); break;
    case IdCompact: cb_.onCommand("compact"); break;
    case IdLyrics: cb_.onCommand("lyrics"); break;
    case IdPlayPause: cb_.onCommand("toggle"); break;
    case IdNext: cb_.onCommand("next"); break;
    case IdPrev: cb_.onCommand("previous"); break;
    case IdSettings: cb_.onCommand("settings"); break;
    case IdExit: cb_.onCommand("exit"); break;
    default:
        if (cmd >= IdModeBase && cmd < IdModeBase + std::size(kModes))
            cb_.onCommand(std::string("mode:") + kModes[cmd - IdModeBase]);
        break;
    }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, w, l);
    if (msg == self->taskbarCreated_ && msg != 0) {
        self->add();
        if (self->cb_.onTaskbarCreated) self->cb_.onTaskbarCreated();
        return 0;
    }
    if (msg == kMsgActivate) {
        if (self->cb_.onOpen) self->cb_.onOpen();
        return 0;
    }
    if (msg == kTrayMsg) {
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (self->cb_.onOpen) self->cb_.onOpen();
            return 0;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: self->showMenu(); return 0;
        default: break;
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (self->cb_.onCommand) self->cb_.onCommand("exit");
        return 0;
    }
    if (msg == WM_ENDSESSION && w) {
        if (self->cb_.onCommand) self->cb_.onCommand("exit");
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

} // namespace md::app
