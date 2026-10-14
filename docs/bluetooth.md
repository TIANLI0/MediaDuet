# Bluetooth and LAN links

## Roles and why

| | Android | Windows |
|-|-|-|
| RFCOMM | **server**: `BluetoothAdapter.listenUsingRfcommWithServiceRecord("MediaDuet", 5f3a8c2e-7d41-4b9a-9c61-2e8d0f6b4a17)` (secure, needs a system bond) | **client**: `BluetoothDevice.FromBluetoothAddressAsync` → `GetRfcommServicesForIdAsync` (cached, then uncached SDP) → `StreamSocket.ConnectAsync(..., BluetoothEncryptionAllowNullAuthentication)` |
| TCP | **server** on port 47321 (ephemeral fallback) + NSD `_mediaduet._tcp` | **client**; DNS-SD via `DeviceWatcher` (AssociationEndpointService, Dnssd protocol), manual `host:port` |

The phone keeps one cheap blocking `accept()` per transport inside an already
system-bound process; Windows, which has no background limits, owns the
reconnect loop. Windows never listens, so no firewall prompt is needed.

BLE is not used for data. (It could be added later for presence/wake-up only.)

## Phase 0 verification on the development machine

`MediaDuetProbe.exe` (built from `windows/tools/probe`) reported on 2026-10-12:

* Intel Wireless Bluetooth adapter: classic ✔, LE ✔, central ✔.
* Paired *OPPO Find X8 Ultra*: RFCOMM SDP records readable (cached).
* `AudioPlaybackConnection` eligible devices: *OPPO Find X8 Ultra* (A2DP sink feasible).
* GSMTC session enumeration and SMTC registration from a hidden Win32 window ✔.

This is API-level evidence. An RFCOMM data exchange with a real phone requires
the Android app on a physical device (the emulator has no Bluetooth) — see
[testing.md](testing.md) for the manual procedure and its current status.

## Pairing layers

1. **System bond** (Windows Settings → Bluetooth, or Android Settings). Required
   for secure RFCOMM. MediaDuet does not bypass it.
2. **MediaDuet pairing** (application trust): ECDH + 6-digit SAS confirmed on
   both screens, only while the phone's *pairing mode* is open. Produces the
   32-byte pairing key used to authenticate every later connection on any
   transport. A Bluetooth bond alone never authorises control.

## Reconnection triggers (event driven)

| Event | Source | Effect |
|-------|--------|--------|
| Adapter radio on/off | `Windows.Devices.Radios.Radio.StateChanged` | BT attempts start/stop; BT links closed when off |
| Phone ACL connected | `BluetoothDevice.ConnectionStatusChanged` | immediate RFCOMM attempt, back-off reset |
| DNS-SD service found/removed | `DeviceWatcher` | immediate LAN attempt |
| Network changed | `NetworkInformation.NetworkStatusChanged` | LAN probe + DNS-SD restart |
| Phone said `bye {user_disconnect}` | protocol | long back-off (≈ 60 s) |
| `error {not_paired}` | protocol | pairing removed, no more attempts |

Otherwise exponential back-off with ±20 % jitter (BT 2 s → 60 s, LAN 1 s → 30 s).

## Transport selection

See [architecture.md](architecture.md#link-policy-connectionmanager). Unit tests:
`windows/tests/test_connection_manager.cpp` (LAN preferred in Auto, failover to Bluetooth, stable
switch-back, flapping, exactly-once commands, resume, back-off, no adapter,
revocation, heartbeats).

## Known limitations

* Windows' RFCOMM client may take 5–15 s for an uncached SDP query when the phone
  is not already connected at the baseband level.
* Some OEM ROMs kill Bluetooth server sockets of background apps; see
  [android/README.md](../android/README.md) for battery settings.
* Multiple paired phones are supported, but only one is connected at a time (the
  first that becomes reachable).
