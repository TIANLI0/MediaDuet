#include "SmtcBridge.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <fstream>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;

SmtcBridge::SmtcBridge(IScheduler& sched, CommandFn onCommand) : sched_(sched), onCommand_(std::move(onCommand)) {}

SmtcBridge::~SmtcBridge() {
    *alive_ = false;
    try {
        if (smtc_) {
            smtc_.ButtonPressed(buttonTok_);
            smtc_.PlaybackPositionChangeRequested(seekTok_);
            smtc_.PlaybackStatus(MediaPlaybackStatus::Closed);
            smtc_.IsEnabled(false);
        }
    } catch (...) {
    }
}

bool SmtcBridge::init(HWND hwnd) {
    try {
        auto interop = get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls smtc{nullptr};
        check_hresult(interop->GetForWindow(hwnd, guid_of<SystemMediaTransportControls>(), put_abi(smtc)));
        smtc_ = smtc;
        smtc_.IsEnabled(false);
        std::weak_ptr<bool> alive = alive_;
        buttonTok_ = smtc_.ButtonPressed([this, alive](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& a) {
            auto b = a.Button();
            sched_.post([this, alive, b] {
                if (alive.expired() || !shown_) return;
                using B = SystemMediaTransportControlsButton;
                switch (b) {
                case B::Play: onCommand_(media::CommandAction::Play, std::nullopt); break;
                case B::Pause: onCommand_(media::CommandAction::Pause, std::nullopt); break;
                case B::Next: onCommand_(media::CommandAction::Next, std::nullopt); break;
                case B::Previous: onCommand_(media::CommandAction::Previous, std::nullopt); break;
                case B::Stop: onCommand_(media::CommandAction::Stop, std::nullopt); break;
                default: break;
                }
            });
        });
        seekTok_ = smtc_.PlaybackPositionChangeRequested(
            [this, alive](auto&&, PlaybackPositionChangeRequestedEventArgs const& a) {
                int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(a.RequestedPlaybackPosition()).count();
                sched_.post([this, alive, ms] {
                    if (!alive.expired() && shown_) onCommand_(media::CommandAction::Seek, ms);
                });
            });
        log::info("smtc", "SMTC bridge ready");
        return true;
    } catch (hresult_error const& e) {
        log::warn("smtc", "SMTC bridge unavailable: {}", text::toUtf8(e.message()));
        smtc_ = nullptr;
        return false;
    }
}

void SmtcBridge::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) hide();
}

void SmtcBridge::hide() {
    if (!smtc_ || !shown_) return;
    try {
        smtc_.PlaybackStatus(MediaPlaybackStatus::Closed);
        smtc_.DisplayUpdater().ClearAll();
        smtc_.DisplayUpdater().Update();
        smtc_.IsEnabled(false);
    } catch (...) {
    }
    shown_ = false;
    lastTrack_.clear();
    lastArt_.clear();
    lastStatus_ = media::PlaybackStatus::Unknown;
}

void SmtcBridge::setThumbnail(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) return;
    std::weak_ptr<bool> alive = alive_;
    [](SmtcBridge* self, std::weak_ptr<bool> alive, std::vector<uint8_t> data) -> fire_and_forget {
        co_await resume_background();
        try {
            InMemoryRandomAccessStream stream;
            DataWriter w(stream);
            w.WriteBytes(data);
            co_await w.StoreAsync();
            w.DetachStream();
            stream.Seek(0);
            auto ref = RandomAccessStreamReference::CreateFromStream(stream);
            self->sched_.post([self, alive, ref] {
                if (alive.expired() || !self->smtc_ || !self->shown_) return;
                try {
                    auto du = self->smtc_.DisplayUpdater();
                    du.Thumbnail(ref);
                    du.Update();
                } catch (...) {
                }
            });
        } catch (...) {
        }
    }(this, alive, std::move(data));
}

void SmtcBridge::update(const media::MediaSessionInfo* r, const std::filesystem::path& artworkPath,
                        const std::string& deviceName) {
    if (!smtc_) return;
    if (!enabled_ || !r || r->status == media::PlaybackStatus::Stopped) {
        hide();
        return;
    }
    try {
        if (!shown_) {
            smtc_.IsEnabled(true);
            shown_ = true;
        }
        smtc_.IsPlayEnabled(r->supports(media::CommandAction::Play) || r->supports(media::CommandAction::Toggle));
        smtc_.IsPauseEnabled(r->supports(media::CommandAction::Pause) || r->supports(media::CommandAction::Toggle));
        smtc_.IsNextEnabled(r->supports(media::CommandAction::Next));
        smtc_.IsPreviousEnabled(r->supports(media::CommandAction::Previous));
        smtc_.IsStopEnabled(r->supports(media::CommandAction::Stop));
        auto status = r->status == media::PlaybackStatus::Playing  ? MediaPlaybackStatus::Playing
                      : r->status == media::PlaybackStatus::Paused ? MediaPlaybackStatus::Paused
                                                                    : MediaPlaybackStatus::Changing;
        if (r->status != lastStatus_) smtc_.PlaybackStatus(status);
        if (r->trackId != lastTrack_) {
            auto du = smtc_.DisplayUpdater();
            du.Type(MediaPlaybackType::Music);
            auto mp = du.MusicProperties();
            mp.Title(text::toWide(r->title));
            std::string artist = r->artist;
            mp.Artist(text::toWide(artist.empty() ? deviceName : artist));
            mp.AlbumTitle(text::toWide(r->album));
            mp.AlbumArtist(text::toWide(r->albumArtist));
            du.Update();
            lastTrack_ = r->trackId;
            lastArt_.clear();
        }
        std::string art = r->artwork ? r->artwork->hash : std::string();
        if (art != lastArt_ && !art.empty() && std::filesystem::exists(artworkPath)) {
            lastArt_ = art;
            setThumbnail(artworkPath);
        }
        // Timeline only when something meaningful changed (status, seek, track).
        auto pos = r->positionAt(sched_.nowMs());
        bool jumped = true;
        if (lastPos_ && pos && r->status == lastStatus_) {
            int64_t expected = *lastPos_ + (r->isPlaying() ? sched_.nowMs() - lastPosAt_ : 0);
            jumped = std::llabs(expected - *pos) > 1500;
        }
        if (jumped || r->actions != lastActions_) {
            SystemMediaTransportControlsTimelineProperties tl;
            int64_t dur = r->durationMs.value_or(0);
            tl.StartTime(Windows::Foundation::TimeSpan{0});
            tl.EndTime(std::chrono::milliseconds(dur));
            tl.MinSeekTime(Windows::Foundation::TimeSpan{0});
            tl.MaxSeekTime(std::chrono::milliseconds(r->supports(media::CommandAction::Seek) ? dur : 0));
            tl.Position(std::chrono::milliseconds(pos.value_or(0)));
            smtc_.UpdateTimelineProperties(tl);
            lastPos_ = pos;
            lastPosAt_ = sched_.nowMs();
        }
        lastStatus_ = r->status;
        lastActions_ = r->actions;
    } catch (hresult_error const& e) {
        log::warn("smtc", "update failed: {}", text::toUtf8(e.message()));
    }
}

} // namespace md::platform
