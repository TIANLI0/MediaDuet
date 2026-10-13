#include "TaskbarWidget.h"

#include "OverlayCommon.h"

#include "md/lyrics/Karaoke.h"
#include "md/overlay/SurfacePolicy.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windowsx.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace md::platform {

using winrt::com_ptr;
namespace ov = md::overlay;

namespace {

constexpr wchar_t kClass[] = L"MediaDuet.TaskbarWidget";
constexpr int64_t kTickMs = 2'000;          // fullscreen / z-order / geometry-change / watchdog checks
constexpr int64_t kProbeRefreshMs = 15'000;  // re-query UIA at most this often without a change
constexpr int64_t kProbeMinGapMs = 1'500;
constexpr int64_t kProbeStuckMs = 8'000;     // worker did not answer despite UIA timeouts
constexpr float kPadDip = 8.f;
constexpr float kButtonDip = 30.f;
constexpr UINT kMsgPostedLayout = WM_APP + 0x31;

bool taskbarIsLight() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

DWORD taskbarAlignment() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl",
                 RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

DWORD osBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = [] {
        OSVERSIONINFOW v{sizeof(v)};
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            fn(&v);
        return v.dwBuildNumber;
    }();
    return build;
}

ov::Rect toRect(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

// Cheap signature of the windows that get taskbar buttons (local window-manager
// data only; no messages): changes when apps open or close, which moves the
// task-button area.
uint64_t taskWindowSignature() {
    struct Acc {
        uint64_t n = 0, h = 1469598103934665603ull;
    } acc;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* a = reinterpret_cast<Acc*>(lp);
            if (!IsWindowVisible(w)) return TRUE;
            LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return TRUE;
            if (GetWindow(w, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
            ++a->n;
            a->h = (a->h ^ reinterpret_cast<uintptr_t>(w)) * 1099511628211ull;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&acc));
    return acc.h ^ (acc.n << 56);
}

// ---------------------------------------------------------------------------
// UI Automation worker: the only place that talks to Explorer synchronously.
// MTA thread, owns no windows, UIA connection/transaction timeouts, one cached
// FindAll per query. Results are handed to `sink` on the worker thread.
// ---------------------------------------------------------------------------
struct ProbeResult {
    bool ok = false;
    bool timedOut = false;
    std::string error;
    HWND tray = nullptr;
    ov::Rect trayRect;
    bool autoHide = false;
    bool xaml = false;
    std::optional<ov::Rect> start, widgets;
    long tasksRight = 0;
    int64_t tookMs = 0;
};

class TaskbarProbe {
public:
    using Sink = std::function<void(ProbeResult)>;
    explicit TaskbarProbe(Sink sink) : st_(std::make_shared<State>()) {
        st_->sink = std::move(sink);
        auto st = st_;
        std::thread([st] { run(st); }).detach();
    }
    ~TaskbarProbe() {
        {
            std::lock_guard l(st_->m);
            st_->stop = true;
        }
        st_->cv.notify_all();
        // Wait for a running callback to finish, then never call back again. A UIA
        // call stuck in Explorer keeps only the worker (and its shared state) alive.
        std::lock_guard l(st_->sinkMutex);
        st_->sink = nullptr;
    }
    bool busy() const { return st_->busy.load(); }
    bool request(HWND tray) {
        std::lock_guard l(st_->m);
        if (st_->busy || st_->job) return false;
        st_->job = tray;
        st_->busy = true;
        st_->cv.notify_all();
        return true;
    }

private:
    struct State {
        std::mutex m, sinkMutex;
        std::condition_variable cv;
        std::optional<HWND> job;
        std::atomic<bool> busy{false};
        bool stop = false;
        Sink sink;
    };

    static void run(std::shared_ptr<State> st) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        com_ptr<IUIAutomation> uia;
        for (;;) {
            HWND tray;
            {
                std::unique_lock l(st->m);
                st->cv.wait(l, [&] { return st->stop || st->job; });
                if (st->stop) break;
                tray = *st->job;
                st->job.reset();
            }
            ProbeResult r;
            try {
                r = probe(uia, tray);
            } catch (...) {
                r.error = "exception";
            }
            st->busy = false;
            std::lock_guard l(st->sinkMutex);
            if (st->sink) st->sink(std::move(r));
        }
        uia = nullptr;
        winrt::uninit_apartment();
    }

    static com_ptr<IUIAutomationCondition> idOr(IUIAutomation* uia) {
        std::vector<com_ptr<IUIAutomationCondition>> parts;
        auto prop = [&](PROPERTYID id, const wchar_t* value) {
            VARIANT v;
            v.vt = VT_BSTR;
            v.bstrVal = SysAllocString(value);
            com_ptr<IUIAutomationCondition> c;
            if (SUCCEEDED(uia->CreatePropertyCondition(id, v, c.put()))) parts.push_back(c);
            VariantClear(&v);
        };
        for (const wchar_t* id : {L"StartButton", L"WidgetsButton", L"SearchButton", L"TaskViewButton",
                                  L"ComposerControlTaskbarUx"})
            prop(UIA_AutomationIdPropertyId, id);
        prop(UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer");
        std::vector<IUIAutomationCondition*> raw;
        for (auto& p : parts) raw.push_back(p.get());
        com_ptr<IUIAutomationCondition> out;
        uia->CreateOrConditionFromNativeArray(raw.data(), static_cast<int>(raw.size()), out.put());
        return out;
    }

    static ProbeResult probe(com_ptr<IUIAutomation>& uia, HWND tray) {
        ProbeResult r;
        r.tray = tray;
        auto t0 = GetTickCount64();
        RECT tb{};
        if (!IsWindow(tray) || !GetWindowRect(tray, &tb)) {
            r.error = "no_taskbar";
            return r;
        }
        r.trayRect = toRect(tb);
        // SHAppBarMessage sends a message to the taskbar: only ever from this thread.
        APPBARDATA abd{sizeof(abd)};
        r.autoHide = (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
        HWND bridge = FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
        HWND classicStart = FindWindowExW(tray, nullptr, L"Start", nullptr);
        // Windows 11 XAML taskbar (also while it is still starting and has no bridge
        // yet); a visible classic Start button means a Windows 10 style taskbar.
        r.xaml = bridge || (osBuild() >= 22000 && !(classicStart && IsWindowVisible(classicStart)));
        if (!r.xaml) {
            r.ok = true;
            r.tookMs = static_cast<int64_t>(GetTickCount64() - t0);
            return r;
        }
        if (!uia) {
            com_ptr<IUIAutomation> u;
            if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(u.put()))) &&
                FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(u.put())))) {
                r.error = "uia_unavailable";
                return r;
            }
            if (auto u2 = u.try_as<IUIAutomation2>()) {
                u2->put_ConnectionTimeout(1000);
                u2->put_TransactionTimeout(1500);
            }
            uia = u;
        }
        auto fail = [&](HRESULT hr, const char* what) {
            r.timedOut = hr == static_cast<HRESULT>(0x80131505); // UIA_E_TIMEOUT
            r.error = r.timedOut ? "uia_timeout" : what;
            r.tookMs = static_cast<int64_t>(GetTickCount64() - t0);
            return r;
        };
        com_ptr<IUIAutomationElement> root;
        HRESULT hr = uia->ElementFromHandle(tray, root.put());
        if (FAILED(hr) || !root) return fail(hr, "uia_root");
        com_ptr<IUIAutomationCacheRequest> cache;
        if (FAILED(uia->CreateCacheRequest(cache.put()))) return fail(E_FAIL, "uia_cache");
        cache->AddProperty(UIA_BoundingRectanglePropertyId);
        cache->AddProperty(UIA_AutomationIdPropertyId);
        cache->AddProperty(UIA_ClassNamePropertyId);
        cache->put_AutomationElementMode(AutomationElementMode_None); // cached values only
        auto cond = idOr(uia.get());
        com_ptr<IUIAutomationElementArray> arr;
        hr = root->FindAllBuildCache(TreeScope_Descendants, cond.get(), cache.get(), arr.put());
        if (FAILED(hr) || !arr) return fail(hr, "uia_query");
        int n = 0;
        arr->get_Length(&n);
        long tasks = 0;
        for (int i = 0; i < n; ++i) {
            com_ptr<IUIAutomationElement> e;
            if (FAILED(arr->GetElement(i, e.put())) || !e) continue;
            RECT rc{};
            e->get_CachedBoundingRectangle(&rc);
            BSTR id = nullptr, cls = nullptr;
            e->get_CachedAutomationId(&id);
            e->get_CachedClassName(&cls);
            std::wstring sid = id ? id : L"", scls = cls ? cls : L"";
            SysFreeString(id);
            SysFreeString(cls);
            if (rc.right <= rc.left) continue; // hidden button
            if (sid == L"StartButton") r.start = toRect(rc);
            else if (sid == L"WidgetsButton") r.widgets = toRect(rc);
            else tasks = std::max<long>(tasks, rc.right);
        }
        if (r.start) r.tasksRight = std::max(tasks, r.start->right);
        r.ok = true;
        r.tookMs = static_cast<int64_t>(GetTickCount64() - t0);
        return r;
    }

    std::shared_ptr<State> st_;
};

} // namespace

