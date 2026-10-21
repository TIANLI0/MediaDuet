<#
.SYNOPSIS
  Builds MediaDuet for Windows (Release) and stages a portable folder + zip.

.DESCRIPTION
  1. CMake/Ninja build of the native host (MediaDuet.exe), probe and unit tests.
  2. MSBuild of the WinUI 3 UI (self-contained Windows App SDK, unpackaged).
  3. Runs the unit tests.
  4. Copies everything into dist\MediaDuet-<version>-win-<arch>\ and zips it.
  An Inno Setup script (installer.iss) can turn the staged folder into a setup.exe.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File windows\packaging\package.ps1 -Arch x64
#>
param(
    [ValidateSet('x64', 'ARM64')] [string]$Arch = 'x64',
    [string]$Version = '1.6.0',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$win = Join-Path $root 'windows'

function Enter-DevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }
    $hostArch = 'amd64'
    $target = if ($Arch -eq 'ARM64') { 'arm64' } else { 'amd64' }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch $target -HostArch $hostArch -SkipAutomaticLocation | Out-Null
}

Enter-DevShell
$build = Join-Path $win "build-release-$Arch"
$tests = if ($SkipTests) { 'OFF' } else { 'ON' }
cmake -S $win -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DMD_BUILD_TESTS=$tests"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'Host build failed' }
if (-not $SkipTests) {
    & (Join-Path $build 'md_tests.exe')
    if ($LASTEXITCODE) { throw 'Unit tests failed' }
}

$uiProj = Join-Path $win 'ui\MediaDuetUI.vcxproj'
msbuild $uiProj -restore -m -p:Configuration=Release -p:Platform=$Arch -v:minimal
if ($LASTEXITCODE) { throw 'WinUI build failed' }
$uiExe = Get-ChildItem (Join-Path $win 'ui') -Recurse -Filter MediaDuetUI.exe |
    Where-Object { $_.FullName -match "\\$Arch\\Release\\" -and $_.FullName -notmatch '\\obj\\' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $uiExe) { throw 'MediaDuetUI.exe not found after build' }

$dist = Join-Path $root "dist\MediaDuet-$Version-win-$Arch"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null
# UI first (it brings the self-contained Windows App SDK runtime), then the host.
Copy-Item (Join-Path $uiExe.DirectoryName '*') $dist -Recurse -Force
Copy-Item (Join-Path $build 'MediaDuet.exe') $dist
Copy-Item (Join-Path $win 'app\resources\app_512.png') (Join-Path $dist 'MediaDuet.png')
Copy-Item (Join-Path $root 'LICENSE') $dist
Copy-Item (Join-Path $root 'README.md') $dist
Copy-Item (Join-Path $root 'docs\THIRD_PARTY_NOTICES.md') $dist -ErrorAction SilentlyContinue
Get-ChildItem $dist -Include *.pdb, *.lib, *.exp, *.winmd -Recurse | Remove-Item -Force
# Trim the self-contained Windows App SDK to what MediaDuet uses (checked with the off-screen
# render test: main window, settings pages, mini player):
#  * WinUI's own translations only for the languages the UI has (English, Chinese); others fall
#    back to English,
#  * WebView2 interop, push-notification and restart helpers, the design-time DLL and the map
#    control's page: none of these features are used.
#  Microsoft.UI.Xaml.Phone.dll stays: the settings controls need it.
$keepLanguages = 'en-us', 'en-GB', 'zh-CN', 'zh-TW'
Get-ChildItem $dist -Directory | Where-Object {
    $_.Name -match '^[a-z]{2,3}(-[A-Za-z]{2,4})*(-[A-Za-z0-9]+)?$' -and $keepLanguages -notcontains $_.Name -and
    (Test-Path (Join-Path $_.FullName 'Microsoft.ui.xaml.dll.mui'))
} | Remove-Item -Recurse -Force
foreach ($f in 'Microsoft.Web.WebView2.Core.dll', 'PushNotificationsLongRunningTask.ProxyStub.dll', 'RestartAgent.exe',
                'Microsoft.UI.Designer.dll') {
    Remove-Item (Join-Path $dist $f) -Force -ErrorAction SilentlyContinue
}
Remove-Item (Join-Path $dist 'Microsoft.UI.Xaml') -Recurse -Force -ErrorAction SilentlyContinue
# Lyrics typeface (HarmonyOS Sans SC, Assets\Fonts via the UI output): shipped unmodified and only
# together with its licence (the in-app notice is in Settings > About).
foreach ($f in 'HarmonyOS_Sans_SC_Medium.ttf', 'HarmonyOS_Sans_SC_Bold.ttf', 'HarmonyOS_Sans_LICENSE.txt') {
    $p = Join-Path $dist "Assets\Fonts\$f"
    if (-not (Test-Path $p)) { throw "Staged UI is missing Assets\Fonts\$f" }
    $src = Join-Path $win "ui\Assets\Fonts\$f"
    if ((Get-FileHash $p).Hash -ne (Get-FileHash $src).Hash) { throw "Assets\Fonts\$f differs from the original" }
}
if (-not (Test-Path (Join-Path $dist 'Assets\Developer.png'))) { throw 'Staged UI is missing Assets\Developer.png' }

$zip = "$dist.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip
"Staged: $dist"
"Zip:    $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)

# Installer (NSIS 3), if available.
$nsis = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source
if (-not $nsis -and (Test-Path "${env:ProgramFiles(x86)}\NSIS\Bin\makensis.exe")) { $nsis = "${env:ProgramFiles(x86)}\NSIS\Bin\makensis.exe" }
if ($nsis) {
    & $nsis /V2 /INPUTCHARSET UTF8 "/DVERSION=$Version" "/DARCH=$Arch" (Join-Path $PSScriptRoot 'installer.nsi')
    if ($LASTEXITCODE) { throw 'NSIS failed' }
    "Setup:  " + (Join-Path $root "dist\MediaDuet-$Version-$Arch-setup.exe")
} else {
    'NSIS not found: skipped setup.exe (install NSIS 3 or use installer.iss with Inno Setup).'
}
