<#
.SYNOPSIS
  Builds MediaDuet for Windows (Release) and stages a portable folder + zip.

.DESCRIPTION
  1. CMake/Ninja build of the native host (MediaDuet.exe), probe and unit tests.
  2. MSBuild of the WinUI 3 UI (unpackaged), twice: self-contained (Windows App SDK included) and framework-dependent
     on the Windows App Runtime ("shared").
  3. Runs the unit tests.
  4. Copies everything into dist\MediaDuet-<version>-win-<arch>\ (self-contained) and
     dist\MediaDuet-<version>-win-<arch>-shared\.
  5. pack.ps1: signs the own binaries of both (release builds), zips the self-contained one (the portable copy), and
     builds dist\MediaDuet-<version>-<arch>-setup.exe from both staged folders with FluentSetup: setup installs the
     shared build where the Windows App Runtime 2.5.1+ is already present, else the self-contained one
     (fluentsetup.json; the FluentSetup CLI from $env:FLUENTSETUP, else fsetup or, before 1.5, fluentsetup on
     PATH, see https://github.com/TIANLI0/FluentSetup/releases; CI uses the TIANLI0/FluentSetup action).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File windows\packaging\package.ps1 -Arch x64
#>
param(
    [ValidateSet('x64', 'ARM64')] [string]$Arch = 'x64',
    [string]$Version = '1.13.0',
    [switch]$SkipTests,
    # Build and stage only; pack.ps1 then signs (release builds), zips and builds the installer.
    [switch]$NoPackage
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
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'Unit tests failed' }
}

$uiProj = Join-Path $win 'ui\MediaDuetUI.vcxproj'
# The UI twice, the installer's two variants (fluentsetup.json): self-contained (Windows App SDK included) into
# bin\<arch>\Release, and "shared" (uses the Windows App Runtime installed on the PC) into bin\<arch>\Release-shared.
# A clean rebuild: an incremental build once kept objects compiled against an older Model.h,
# and the mixed struct layouts crashed the settings page.
$uiBuilds = @(
    @{ SelfContained = 'true'; Out = Join-Path $win "ui\bin\$Arch\Release"; Dist = "MediaDuet-$Version-win-$Arch" },
    @{ SelfContained = 'false'; Out = Join-Path $win "ui\bin\$Arch\Release-shared"; Dist = "MediaDuet-$Version-win-$Arch-shared" }
)
foreach ($ui in $uiBuilds) {
    msbuild $uiProj -restore -t:Rebuild -m -p:Configuration=Release -p:Platform=$Arch "-p:WindowsAppSDKSelfContained=$($ui.SelfContained)" -v:minimal
    if ($LASTEXITCODE) { throw "WinUI build failed (WindowsAppSDKSelfContained=$($ui.SelfContained))" }
    if (-not (Test-Path (Join-Path $ui.Out 'MediaDuetUI.exe'))) { throw "MediaDuetUI.exe not found in $($ui.Out)" }
}

$distRoot = [IO.Path]::GetFullPath((Join-Path $root 'dist')) + [IO.Path]::DirectorySeparatorChar
foreach ($ui in $uiBuilds) {
    $shared = $ui.SelfContained -ne 'true'
    $dist = [IO.Path]::GetFullPath((Join-Path $distRoot $ui.Dist))
    if (-not $dist.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Staging path is outside dist' }
    if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
    New-Item -ItemType Directory -Force $dist | Out-Null
    # UI first (the self-contained one brings the Windows App SDK runtime), then the host.
    Copy-Item (Join-Path $ui.Out '*') $dist -Recurse -Force
    Copy-Item (Join-Path $build 'MediaDuet.exe') $dist
    Copy-Item (Join-Path $build 'MediaDuetUnlock.dll') $dist
    Copy-Item (Join-Path $build 'MediaDuetUnlockService.exe') $dist
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
    # The shared build must not carry the Windows App SDK (it would shadow the installed runtime) and needs the
    # bootstrapper that finds that runtime; the self-contained build is the other way round.
    if ($shared) {
        if (Test-Path (Join-Path $dist 'Microsoft.ui.xaml.dll')) { throw "$dist is not framework-dependent (has Microsoft.ui.xaml.dll)" }
        if (-not (Test-Path (Join-Path $dist 'Microsoft.WindowsAppRuntime.Bootstrap.dll'))) { throw "$dist is missing Microsoft.WindowsAppRuntime.Bootstrap.dll" }
    } else {
        foreach ($f in 'Microsoft.ui.xaml.dll', 'Microsoft.WindowsAppRuntime.dll', 'Microsoft.UI.Xaml.Phone.dll') {
            if (-not (Test-Path (Join-Path $dist $f))) { throw "$dist is missing $f" }
        }
    }
    "Staged: $dist"
}

# Packaging (code signing for releases, zip, installer) is pack.ps1, so the release workflow can sign in to the
# code-signing service between building and packaging.
if ($NoPackage) { return }
& (Join-Path $PSScriptRoot 'pack.ps1') -Arch $Arch -Version $Version
