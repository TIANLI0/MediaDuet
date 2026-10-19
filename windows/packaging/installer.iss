; Inno Setup 6 script for MediaDuet (per-user install, no admin rights).
; Build the staged folder first:  windows\packaging\package.ps1 -Arch x64
; Then:  iscc /DArch=x64 /DVersion=1.5.0 windows\packaging\installer.iss

#ifndef Arch
  #define Arch "x64"
#endif
#ifndef Version
  #define Version "1.5.0"
#endif
#define Staged "..\..\dist\MediaDuet-" + Version + "-win-" + Arch

[Setup]
AppId={{7B0E6E52-6E5F-4C61-9D0E-3B2B8B7A6D21}
AppName=MediaDuet
AppVersion={#Version}
AppPublisher=tianli
DefaultDirName={localappdata}\Programs\MediaDuet
DefaultGroupName=MediaDuet
PrivilegesRequired=lowest
OutputDir=..\..\dist
OutputBaseFilename=MediaDuet-{#Version}-{#Arch}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed={#Arch}compatible
ArchitecturesInstallIn64BitMode={#Arch}compatible
MinVersion=10.0.19041
UninstallDisplayIcon={app}\MediaDuet.exe
CloseApplications=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#Staged}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\MediaDuet"; Filename: "{app}\MediaDuet.exe"
; Autostart is managed by the app itself (Settings > General), via the HKCU Run key.

[Run]
Filename: "{app}\MediaDuet.exe"; Description: "Start MediaDuet"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM MediaDuet.exe"; Flags: runhidden; RunOnceId: "StopHost"
Filename: "{sys}\taskkill.exe"; Parameters: "/IM MediaDuetUI.exe"; Flags: runhidden; RunOnceId: "StopUI"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "MediaDuet"; Flags: uninsdeletevalue dontcreatekey