struct TaskbarWidget::Impl {
    IScheduler& sched;
    Callbacks cb;
    TestOptions test;
    HWND hwnd = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // configuration
    bool enabled = false;
    std::string content = "auto";
    std::string position = "left";
    int offset = 0;
    float widthDip = 360.f;
    float fontSize = 13.f;
    bool translation = true;
    bool animations = true, lowPower = false;

    // content
    std::string title, artist, trackId;
    bool playing = false;
    uint32_t actions = 0;
    std::shared_ptr<const lyrics::Lyrics> lyr;
    std::shared_ptr<const lyrics::LyricsClock> clock;
    int64_t delayMs = 0;
    int karaIdx = -1;
    lyrics::KaraokeLine kara;

    // state
    bool shownFlag = false;
    std::string reason = "disabled";
    ov::Rect rect;
    UINT dpi = 96;
    bool hover = false;
    bool light = false;
    bool fullscreen = false;

    // geometry probing (results from the UIA worker)
    std::unique_ptr<TaskbarProbe> probe;
    std::optional<ProbeResult> geo; // last good
    std::string geoError;           // why there is no usable geometry
    int probeFailures = 0;
    int64_t probeStartedAt = -1;
    int64_t lastProbeAt = -1;
    int64_t nextProbeAt = 0;
    bool probeStuckLogged = false;
    struct Key {
        HWND tray = nullptr;
        ov::Rect rect;
        UINT dpi = 0;
        DWORD align = 0;
        uint64_t windows = 0;
        bool operator==(const Key&) const = default;
    } probedKey;
    int timeoutLogs = 0;
    int64_t lastRecreatedAt = -1;

