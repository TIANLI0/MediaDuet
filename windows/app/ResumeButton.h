#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

struct ITaskbarList3;

namespace md::app {

// A taskbar button of its own that offers to continue the phone's song on this PC, like
// Windows' cross-device resume: the music app's icon with a phone badge, the song as its
// tooltip and the cover as its thumbnail, with previous / play-pause / next for the phone
// under the thumbnail. Clicking the button does not open a window; it asks for the handoff.
// Backed by a minimized, off-screen window with its own AppUserModelID, so it never groups
// with MediaDuet's windows.
class ResumeButton {
public:
    enum class Control { Previous, PlayPause, Next };
    // The thumbnail toolbar (the phone's player).
    struct Controls {
        bool previous = false, playPause = false, next = false; // enabled
        bool playing = false;
        std::wstring previousTip, playTip, pauseTip, nextTip;
        bool operator==(const Controls&) const = default;
    };
    struct Offer {
        std::string id;                 // what is offered; a new id rebuilds icon and thumbnail
        std::wstring title;             // window title = taskbar tooltip
        std::filesystem::path iconFile; // the music app's executable; empty: MediaDuet's icon
        std::filesystem::path artwork;  // cover for the thumbnail; empty: the icon
        std::wstring badgeTip;          // tooltip of the phone badge
        Controls controls;
    };
    enum class State { Idle, Working, Failed };
    struct Callbacks {
        std::function<void()> clicked;
        std::function<void()> closed; // "Close window" from the button's menu
        std::function<void(Control)> control;
    };

    explicit ResumeButton(Callbacks cb);
    ~ResumeButton();
    ResumeButton(const ResumeButton&) = delete;
    ResumeButton& operator=(const ResumeButton&) = delete;

    void show(const Offer& offer);
    void hide();
    bool visible() const { return visible_; }
    void setState(State s);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l);
    LRESULT handle(UINT msg, WPARAM w, LPARAM l);
    void ensureWindow();
    void applyIcons();
    void applyState();
    void sendThumbnail(int maxWidth, int maxHeight);
    void applyThumbBar();

    Callbacks cb_;
    HWND hwnd_ = nullptr;
    UINT buttonCreated_ = 0;
    ITaskbarList3* taskbar_ = nullptr;
    bool buttonReady_ = false;
    bool visible_ = false;
    Offer offer_;
    State state_ = State::Idle;
    HICON big_ = nullptr, small_ = nullptr, badge_ = nullptr;
    HICON thumbIcons_[4] = {}; // previous, play, pause, next
    bool thumbLight_ = false;  // the icons are drawn for a light taskbar
    bool thumbAdded_ = false;  // ThumbBarAddButtons done for the current taskbar button
};

} // namespace md::app
