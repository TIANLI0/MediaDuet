# Unlock the PC with the phone

A locked Windows session can be unlocked from the phone: the lock screen asks the
phone, the user confirms with a fingerprint (or face / the phone's own PIN), and
Windows unlocks.

Phone approval supports both **Windows sign-in after startup, restart or sign-out**
and **unlocking an existing session** (Win+L, screen-saver lock, lock after sleep).
An automatically started LocalSystem service handles the physical console before
any user signs in. After sign-in the tray host handles ordinary session unlocking.
If Windows restores a locked user session after restart (ARSO) or the tray host
is absent, the service can also unlock that same user's locked console. It verifies
the session token SID and lock state and refuses this fallback while the tray host's
per-session pipe exists, avoiding competing phone sessions.

Initial enrollment requires the Windows **account password**, not a Windows Hello
PIN. Phone biometrics or the phone's own lock-screen PIN confirm subsequent requests.
This runs at the Windows sign-in screen; BitLocker/pre-boot PINs are a separate step.
The paired phone must be running MediaDuet, and network or system-paired Bluetooth
must be available. A Wi-Fi profile requiring user sign-in cannot provide pre-sign-in
connectivity. Existing Windows sign-in methods remain available.

## Pieces

| Piece | Where | Role |
|-------|-------|------|
| `MediaDuetUnlock.dll` | `windows/credential` | Credential provider (COM, loaded by `LogonUI.exe` as SYSTEM). Shows the "MediaDuet" sign-in option, asks the host for the password, hands it to Windows. |
| `UnlockHost` | `windows/app` | Keeps the enrollment, serves the credential provider over a named pipe, talks `unlock.*` with the phone. |
| `MediaDuetUnlockService.exe` | `windows/unlock` | Automatic LocalSystem service. Connects to the enrolled phone before console sign-in using native TCP/RFCOMM, authenticated with the existing pairing key. |
| `MediaDuet.exe --unlock-component …` | `windows/app/UnlockSetup.cpp` | Elevated (UAC) one-time setup: copies the DLL to `%ProgramFiles%\MediaDuet` and registers it. |
| Phone | `android/…/unlock` | Keeps the key in Android Keystore behind biometric / device-credential authentication; releases it after the user confirms. |

The credential provider DLL must live in a folder only administrators can write:
LogonUI loads it as SYSTEM, and MediaDuet itself is installed per user (user-writable).

## Key split (why neither device alone can unlock)

* At setup the user types the Windows password once on the PC. The host verifies it
  (`LogonUserW`), generates a random 32-byte key **K** and an enrollment id, and stores
  `seal = AES-256-GCM(K, nonce, aad, password)` (then DPAPI-protected for the user) in
  `%LOCALAPPDATA%\MediaDuet\unlock.json`. `aad = "MediaDuet-unlock-v1|" + enrollment + "|" + userSid`.
* K is sent to the phone (`unlock.enroll`, inside the authenticated, encrypted link) and the
  PC forgets it. The phone wraps K with an Android Keystore AES key that needs a fresh
  user authentication for every use (`BIOMETRIC_STRONG | DEVICE_CREDENTIAL`).
* To unlock, the phone sends K back (`unlock.result`) after the user confirmed; the
  host opens the seal and passes the password to the credential provider, which
  packs it for Windows (`KERB_INTERACTIVE_UNLOCK_LOGON` / `CredPackAuthenticationBuffer`).
* The PC alone has only the seal, the phone alone only K (behind biometrics). The
  password never leaves the PC; the phone never sees it.

For pre-sign-in use, the UAC helper additionally provisions a version-2 record at
`%ProgramData%\MediaDuet\Unlock\<userSid>.bin`. It contains the same seal, enrollment
and SID, the PC identity, the existing link pairing key, and phone endpoints; it
never contains plaintext password or K. The record uses **machine-scoped DPAPI**
so it can be read before a user profile is loaded. Both ProgramData\MediaDuet and
its Unlock child, and the records themselves, have protected ACLs permitting only
SYSTEM and administrators. Reparse points are rejected. Machine DPAPI alone is not
an access boundary between users: Microsoft documents that any local user can
decrypt a machine-scoped blob if they obtain it, which is why these ACLs are required.
[DPAPI documentation](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)

Provisioning happens after phone enrollment succeeds. A machine-protected temporary
handoff file, readable only by the enrolling user, administrators and SYSTEM, carries
the record to the elevated helper and is removed afterwards. An older enrollment
can be provisioned without re-entering the password by choosing **Repair component**.

## Link protocol (cap `unlock`)

Both sides announce `unlock` when they implement this section. Only a PC sends
`unlock.enroll` / `unlock.request` / `unlock.cancel`; only a phone answers.

| Type | Dir | Body |
|------|-----|------|
| `unlock.enroll` | C→S | `{enrollment, key, pc}` — `enrollment`: 32 lower-hex chars; `key`: base64 of 32 bytes; `pc`: PC name (≤ 64 code points). The phone asks the user to allow it (biometric prompt), wraps `key` and stores it with the sender's device id and `pc`. It replaces any earlier enrollment of the same PC. Answered with `unlock.result {ok}` |
| `unlock.request` | C→S | `{enrollment, pc}` — the PC's lock screen asks to be unlocked. The phone prompts the user and answers `unlock.result {ok:true, key}` or `{ok:false, error:{code}}` |
| `unlock.cancel` | C→S | `{}` with `re` = the `unlock.request` / `unlock.enroll` id — the PC no longer waits (unlocked otherwise, tile left, timed out). The phone withdraws its prompt and does not answer |
| `unlock.result` | S→C | `{ok, key?, error?:{code}}` with `re` |
| `unlock.revoke` | both | `{enrollment}` — forget this enrollment (phone: delete the wrapped key; PC: delete the seal) |

* The phone answers only enrollments it stores **for the sending device id**; others →
  `not_enrolled`.
* Error codes: `denied` (the user cancelled the prompt), `not_enrolled`,
  `key_invalidated` (the Keystore key is gone, e.g. new fingerprints or the lock screen
  was removed — set up again), `no_auth` (the phone has no secure lock screen),
  `busy` (another prompt is open), `timeout` (no answer from the user),
  `unsupported`, `bad_message`.
* The phone waits up to **60 s** for the user on `unlock.request` and **120 s** on
  `unlock.enroll`; the PC gives up 10 s later (and sends `unlock.cancel`).
* One outstanding request at a time per PC; a new request replaces the old one (the old
  one is answered `busy`).

### Phone UX

* `unlock.request` / `unlock.enroll` post a high-importance notification ("Unlock
  DESKTOP-1?") whose tap opens a translucent activity (`showWhenLocked`, `turnScreenOn`)
  that shows the system biometric prompt (device PIN allowed as fallback). With the
  full-screen-intent permission the activity comes up by itself on a locked / dark phone;
  when MediaDuet is in the foreground it opens at once.
* Settings lists the PCs that can be unlocked, each with "Remove" (sends `unlock.revoke`
  when connected; deletes the key either way). Unpairing a PC removes its enrollments.

## Credential provider ↔ host pipe

`\\.\pipe\MediaDuet.Unlock.<sessionId>` (decimal Windows session id). Created by the host
with DACL `D:P(A;;GA;;;SY)` and `PIPE_REJECT_REMOTE_CLIENTS`; the host additionally checks
(after the first line, with `ImpersonateNamedPipeClient`) that the client token is
LocalSystem. The credential provider opens it with `SECURITY_IDENTIFICATION` and checks
that the server process runs in its session as the tile's user.

If the per-session host pipe is absent, the provider connects to
`\\.\pipe\MediaDuet.Unlock.Boot.<userSid>`. It authenticates the server PID against
SCM's running `MediaDuetUnlock` own-process LocalSystem service. The service's pipe
ACL permits SYSTEM only; after the first read it impersonates the client for a
SYSTEM-token check and gets the real client session with `GetNamedPipeClientSessionId`.
It accepts requests only from the active physical console, either before sign-in
or for the tile's same-SID locked user when the tray host pipe is absent. Unknown
state, another session/user, or an unlocked console fails closed. If the tray host
starts while the boot pipe is open, the provider retries its preferred host pipe.

The authenticated `hello` and the JSON-lines messages below are shared by both pipes.
The service preconnects the last Windows user's enrolled phone while the console
is eligible, or the sole enrolled phone if no previous-user hint matches. Opening
another enrolled user's tile also preconnects that user's link. This only completes
the encrypted link handshake: it never requests phone authentication or releases K.
Selecting the tile sends the actual `unlock.request` over the existing link; a retry
expedites reconnection rather than waiting for a background backoff. Leaving the tile
cancels the approval while retaining the warm link until the console becomes ineligible.
It discovers the phone through DNS-SD and cached LAN endpoints,
with native RFCOMM as a fallback; no WinRT user-profile or interactive desktop is
needed for transport. Every connection uses the existing authenticated/encrypted
link protocol, including peer identity and request correlation checks.

UTF-8 JSON, one object per line.

Credential provider → host:

| Type | Body |
|------|------|
| `hello` | `{"v":1, "sid":"S-1-5-21-…"}` — the user of the tile |
| `request` | `{}` — ask the phone (the tile was selected, or "Send to phone again") |
| `cancel` | `{}` — the tile was left |
| `result` | `{"ok":bool, "status":int}` — what Windows said (`ReportResult` NTSTATUS) |

Host → credential provider:

| Type | Body |
|------|------|
| `status` | `{"state", "phone"?, "code"?, "lang"}` — `state` ∈ `not_enrolled` (hide the tile), `other_user` (hide), `offline` (phone not connected; a pending request is sent when it connects), `ready`, `waiting` (prompt on the phone), `denied`, `failed` (`code`: link error code), `stale` (set up again in MediaDuet), `unlocking`. `lang`: `zh-CN` / `en-US` |
| `credential` | `{"password":"…"}` — once, right after the phone released K |

The tray host serves `request` only while its session is locked
(`WTSSessionInfoEx` → `WTS_SESSIONSTATE_LOCK`). The boot service instead requires a
pre-sign-in physical console and rechecks that condition before releasing a credential.
Signing in by another method cancels a pending request; late replies are discarded.

`result` with `ok:false` and `STATUS_LOGON_FAILURE` / `STATUS_WRONG_PASSWORD` marks
the enrollment **stale** (the Windows password changed): the tile says so and the
settings page asks to set up again.

## Elevated component setup

`MediaDuet.exe --unlock-component install <userSid>` / `remove <userSid>` (started with
`ShellExecuteEx "runas"`, exit code 0 = success):

* install: copies `MediaDuetUnlock.dll` from the exe's folder to
  `%ProgramFiles%\MediaDuet\MediaDuetUnlock.dll` (a DLL in use is renamed and deleted at
  reboot), registers `CLSID {91835FE0-2B7C-453B-926F-B67713F7DF99}` (InprocServer32,
  `ThreadingModel=Apartment`) and
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{…}`,
  writes `HKLM\SOFTWARE\MediaDuet\Unlock` `Version` = app version and adds
  `Users\<userSid>`. It also copies the service executable into the protected install
  folder and creates/updates `MediaDuetUnlock` as an **automatic (not delayed)**
  LocalSystem own-process service, depending on Tcpip and Dnscache, with restart
  recovery actions. The quoted service binary path never points into a user-writable
  installation. Existing services are stopped before replacing the executable.
* install can additionally receive a protected provisioning-file path. The helper
  validates the record's SID and schema, stores it atomically, records `BootVersion`
  for that user, and waits until the service reports running.
* remove: deletes the user's boot record and `Users\<userSid>`; when no user is left,
  stops/deletes the service, unregisters the provider, and removes both binaries.

The credential provider shows its tile only for users listed under `Users`.
It declares its first visible tile as a default with automatic submission disabled
until phone approval supplies a credential. This lets Windows prefer the last
successfully used provider on subsequent sign-ins. Selecting MediaDuet and completing
one sign-in is the normal way to establish that preference. No system-wide default
provider policy or exclusion of other sign-in options is installed. Microsoft's
`DefaultCredentialProvider` policy explicitly describes the Other User tile, so it
must not be presented as a guarantee for every current-user sign-in configuration.

The service also exposes `\\.\pipe\MediaDuet.Unlock.Control.<userSid>`, readable and
writable only by SYSTEM and that SID. The normal-user host authenticates the server
against SCM before sending `status` or `remove`. This channel returns only enrollment
status, stale state and last sign-in time; it never returns passwords or pairing keys.
Removal deletes the service record before optional component-removal UAC, so cancelling
that UAC cannot restore usable boot credentials. If the service cannot acknowledge
revocation, elevated removal must succeed before the UI reports disabled.

`MediaDuet.exe --unlock-remove` is the unelevated cleanup entry used by both Windows
uninstallers. It revokes the current user's boot enrollment and offers elevated
component removal. Uninstall aborts if neither service revocation nor elevated removal
succeeds. Other users' enrollments are retained.
Revocation also records a protected per-user `Revoked` marker in the component's
HKLM registration. The tray host checks it on startup and during status refresh,
so an old local seal cannot be silently re-provisioned after phone-side revocation
while the tray host was not running. Successful explicit provisioning clears it.

## Windows UI ↔ host IPC

UI → host (all with `rid`, answered with `result`):

| Type | Body | Errors (`result.error`) |
|------|------|------------------------|
| `unlock.enable` | `{"password"}` — verify, enroll with the connected phone, then install/update and provision the component and boot service (UAC) | `wrong_password`, `no_phone` (no connected phone announced `unlock`), `elevation_cancelled`, `component_failed`, `phone_denied`, `phone_timeout`, `phone_no_auth`, `phone_failed`, `busy` |
| `unlock.disable` | `{}` — revoke boot access, revoke on the phone (if connected), delete the local enrollment, then remove this user's component (UAC) | `elevation_cancelled` or `component_failed` if boot revocation cannot be confirmed; `busy` |
| `unlock.repair` | `{}` — reinstall / update the component only (UAC) | `elevation_cancelled`, `component_failed` |

`state.unlock`:

```json
"unlock": {
  "available": true,
  "enabled": true,
  "bootEnabled": true,
  "phone": {"id": "…", "name": "OPPO Find X8 Ultra"},
  "phoneConnected": true,
  "component": "ok",
  "stale": false,
  "setup": null,
  "lastUnlockAt": 1760000000000
}
```

* `available`: a connected phone announced `unlock` (setup possible now).
* `bootEnabled`: the running system service acknowledges this enrollment for pre-sign-in use.
* `phone`: the enrolled phone, `null` when not enabled. `phoneConnected`: that phone is connected.
* `component`: `ok`, `missing` (not installed / not for this user), `outdated` (another version).
* `stale`: the last unlock failed with a wrong password, or the phone lost / removed its key.
* `setup`: `null`, `verifying`, `installing`, `waiting_phone` while `unlock.enable` runs.
* `lastUnlockAt`: wall ms of the last unlock through the phone, or `null`.

## Building and checking the Windows implementation

The CMake build produces `MediaDuet.exe`, `MediaDuetUnlock.dll` and
`MediaDuetUnlockService.exe`. Ship the DLL and service beside the host executable;
`windows/packaging/package.ps1` includes both in the
portable folder and installer payload. The app copies it into the protected
Program Files directory only when the user enables or repairs phone unlock.

In a Visual Studio Developer PowerShell, from the repository root:

```powershell
cmake -S windows -B windows/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build windows/build --target MediaDuet MediaDuetUnlock MediaDuetUnlockService md_tests md_unlock_tests md_boot_tests
ctest --test-dir windows/build --output-on-failure
msbuild windows/ui/MediaDuetUI.vcxproj -restore -p:Configuration=Release -p:Platform=x64
python windows/ui/tools/gen_resw.py
```

`md_tests` covers sealing, tampering, enrollment/user binding and protocol bodies.
`md_unlock_tests` exercises the actual credential-provider COM implementation with
synthetic credentials, without registering the DLL or locking the machine. It
checks relative-buffer serialization, protected credentials for the unified logon
scenario, one-time submission, deselection/failure cleanup, user/session checks
  on a temporary pipe, and COM lifetime/scenario handling.
`md_boot_tests` covers pre-sign-in session gating, protected provisioning/restoration,
boot record validation, password release after a matching approval, denial of foreign,
cancelled and late replies, stale-password persistence, request timeouts, and native
TCP delivery including data received immediately before EOF. It uses synthetic
credentials and injected session/storage boundaries; it does not register a real
provider, alter the machine's service configuration, or reboot Windows.

The settings entry is under **Devices & pairing → Unlock this PC with your phone**.
For UI development, the fake host accepts
`--unlock unavailable|ready|enabled|offline|stale|component|waiting` on a private
`--pipe`. Its setup/remove/repair actions only simulate state changes. Use synthetic
passwords for this tool; password fields are redacted from its logs.

Before releasing, validate on a Windows test machine with the Android app:

1. Set up using the Windows account password, accept UAC, and confirm enrollment
   on the phone. Check that `unlock.json` contains a protected seal and no password
   or phone key, and that ordinary users cannot modify the installed DLL or folder.
2. Lock with Win+L, select the MediaDuet sign-in option, and confirm on the phone.
   Repeat after sleep and check the last-unlock time.
3. Cancel the phone prompt, let it time out, resend, switch to another sign-in
   option, and unlock with a different method while the phone prompt is pending.
   Check that cancelled or late replies cannot submit credentials.
4. Disconnect/reconnect the enrolled phone while locked. An unrelated paired
   phone must not release a credential. A phone without the `unlock` capability
   must not enable setup.
5. Change the Windows password, remove the phone key, and revoke/unpair on either
   end. Check stale/setup-again or disabled state as appropriate.
6. Repair/update the DLL and service while LogonUI has the DLL loaded. With two enrolled Windows
   users, removing one user must retain the other user's registration. Removing
   the final user unregisters the component. Cancelling removal UAC still removes
   the local enrollment and hides the tile.
7. Restart and perform a cold boot without starting the tray host. Confirm with
   `sc.exe qc MediaDuetUnlock` that startup is AUTO_START and the account is LocalSystem,
   and with `sc.exe query MediaDuetUnlock` that it is RUNNING. At the Windows sign-in
   screen select MediaDuet, approve on the paired phone, and verify successful sign-in.
   Repeat after sign-out, with the phone initially offline then reconnected, and
   using Bluetooth when available. With a per-user Wi-Fi profile or unavailable
   phone, verify an offline/timeout state and successful fallback to Windows sign-in.
   Also test Windows automatic restart sign-in restoration with tray autostart off;
   the same-SID locked-console fallback must remain usable.
8. Change the password, delete/revoke enrollment, disable the feature or uninstall,
   then restart. No revoked enrollment may release a credential. Repeat with a
   second enrolled Windows account to check that removing one user retains the other.
9. Check Windows Hello-only/passwordless account policies and domain policies on
   the actual deployment. This provider submits an account password; it does not
   convert a Windows Hello PIN into a reusable password or bypass policy.

These interactive checks are separate from the automated tests; compiling and
passing the tests alone does not verify the LogonUI/phone/UAC integration.