    ov::ShowWatchdog watchdog;
    int watchdogLogs = 0;

    // resources
    LayeredSurface surface;
    TextCache texts;
    com_ptr<ID2D1Layer> layer;
    uint64_t layerGen = 0; // layers belong to one render target
    D2D1_RECT_F btnPrev{}, btnPlay{}, btnNext{};

    ScopedTimer frameTimer, tickTimer;
    HWINEVENTHOOK fgHook = nullptr;
    UINT taskbarCreatedMsg = 0;
    static inline Impl* hookTarget = nullptr;

    Impl(IScheduler& s, Callbacks c, TestOptions t)
        : sched(s), cb(std::move(c)), test(std::move(t)), frameTimer(&s), tickTimer(&s) {
        if (!test.now) test.now = [] { return lyrics::steadyNowMs(); };
        taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        if (test.offscreen) {
            dpi = test.dpi;
            light = test.light;
        }
    }

    ~Impl() {
        *alive = false;
        if (fgHook) UnhookWinEvent(fgHook);
        if (hookTarget == this) hookTarget = nullptr;
        probe.reset();
        destroyWindow();
    }

    float scale() const { return dpi / 96.f; }
    double now() const { return test.now(); }

    // ------------------------------------------------------------ geometry
    static HWND trayWindow() { return FindWindowW(L"Shell_TrayWnd", nullptr); }

    Key currentKey() {
        Key k;
        k.tray = trayWindow();
        RECT r{};
        if (k.tray && GetWindowRect(k.tray, &r)) k.rect = toRect(r);
        k.dpi = k.tray ? GetDpiForWindow(k.tray) : 0;
        k.align = taskbarAlignment();
        k.windows = taskWindowSignature();
        return k;
    }

    void ensureProbe() {
        if (probe) return;
        std::weak_ptr<bool> a = alive;
        IScheduler* s = &sched;
        probe = std::make_unique<TaskbarProbe>([a, s, this](ProbeResult r) {
            // Worker thread: hand over to the host thread.
            s->post([a, this, r = std::move(r)]() mutable {
                if (!a.expired()) onProbeResult(std::move(r));
            });
        });
    }

    // Asks the worker for fresh geometry unless one is running or back-off applies.
    void requestProbe(bool force) {
        if (test.offscreen) return;
        ensureProbe();
        int64_t t = sched.nowMs();
        if (probe->busy()) {
            if (probeStartedAt >= 0 && t - probeStartedAt > kProbeStuckMs && !probeStuckLogged) {
                probeStuckLogged = true;
                log::info("taskbar", "taskbar measurement has not answered for {} s; keeping the last layout",
                          (t - probeStartedAt) / 1000);
            }
            return;
        }
        if (!force && t < nextProbeAt) return;
        HWND tray = trayWindow();
        if (!tray) {
            geoError = "no_taskbar";
            return;
        }
        if (probe->request(tray)) {
            probeStartedAt = t;
            probedKey = currentKey();
        }
    }

