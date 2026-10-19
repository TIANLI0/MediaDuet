#include "Strings.h"

#include <windows.h>

#include <map>
#include <string>

namespace md::app {

namespace {

Lang g_lang = Lang::En;

struct Entry {
    const wchar_t* en;
    const wchar_t* zh;
};

const std::map<std::string_view, Entry>& table() {
    static const std::map<std::string_view, Entry> t = {
        {"app", {L"MediaDuet", L"MediaDuet"}},
        {"open", {L"Open MediaDuet", L"打开 MediaDuet"}},
        {"compact", {L"Mini player", L"迷你播放器"}},
        {"lyrics", {L"Desktop lyrics", L"桌面歌词"}},
        {"a2dp", {L"Play phone audio on this PC", L"用电脑播放手机声音"}},
        {"playpause", {L"Play / Pause", L"播放 / 暂停"}},
        {"next", {L"Next", L"下一首"}},
        {"previous", {L"Previous", L"上一首"}},
        {"arbitration", {L"Auto pause", L"自动暂停"}},
        {"mode.off", {L"Off", L"关闭"}},
        {"mode.windows_first", {L"PC first", L"电脑优先"}},
        {"mode.android_first", {L"Phone first", L"手机优先"}},
        {"mode.latest", {L"Latest wins", L"最新播放优先"}},
        {"mode.custom", {L"Custom priorities", L"自定义优先级"}},
        {"settings", {L"Settings…", L"设置…"}},
        {"exit", {L"Exit", L"退出"}},
        {"update", {L"Update to %s…", L"更新到 %s…"}},
        {"status.no_peer", {L"No phone paired", L"尚未配对手机"}},
        {"status.disconnected", {L"Phone not connected", L"手机未连接"}},
        {"status.connecting", {L"Connecting…", L"正在连接…"}},
        {"status.bt", {L"Connected via Bluetooth", L"已通过蓝牙连接"}},
        {"status.lan", {L"Connected via LAN", L"已通过局域网连接"}},
        {"status.standalone", {L"This PC only", L"仅此电脑"}},
        {"overlay.lock", {L"Lock position", L"锁定位置"}},
        {"overlay.unlock", {L"Unlock position", L"解锁位置"}},
        {"overlay.hide", {L"Hide lyrics", L"隐藏歌词"}},
        {"overlay.settings", {L"Lyrics settings…", L"歌词设置…"}},
        {"overlay.earlier", {L"Lyrics earlier 0.1 s", L"歌词提前 0.1 秒"}},
        {"overlay.later", {L"Lyrics later 0.1 s", L"歌词延后 0.1 秒"}},
        {"overlay.resetOffset", {L"Reset lyrics offset (now %s)", L"重置歌词偏移（当前 %s）"}},
        {"overlay.previous", {L"Previous", L"上一首"}},
        {"overlay.playPause", {L"Play / Pause", L"播放 / 暂停"}},
        {"overlay.next", {L"Next", L"下一首"}},
        {"overlay.smaller", {L"Smaller text", L"缩小字体"}},
        {"overlay.larger", {L"Larger text", L"放大字体"}},
        {"overlay.close", {L"Close desktop lyrics", L"关闭桌面歌词"}},
        {"overlay.unlockTip", {L"Unlock desktop lyrics", L"解锁桌面歌词"}},
        {"ui.missing", {L"MediaDuetUI.exe was not found next to MediaDuet.exe. Reinstall MediaDuet.",
                        L"未在 MediaDuet.exe 同目录找到 MediaDuetUI.exe，请重新安装 MediaDuet。"}},
        {"already", {L"MediaDuet is already running.", L"MediaDuet 已在运行。"}},
    };
    return t;
}

} // namespace

void setLanguage(std::string_view s) {
    if (s == "zh-CN") g_lang = Lang::Zh;
    else if (s == "en-US") g_lang = Lang::En;
    else g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En;
}

Lang language() { return g_lang; }

std::string languageTag() { return g_lang == Lang::Zh ? "zh-CN" : "en-US"; }

const wchar_t* tr(std::string_view key) {
    auto& t = table();
    auto it = t.find(key);
    if (it == t.end()) return L"?";
    return g_lang == Lang::Zh ? it->second.zh : it->second.en;
}

} // namespace md::app
