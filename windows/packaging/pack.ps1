<#
.SYNOPSIS
  Packages the staged folders dist\MediaDuet-<version>-win-<arch> and ...-win-<arch>-shared (made by package.ps1):
  code signing, zip, installer.

.DESCRIPTION
  1. Code signing, only when the TIANLI0/codesign action has signed in to the Certum service (release workflow;
     it sets CODESIGN_SIGN and CODESIGN_THUMBPRINT). Local and CI builds stay unsigned. Only MediaDuet's own
     binaries are signed: the Windows App SDK files next to them already carry Microsoft's signature.
     Both builds are signed: the self-contained one (zip, installer default) and the shared one (installer, on PCs
     with the Windows App Runtime).
  2. dist\MediaDuet-<version>-win-<arch>.zip (portable): the self-contained build only, it has to run on any PC.
  3. dist\MediaDuet-<version>-<arch>-setup.exe with FluentSetup from both folders; when signing, FluentSetup signs setup.exe and the
     uninstall.exe it installs (fluentsetup.json "signing", thumbprint passed as -D CodesignThumbprint).
  package.ps1 runs this after staging; the release workflow runs package.ps1 -NoPackage, signs in, then runs this.
#>
param(
    [ValidateSet('x64', 'ARM64')] [string]$Arch = 'x64',
    [Parameter(Mandatory)] [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dist = [IO.Path]::GetFullPath((Join-Path $root "dist\MediaDuet-$Version-win-$Arch"))
$distShared = "$dist-shared"
foreach ($d in $dist, $distShared) {
    if (-not (Test-Path $d)) { throw "Staged folder not found: $d (run package.ps1 first)" }
}

$sign = [bool]$env:CODESIGN_SIGN
if ($sign) {
    $description = @{ Description = 'MediaDuet'; DescriptionUrl = 'https://github.com/TIANLI0/MediaDuet' }
    & $env:CODESIGN_SIGN -Root $dist @description -Path @(
        'MediaDuet.exe',              # host
        'MediaDuetUI.exe',            # WinUI front end
        'MediaDuetUnlock.dll',        # credential provider, loaded by LogonUI as SYSTEM
        'MediaDuetUnlockService.exe') # unlock service
    # The shared build has the same host binaries: copy the signed ones over (signing twice would make them differ,
    # and FluentSetup stores files identical in both builds only once). Its own front end is signed separately.
    foreach ($f in 'MediaDuet.exe', 'MediaDuetUnlock.dll', 'MediaDuetUnlockService.exe') {
        Copy-Item (Join-Path $dist $f) (Join-Path $distShared $f) -Force
    }
    & $env:CODESIGN_SIGN -Root $distShared @description -Path @('MediaDuetUI.exe')
}

$zip = "$dist.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip
"Staged: $dist"
"Zip:    $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)

# Installer (FluentSetup), if the CLI is available. FluentSetup 1.5 renamed it fsetup; fluentsetup stays an alias.
# Unchanged files reuse the compressed streams of earlier builds (%LocalAppData%\FluentSetup\cache\streams).
$fluentsetup = if ($env:FLUENTSETUP) { $env:FLUENTSETUP } else {
    (Get-Command fsetup, fluentsetup -ErrorAction SilentlyContinue | Select-Object -First 1).Source
}
if ($fluentsetup) {
    $fsArch = $Arch.ToLowerInvariant()
    $signArgs = if ($sign) { @('--sign', '-D', "CodesignThumbprint=$env:CODESIGN_THUMBPRINT") } else { @() }
    & $fluentsetup build (Join-Path $PSScriptRoot 'fluentsetup.json') -D "Version=$Version" -D "Arch=$Arch" --arch $fsArch @signArgs -o (Join-Path $root 'dist')
    if ($LASTEXITCODE) { throw 'FluentSetup failed' }
    "Setup:  " + (Join-Path $root "dist\MediaDuet-$Version-$fsArch-setup.exe")
} elseif ($sign) {
    throw 'fsetup not found: a signed release needs the installer.'
} else {
    'fsetup not found: skipped setup.exe (install FluentSetup from https://github.com/TIANLI0/FluentSetup/releases or set FLUENTSETUP).'
}