    void onProbeResult(ProbeResult r) {
        int64_t t = sched.nowMs();
        probeStartedAt = -1;
        probeStuckLogged = false;
        lastProbeAt = t;
        bool notReady = r.ok && r.xaml && !r.start;
        if (r.ok && !notReady) {
            if (probeFailures > 0) log::info("taskbar", "taskbar measured again after {} failed attempt(s)", probeFailures);
            geo = r;
            geoError.clear();
            probeFailures = 0;
            nextProbeAt = t + kProbeMinGapMs;
        } else {
            ++probeFailures;
            nextProbeAt = t + ov::probeBackoffMs(probeFailures, 1000, notReady ? 15'000 : 60'000);
            if (notReady) {
                geoError = "taskbar_not_ready";
                if (r.ok) geo.reset(); // never keep stale Windows 10 / old geometry
            } else if (!geo) {
                geoError = r.error == "no_taskbar" ? "no_taskbar" : "taskbar_not_ready";
            }
            // Rate limited: first few, then every 10th.
            if (timeoutLogs < 3 || probeFailures % 10 == 0) {
                ++timeoutLogs;
                log::info("taskbar", "taskbar measurement {} after {} ms ({}); retry in {} s",
                          r.timedOut ? "timed out" : "failed", r.tookMs, notReady ? "taskbar_not_ready" : r.error,
                          (nextProbeAt - t) / 1000);
            }
        }
        refresh(true);
    }

