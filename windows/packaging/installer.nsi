; MediaDuet Windows installer (NSIS 3). Per-user install, no admin rights.
; Build the staged folder first:
;   powershell -ExecutionPolicy Bypass -File windows\packaging\package.ps1 -Arch x64
; then:
;   makensis /DVERSION=1.9.0 /DARCH=x64 windows\packaging\installer.nsi

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "1.9.0"
!endif
!ifndef ARCH
  !define ARCH "x64"
!endif
!define STAGED "..\..\dist\MediaDuet-${VERSION}-win-${ARCH}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MediaDuet"

Name "MediaDuet"
OutFile "..\..\dist\MediaDuet-${VERSION}-${ARCH}-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\MediaDuet"
InstallDirRegKey HKCU "Software\MediaDuet" "InstallDir"
BrandingText "MediaDuet ${VERSION}"

!include "MUI2.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "..\app\resources\app.ico"
!define MUI_UNICON "..\app\resources\app.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\MediaDuet.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunNow)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

LangString RunNow ${LANG_SIMPCHINESE} "启动 MediaDuet"
LangString RunNow ${LANG_ENGLISH} "Start MediaDuet"
LangString NeedWin10 ${LANG_SIMPCHINESE} "MediaDuet 需要 Windows 10 2004 (19041) 或更高版本。"
LangString NeedWin10 ${LANG_ENGLISH} "MediaDuet requires Windows 10 version 2004 (19041) or later."
LangString Need64 ${LANG_SIMPCHINESE} "此安装包适用于 64 位 Windows。"
LangString Need64 ${LANG_ENGLISH} "This installer is for 64-bit Windows."
LangString RemoveData ${LANG_SIMPCHINESE} "是否同时删除配对信息、设置和缓存？$\n（选择“否”则保留，重新安装后无需再次配对）"
LangString RemoveData ${LANG_ENGLISH} "Also delete pairings, settings and cache?$\n(Choose No to keep them for a later reinstall.)"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "MediaDuet"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "MediaDuet Setup"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "Copyright (c) 2026 tianli. MIT License."
VIAddVersionKey /LANG=${LANG_ENGLISH} "CompanyName" "tianli"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(Need64)"
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastBuild} 19041
    MessageBox MB_ICONSTOP "$(NeedWin10)"
    Abort
  ${EndIf}
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

; In-app update (Settings > About): `setup.exe /S /UPDATE` installs silently, then starts
; MediaDuet again (a plain silent install does not).
Function .onInstSuccess
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/UPDATE" $R1
  ${IfNot} ${Errors}
    Exec '"$INSTDIR\MediaDuet.exe"'
  ${EndIf}
FunctionEnd

!macro StopApp
  ; Graceful first (the host handles WM_CLOSE), then force.
  nsExec::Exec 'taskkill /IM MediaDuetUI.exe'
  nsExec::Exec 'taskkill /IM MediaDuet.exe'
  Sleep 1500
  nsExec::Exec 'taskkill /F /IM MediaDuetUI.exe'
  nsExec::Exec 'taskkill /F /IM MediaDuet.exe'
!macroend

Section "MediaDuet" SecMain
  SectionIn RO
  !insertmacro StopApp
  SetOutPath "$INSTDIR"
  File /r "${STAGED}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\MediaDuet"
  CreateShortcut "$SMPROGRAMS\MediaDuet\MediaDuet.lnk" "$INSTDIR\MediaDuet.exe"
  CreateShortcut "$SMPROGRAMS\MediaDuet\Uninstall MediaDuet.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\MediaDuet" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "MediaDuet"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "tianli"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\MediaDuet.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ; Autostart is managed by the app itself (Settings > General) via the HKCU Run key.
SectionEnd

Section "Uninstall"
  !insertmacro StopApp
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "MediaDuet"
  RMDir /r "$SMPROGRAMS\MediaDuet"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\MediaDuet"
  IfSilent done
  MessageBox MB_YESNO|MB_ICONQUESTION "$(RemoveData)" IDNO done
    RMDir /r "$LOCALAPPDATA\MediaDuet"
  done:
SectionEnd
