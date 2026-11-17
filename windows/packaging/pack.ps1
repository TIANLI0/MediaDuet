<#
.SYNOPSIS
  Packages the staged folder dist\MediaDuet-<version>-win-<arch> (made by package.ps1): code signing, zip, installer.

.DESCRIPTION
  1. Code signing, only when the TIANLI0/codesign action has signed in to the Certum service (release workflow;
     it sets CODESIGN_SIGN and CODESIGN_THUMBPRINT). Local and CI builds stay unsigned. Only MediaDuet's own
     binaries are signed: the Windows App SDK files next to them already carry Microsoft's signature.
  2. dist\MediaDuet-<version>-win-<arch>.zip (portable).
  3. dist\MediaDuet-<version>-<arch>-setup.exe with FluentSetup; when signing, FluentSetup signs setup.exe and the
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
if (-not (Test-Path $dist)) { throw "Staged folder not found: $dist (run package.ps1 first)" }

$sign = [bool]$env:CODESIGN_SIGN
if ($sign) {
    & $env:CODESIGN_SIGN -Root $dist -Description 'MediaDuet' -DescriptionUrl 'https://github.com/TIANLI0/MediaDuet' -Path @(
        'MediaDuet.exe',              # host
        'MediaDuetUI.exe',            # WinUI front end
        'MediaDuetUnlock.dll',        # credential provider, loaded by LogonUI as SYSTEM
        'MediaDuetUnlockService.exe') # unlock service
}

$zip = "$dist.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip
"Staged: $dist"
"Zip:    $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)

# Installer (FluentSetup), if the CLI is available.
$fluentsetup = if ($env:FLUENTSETUP) { $env:FLUENTSETUP } else { (Get-Command fluentsetup -ErrorAction SilentlyContinue).Source }
if ($fluentsetup) {
    $fsArch = $Arch.ToLowerInvariant()
    $signArgs = if ($sign) { @('--sign', '-D', "CodesignThumbprint=$env:CODESIGN_THUMBPRINT") } else { @() }
    & $fluentsetup build (Join-Path $PSScriptRoot 'fluentsetup.json') -D "Version=$Version" -D "Arch=$Arch" --arch $fsArch @signArgs -o (Join-Path $root 'dist')
    if ($LASTEXITCODE) { throw 'FluentSetup failed' }
    "Setup:  " + (Join-Path $root "dist\MediaDuet-$Version-$fsArch-setup.exe")
} elseif ($sign) {
    throw 'fluentsetup not found: a signed release needs the installer.'
} else {
    'fluentsetup not found: skipped setup.exe (install FluentSetup from https://github.com/TIANLI0/FluentSetup/releases or set FLUENTSETUP).'
}