    // Placement from the last probe plus live (non-blocking) window data.
    ov::TaskbarPlacement placement() {
        ov::TaskbarPlacement p;
        HWND tray = trayWindow();
        if (!tray) {
            p.reason = "no_taskbar";
            return p;
        }
        if (!geo || geo->tray != tray) {
            p.reason = geoError.empty() ? "taskbar_not_ready" : geoError;
            return p;
        }
        ov::TaskbarGeometry g;
        RECT tb{};
        GetWindowRect(tray, &tb);
        g.tray = toRect(tb);
        if (HWND tn = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr)) {
            RECT r{};
            GetWindowRect(tn, &r);
            g.notify = toRect(r);
        }
        if (HWND rb = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr)) {
            RECT r{};
            GetWindowRect(rb, &r);
            g.rebar = toRect(r);
        }
        dpi = GetDpiForWindow(tray);
        if (!dpi) dpi = 96;
        g.scale = scale();
        g.autoHide = geo->autoHide;
        g.xaml = geo->xaml;
        g.start = geo->start;
        g.widgets = geo->widgets;
        g.tasksRight = geo->tasksRight;
        return ov::placeTaskbarWidget(g, position, offset, widthDip);
    }

    // ------------------------------------------------------------ window
    bool windowValid() const { return hwnd && IsWindow(hwnd); }

    bool ensureWindow() {
        if (test.offscreen) return false;
        if (hwnd && !IsWindow(hwnd)) {
            log::info("taskbar", "taskbar widget window was destroyed; recreating");
            hwnd = nullptr;
            shownFlag = false;
        }
        if (hwnd) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassExW(&wc);
        // Unowned on purpose (see the class comment): never GWLP_HWNDPARENT = Shell_TrayWnd.
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass,
                               L"MediaDuet Taskbar", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!fgHook) {
            hookTarget = this;
            fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        light = taskbarIsLight();
        return true;
    }

    void destroyWindow() {
        surface.release();
        layer = nullptr;
        if (hwnd) {
            HWND h = hwnd;
            hwnd = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            DestroyWindow(h);
        }
        shownFlag = false;
    }

    // Keeps the widget above the taskbar (both are topmost; activating the taskbar
    // raises it over us). Only touches our own window, asynchronously.
    void raiseAboveTaskbar(bool force) {
        if (!windowValid() || !shownFlag) return;
        if (!force) {
            HWND tray = trayWindow();
            bool covered = false;
            int n = 0;
            for (HWND h = GetWindow(hwnd, GW_HWNDPREV); h && n < 256; h = GetWindow(h, GW_HWNDPREV), ++n)
                if (h == tray) {
                    covered = true;
                    break;
                }
            if (!covered) return;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }

    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
        if (!hookTarget) return;
        hookTarget->checkFullscreen();
        hookTarget->raiseAboveTaskbar(false);
    }

    void checkFullscreen() {
        if (test.offscreen) return;
        QUERY_USER_NOTIFICATION_STATE st{};
        bool fs = SUCCEEDED(SHQueryUserNotificationState(&st)) &&
                  ov::taskbarBlockedByFullscreen(static_cast<ov::NotifyState>(st));
        if (fs != fullscreen) {
            fullscreen = fs;
            refresh(true);
        }
    }

    bool hasContent() const { return !title.empty(); }

    void setState(bool show, const std::string& why) {
        bool changed = show != shownFlag || why != reason;
        if (show && !shownFlag && windowValid()) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            shownFlag = true;
            raiseAboveTaskbar(true);
        }
        if (!show && shownFlag && windowValid()) ShowWindow(hwnd, SW_HIDE);
        shownFlag = show && windowValid();
        reason = why;
        if (!show) surface.release();
        if (changed && cb.onState) cb.onState(shownFlag, why);
    }

    // Periodic, only while the widget is wanted: full-screen state, z-order,
    // geometry changes, watchdog. Zero cost while disabled / nothing plays.
    void tick() {
        if (test.offscreen || !enabled || !hasContent()) return;
        checkFullscreen();
        raiseAboveTaskbar(false);
        int64_t t = sched.nowMs();
        if (!fullscreen) {
            Key k = currentKey();
            bool changed = !(k == probedKey);
            if (changed || !geo || (shownFlag && t - lastProbeAt >= kProbeRefreshMs)) requestProbe(false);
        }
        // Watchdog: should be visible (or failing for a non-legitimate reason) but is not.
        bool want = !fullscreen && (shownFlag || !ov::isExpectedHideReason(reason));
        bool actual = shownFlag && windowValid() && IsWindowVisible(hwnd);
        if (watchdog.tick(t, want, actual)) {
            if (watchdogLogs < 5 || watchdog.rebuilds() % 20 == 0) {
                ++watchdogLogs;
                log::info("taskbar", "taskbar widget not shown for {} s (reason: {}); rebuilding window and surface",
                          ov::ShowWatchdog::kGraceMs / 1000, reason.empty() ? "window lost" : reason);
            }
            destroyWindow();
            requestProbe(false);
            refresh(true);
        }
        tickTimer.start(kTickMs, [this] { tick(); });
    }

    void ensureTicking() {
        if (!test.offscreen && !tickTimer.active()) tickTimer.start(kTickMs, [this] { tick(); });
    }

    // Re-evaluates visibility/geometry and draws one frame.
    void refresh(bool relayout) {
        frameTimer.cancel();
        if (test.offscreen) return;
        ov::TaskbarInputs in;
        in.enabled = enabled;
        in.hasContent = hasContent();
        in.fullscreen = fullscreen;
        if (!enabled || !hasContent()) {
            tickTimer.cancel();
            auto d = ov::decideTaskbar(in);
            return setState(false, d.reason);
        }
        ensureTicking();
        if (fullscreen) return setState(false, "fullscreen");
        if (relayout || !shownFlag) {
            auto p = placement();
            if (!p.ok && (p.reason == "taskbar_not_ready" || p.reason == "no_taskbar")) requestProbe(false);
            in.havePlacement = p.ok;
            in.placementError = p.reason;
            auto d = ov::decideTaskbar(in);
            if (!d.show) return setState(false, d.reason);
            if (!ensureWindow()) return setState(false, "unavailable");
            bool moved = !(p.rect == rect);
            rect = p.rect;
            if (moved) surface.release();
        }
        if (!windowValid()) {
            hwnd = nullptr;
            shownFlag = false;
            if (!ensureWindow()) return setState(false, "unavailable");
        }
        if (!render()) return setState(false, "unavailable");
        setState(true, "");
        scheduleFrame();
    }

    double playPos() const {
        if (!clock) return 0;
        auto p = clock->position(now());
        return p ? std::max(0.0, *p - static_cast<double>(delayMs)) : 0.0;
    }

    int currentLine() const {
        if (!lyr || !lyr->synced || content == "player" || !clock || !clock->valid()) return -1;
        return lyrics::lineAt(*lyr, static_cast<int64_t>(playPos()));
    }

    void scheduleFrame() {
        if (!shownFlag || !clock || !clock->playing()) return;
        int idx = currentLine();
        if (!lyr || content == "player") return;
        double p = playPos();
        double rate = std::max(0.1, clock->rate());
        double next = -1;
        bool moving = idx >= 0 && (ensureKara(idx), lyrics::fillMoving(kara, p, &next));
        int64_t delay;
        if (moving) {
            delay = lowPower ? 100 : (animations ? 16 : 250);
        } else {
            double target = next;
            if (idx + 1 < static_cast<int>(lyr->lines.size())) {
                double nl = static_cast<double>(lyr->lines[idx + 1].t);
                target = target < 0 ? nl : std::min(target, nl);
            }
            if (target < 0) return;
            delay = std::clamp<int64_t>(static_cast<int64_t>((target - p) / rate), 15, 60'000);
        }
        frameTimer.start(delay, [this] { refresh(false); });
    }

    void ensureKara(int idx) {
        if (idx == karaIdx || !lyr || idx < 0 || idx >= static_cast<int>(lyr->lines.size())) return;
        karaIdx = idx;
        kara = lyrics::buildKaraoke(lyr->lines[idx]);
    }

    // ------------------------------------------------------------ rendering
    void drawGlyph(const wchar_t* glyph, const D2D1_RECT_F& r, ID2D1Brush* brush, float sizeDip) {
        auto* e = texts.get(glyph, iconFontFamily(), sizeDip, DWRITE_FONT_WEIGHT_NORMAL);
        if (!e) return;
        float x = (r.left + r.right - e->width) / 2, y = (r.top + r.bottom - e->height) / 2;
        surface.rt()->DrawTextLayout(D2D1::Point2F(x, y), e->layout.get(), brush);
    }

    // One text row: karaoke fill (fill != nullptr), shrink up to ~12 % and scroll
    // long lines so the edge stays visible, fade the clipped side(s).
    void drawRow(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, float x, float y, float w,
                 D2D1_COLOR_F color, const lyrics::FillSpan* fill, D2D1_COLOR_F dim) {
        auto* rt = surface.rt();
        const TextEntry* e = texts.get(s, L"Segoe UI Variable Text", size, weight);
        if (!e || s.empty()) return;
        float k = lyrics::fitScale(e->width, w, 0.88f);
        float tw = e->width * k;
        float fx = fill ? fillX(e->carets, *fill) * k : 0.f;
        float off = fill ? lyrics::scrollOffset(tw, w, fx) : 0.f;
        bool clipped = tw > w + 0.5f;
        com_ptr<ID2D1Brush> mask;
        if (clipped) mask = pushFadeClip(rt, layer.get(), D2D1::RectF(x, y - 2, x + w, y + e->height * k + 2), off > 0.5f,
                                         off < tw - w - 0.5f, 14.f);
        D2D1_MATRIX_3X2_F old;
        rt->GetTransform(&old);
        float yk = y + e->height * (1 - k) / 2;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x - off, yk) * old);
        com_ptr<ID2D1Brush> brush;
        if (fill) {
            float feather = std::max(6.f, size * 0.45f) / k;
            float ex = fx / k;
            D2D1_GRADIENT_STOP stops[] = {{0.f, color}, {1.f, dim}};
            com_ptr<ID2D1GradientStopCollection> coll;
            com_ptr<ID2D1LinearGradientBrush> gb;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.put())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                                            D2D1::Point2F(ex - feather / 2, 0), D2D1::Point2F(ex + feather / 2, 0)),
                                                        coll.get(), gb.put())))
                brush = gb;
        }
        if (!brush) {
            com_ptr<ID2D1SolidColorBrush> sb;
            rt->CreateSolidColorBrush(color, sb.put());
            brush = sb;
        }
        rt->DrawTextLayout(D2D1::Point2F(0, 0), e->layout.get(), brush.get());
        rt->SetTransform(old);
        if (clipped) rt->PopLayer();
    }

    bool render() {
        SIZE sz{rect.width(), rect.height()};
        if (sz.cx <= 0 || sz.cy <= 0 || !surface.ensure(d2dFactory(), sz, dpi)) return false;
        draw(sz);
        if (!surface.end()) return false;
        POINT dst{rect.left, rect.top};
        return surface.present(hwnd, &dst, 255);
    }

    void draw(SIZE sz) {
        surface.begin();
        auto* rt = surface.rt();
        if (!layer || layerGen != surface.generation()) {
            layer = nullptr;
            rt->CreateLayer(nullptr, layer.put());
            layerGen = surface.generation();
        }
        float w = sz.cx / scale(), h = sz.cy / scale();
        D2D1_COLOR_F fg = light ? D2D1::ColorF(0.05f, 0.05f, 0.05f, 1) : D2D1::ColorF(1, 1, 1, 1);
        // Nearly invisible fill makes the whole widget hit-testable; hover shows a pill.
        com_ptr<ID2D1SolidColorBrush> bg;
        D2D1_COLOR_F bgc = fg;
        bgc.a = hover ? 0.08f : 0.004f;
        rt->CreateSolidColorBrush(bgc, bg.put());
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 4, w - 1, h - 4), 6, 6), bg.get());

        float textRight = w - kPadDip;
        bool showButtons = hover || content == "player";
        if (showButtons) {
            float bx = w - kPadDip - 3 * kButtonDip;
            float cy = h / 2;
            btnPrev = D2D1::RectF(bx, cy - kButtonDip / 2, bx + kButtonDip, cy + kButtonDip / 2);
            btnPlay = D2D1::RectF(bx + kButtonDip, cy - kButtonDip / 2, bx + 2 * kButtonDip, cy + kButtonDip / 2);
            btnNext = D2D1::RectF(bx + 2 * kButtonDip, cy - kButtonDip / 2, bx + 3 * kButtonDip, cy + kButtonDip / 2);
            com_ptr<ID2D1SolidColorBrush> ib;
            rt->CreateSolidColorBrush(fg, ib.put());
            drawGlyph(L"\xE892", btnPrev, ib.get(), 13);
            drawGlyph(playing ? L"\xE769" : L"\xE768", btnPlay, ib.get(), 15);
            drawGlyph(L"\xE893", btnNext, ib.get(), 13);
            textRight = bx - 4;
        } else {
            btnPrev = btnPlay = btnNext = {};
        }

        float tw = std::max(20.f, textRight - kPadDip);
        int idx = currentLine();
        std::wstring line1, line2;
        bool karaoke = false;
        if (idx >= 0) {
            ensureKara(idx);
            line1 = kara.text;
            karaoke = !line1.empty();
            const auto& cur = lyr->lines[idx];
            if (translation && lyrics::hasTranslation(*lyr)) line2 = text::toWide(cur.translation);
            else if (idx + 1 < static_cast<int>(lyr->lines.size()))
                line2 = lyrics::buildKaraoke(lyr->lines[idx + 1]).text;
        }
        if (line1.empty()) {
            line1 = text::toWide(title);
            line2 = text::toWide(artist);
            karaoke = false;
        }
        float f1 = fontSize, f2 = fontSize * 0.85f;
        bool two = !line2.empty() && h >= (f1 + f2) * 1.35f;
        float blockH = two ? f1 * 1.3f + f2 * 1.25f : f1 * 1.3f;
        float y = (h - blockH) / 2;
        D2D1_COLOR_F c2 = fg, dim = fg;
        c2.a = 0.62f;
        dim.a = 0.45f;
        lyrics::FillSpan span;
        if (karaoke) span = lyrics::fillAt(kara, playPos());
        drawRow(line1, f1, DWRITE_FONT_WEIGHT_SEMI_BOLD, kPadDip, y, tw, fg, karaoke ? &span : nullptr, dim);
        if (two) drawRow(line2, f2, DWRITE_FONT_WEIGHT_NORMAL, kPadDip, y + f1 * 1.3f, tw, c2, nullptr, c2);
    }

    // ------------------------------------------------------------ input
    static bool inRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

    void taskbarRecreated() {
        // Delivered twice (our own window and the host's tray window): once is enough.
        int64_t t = sched.nowMs();
        if (lastRecreatedAt >= 0 && t - lastRecreatedAt < 1000) return;
        lastRecreatedAt = t;
        log::info("taskbar", "taskbar (re)created by Explorer; rebuilding the taskbar widget");
        geo.reset();
        geoError = "taskbar_not_ready";
        probeFailures = 0;
        nextProbeAt = 0;
        timeoutLogs = 0;
        destroyWindow();
        requestProbe(true);
        refresh(true);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, w, l);
        if (msg == self->taskbarCreatedMsg && msg != 0) {
            // We are a top-level unowned window, so we get the broadcast ourselves.
            PostMessageW(h, kMsgPostedLayout, 1, 0);
            return 0;
        }
        switch (msg) {
        case kMsgPostedLayout:
            if (w == 1) self->taskbarRecreated();
            return 0;
        case WM_NCDESTROY:
            // Destroyed behind our back (session end, …): forget the handle so the
            // next layout pass creates a new window instead of drawing into nothing.
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            if (self->hwnd == h) {
                self->hwnd = nullptr;
                self->shownFlag = false;
                self->surface.release();
            }
            break;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!self->hover) {
                self->hover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                self->refresh(false);
            }
            return 0;
        case WM_MOUSELEAVE:
            self->hover = false;
            self->refresh(false);
            return 0;
        case WM_LBUTTONUP: {
            float x = GET_X_LPARAM(l) / self->scale(), y = GET_Y_LPARAM(l) / self->scale();
            if (self->cb.onCommand && inRect(self->btnPrev, x, y)) self->cb.onCommand(media::CommandAction::Previous);
            else if (self->cb.onCommand && inRect(self->btnPlay, x, y)) self->cb.onCommand(media::CommandAction::Toggle);
            else if (self->cb.onCommand && inRect(self->btnNext, x, y)) self->cb.onCommand(media::CommandAction::Next);
            else if (self->cb.onOpen) self->cb.onOpen();
            return 0;
        }
        case WM_SETTINGCHANGE:
            self->light = taskbarIsLight();
            self->requestProbe(true); // alignment / size may have changed
            self->refresh(true);
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            self->surface.release();
            self->requestProbe(true);
            self->refresh(true);
            return 0;
        default: break;
        }
        return DefWindowProcW(h, msg, w, l);
    }
};

