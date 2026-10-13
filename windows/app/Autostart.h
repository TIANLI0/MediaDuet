#pragma once

namespace md::app {

// Per-user autostart via HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
bool setAutostart(bool enabled);
bool autostartEnabled();

} // namespace md::app
