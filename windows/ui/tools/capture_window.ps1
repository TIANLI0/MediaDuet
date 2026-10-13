<#
.SYNOPSIS
  Saves a PNG of a top-level window (test helper used with fake_host.py).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -Title "MediaDuet" -Out shot.png
  powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -Title "MediaDuet" -Pick smallest -Out compact.png
#>
param(
    [string]$Title = "",
    [string]$ProcessName = "MediaDuetUI",
    [int]$ProcessId = 0,   # preferred: capture only windows of this process
    # popup = second-largest window of the process (an open flyout next to the main window)
    [ValidateSet("first", "largest", "smallest", "popup")][string]$Pick = "first",
    [Parameter(Mandatory = $true)][string]$Out
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class CaptureNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr, out RECT rect, int size);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    public static List<IntPtr> Find(uint[] pids, string title) {
        var result = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (Array.IndexOf(pids, pid) < 0 || !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512); GetWindowText(h, sb, 512);
            if (title.Length == 0 || sb.ToString().Contains(title)) result.Add(h);
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@

[CaptureNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
if ($ProcessId -gt 0) { $pids = @([uint32]$ProcessId) }
else { $pids = @(Get-Process -Name $ProcessName -ErrorAction Stop | ForEach-Object { [uint32]$_.Id }) }
$windows = [CaptureNative]::Find($pids, $Title)
if ($windows.Count -eq 0) { throw "No visible window found (process '$ProcessName', title '$Title')" }
$hwnd = $windows[0]
if ($Pick -ne "first") {
    $sized = foreach ($h in $windows) {
        $r = New-Object CaptureNative+RECT
        [CaptureNative]::GetWindowRect($h, [ref]$r) | Out-Null
        [pscustomobject]@{ H = $h; Area = [int64]($r.Right - $r.Left) * ($r.Bottom - $r.Top) }
    }
    $sorted = $sized | Sort-Object Area
    $hwnd = switch ($Pick) {
        "largest" { $sorted[-1].H }
        "popup" { if (@($sorted).Count -ge 2) { $sorted[-2].H } else { throw "no popup window open" } }
        default { $sorted[0].H }
    }
}

$win = New-Object CaptureNative+RECT
[CaptureNative]::GetWindowRect($hwnd, [ref]$win) | Out-Null
$frame = New-Object CaptureNative+RECT
[CaptureNative]::DwmGetWindowAttribute($hwnd, 9, [ref]$frame, 16) | Out-Null  # DWMWA_EXTENDED_FRAME_BOUNDS

$w = $win.Right - $win.Left; $h = $win.Bottom - $win.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[CaptureNative]::PrintWindow($hwnd, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()

# Crop the invisible resize borders away.
$crop = New-Object System.Drawing.Rectangle ($frame.Left - $win.Left), ($frame.Top - $win.Top), ($frame.Right - $frame.Left), ($frame.Bottom - $frame.Top)
if ($crop.Width -le 0 -or $crop.Height -le 0) { $crop = New-Object System.Drawing.Rectangle 0, 0, $w, $h }
$cropped = $bmp.Clone($crop, $bmp.PixelFormat)
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$cropped.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose(); $cropped.Dispose()
Write-Output "Saved $Out ($($crop.Width)x$($crop.Height))"