TaskbarWidget::TaskbarWidget(IScheduler& sched, Callbacks cb, TestOptions test)
    : impl_(std::make_unique<Impl>(sched, std::move(cb), std::move(test))) {}
TaskbarWidget::~TaskbarWidget() = default;

void TaskbarWidget::configure(const nlohmann::json& t, bool animations, bool lowPower) {
    auto& i = *impl_;
    bool wasEnabled = i.enabled;
    i.enabled = t.value("enabled", true);
    i.content = t.value("content", std::string("auto"));
    i.position = t.value("position", std::string("left"));
    i.offset = t.value("offset", 0);
    i.widthDip = static_cast<float>(t.value("width", 360.0));
    i.fontSize = static_cast<float>(t.value("fontSize", 13.0));
    i.translation = t.value("translation", true);
    i.animations = animations;
    i.lowPower = lowPower;
    i.surface.release();
    i.texts.clear();
    if (i.enabled && !wasEnabled) {
        // Turning it on is a recovery point too: fresh measurement, no back-off.
        i.probeFailures = 0;
        i.nextProbeAt = 0;
        i.checkFullscreen();
        i.requestProbe(true);
    }
    i.refresh(true);
}

void TaskbarWidget::setTrack(const media::MediaSessionInfo* s) {
    auto& i = *impl_;
    std::string title = s ? s->title : "", artist = s ? s->artist : "";
    bool relayout = title.empty() != i.title.empty();
    bool newTrack = s && s->trackId != i.trackId;
    i.title = title;
    i.artist = artist;
    i.playing = s && s->isPlaying();
    i.actions = s ? s->actions : 0;
    if (s) i.trackId = s->trackId;
    if (newTrack && !i.test.offscreen && i.enabled && !i.geo) {
        // A new track is a recovery point: measure now instead of waiting for back-off.
        i.nextProbeAt = 0;
        i.requestProbe(false);
    }
    i.refresh(relayout);
}

void TaskbarWidget::setLyrics(std::shared_ptr<const lyrics::Lyrics> l, const std::string& trackId) {
    impl_->lyr = l && l->synced ? std::move(l) : nullptr;
    impl_->karaIdx = -1;
    impl_->trackId = trackId;
    impl_->texts.clear();
    impl_->refresh(false);
}

void TaskbarWidget::setTiming(std::shared_ptr<const lyrics::LyricsClock> clock, int64_t delayMs) {
    impl_->clock = std::move(clock);
    impl_->delayMs = delayMs;
    impl_->refresh(false);
}

void TaskbarWidget::onTaskbarCreated() {
    if (!impl_->test.offscreen) impl_->taskbarRecreated();
}

bool TaskbarWidget::shown() const { return impl_->shownFlag; }
const std::string& TaskbarWidget::reason() const { return impl_->reason; }

bool TaskbarWidget::snapshot(const std::wstring& path) const { return impl_->surface.savePng(path); }

bool TaskbarWidget::renderOffscreen(int w, int h, bool hover, std::vector<uint8_t>& bgra) {
    auto& i = *impl_;
    i.hover = hover;
    SIZE sz{w, h};
    if (!i.surface.ensure(d2dFactory(), sz, i.dpi)) return false;
    i.draw(sz);
    if (!i.surface.end()) return false;
    return i.surface.copyPixels(bgra);
}

} // namespace md::platform
