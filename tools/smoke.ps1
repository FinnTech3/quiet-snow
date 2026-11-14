# Starts the built Windows app and checks what it actually put on the desktop. Exits 1 if anything is wrong.
param([string]$Exe = ".\QuietSnow.exe")
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint tid, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string cls, string title);
    public static string FindMessageWindow(string cls, out IntPtr found) {
        found = FindWindowEx(new IntPtr(-3), IntPtr.Zero, cls, null);   // HWND_MESSAGE: message-only windows
        return "error " + Marshal.GetLastWin32Error();
    }
    public static int CountMessageWindows() {
        int n = 0;
        IntPtr h = IntPtr.Zero;
        while (n < 1000 && (h = FindWindowEx(new IntPtr(-3), h, null, null)) != IntPtr.Zero) n++;
        return n;
    }
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static List<IntPtr> OfProcess(uint pid) {
        var l = new List<IntPtr>();
        EnumWindows((h, _) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) l.Add(h); return true; }, IntPtr.Zero);
        return l;
    }
    public static List<IntPtr> OfThread(uint tid) {
        var l = new List<IntPtr>();
        EnumThreadWindows(tid, (h, _) => { l.Add(h); return true; }, IntPtr.Zero);
        return l;
    }
    public static string Cls(IntPtr h) { var s = new StringBuilder(64); GetClassName(h, s, 64); return s.ToString(); }
}
"@
$fail = 0
function Check($ok, $what) { if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:fail++ } }

$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 5
$p.Refresh()
Check (-not $p.HasExited) "still running after 5 seconds"
$wins = [Win]::OfProcess([uint32]$p.Id)
$ov = @($wins | Where-Object { [Win]::Cls($_) -eq "QuietSnowOverlay" })
Check ($ov.Count -ge 1) "an overlay window exists ($($ov.Count) found)"
$GWL_EXSTYLE = -20
$WS_EX_TRANSPARENT = 0x20; $WS_EX_LAYERED = 0x80000; $WS_EX_TOPMOST = 0x8; $WS_EX_TOOLWINDOW = 0x80; $WS_EX_NOACTIVATE = 0x08000000
foreach ($h in $ov) {
    $ex = [int64][Win]::GetWindowLongPtr($h, $GWL_EXSTYLE)
    Check ([bool]([Win]::IsWindowVisible($h))) "overlay is visible"
    Check (($ex -band $WS_EX_TRANSPARENT) -ne 0) "overlay is click-through (WS_EX_TRANSPARENT)"
    Check (($ex -band $WS_EX_LAYERED) -ne 0) "overlay is layered"
    Check (($ex -band $WS_EX_TOPMOST) -ne 0) "overlay is topmost"
    Check (($ex -band $WS_EX_NOACTIVATE) -ne 0) "overlay never takes focus (WS_EX_NOACTIVATE)"
    Check (($ex -band $WS_EX_TOOLWINDOW) -ne 0) "overlay stays out of Alt-Tab and the taskbar (WS_EX_TOOLWINDOW)"
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    Check ($w -ge 640 -and $ht -ge 480) "overlay covers the display ($w x $ht)"
    Check ($r.L -eq 0 -and $r.T -eq 0 -and $w -eq [Win]::GetSystemMetrics(0) -and $ht -eq [Win]::GetSystemMetrics(1)) "overlay is exactly the size and place of the primary display"
}
# The tray host is a message-only window, which EnumWindows does not list: ask for it by class under HWND_MESSAGE.
$host_ = [IntPtr]::Zero
$why = [Win]::FindMessageWindow("QuietSnowHost", [ref]$host_)
"      message-only windows on this desktop: $([Win]::CountMessageWindows()); lookup said: $why"
Check ($host_.ToInt64() -ne 0) "tray host window exists"

# It must not flicker: the overlay stays visible, sample after sample, for 12 seconds.
$gaps = 0
for ($i = 0; $i -lt 24; $i++) {
    Start-Sleep -Milliseconds 500
    $now = @([Win]::OfProcess([uint32]$p.Id) | Where-Object { [Win]::Cls($_) -eq "QuietSnowOverlay" -and [Win]::IsWindowVisible($_) })
    if ($now.Count -lt $ov.Count) { $gaps++ }
}
Check ($gaps -eq 0) "overlay stayed visible for 12 seconds ($gaps gaps in 24 samples)"
Check (-not $p.HasExited) "still running after 17 seconds"

$second = Start-Process -FilePath $Exe -PassThru
$second.WaitForExit(5000) | Out-Null
Check $second.HasExited "a second copy exits at once (single instance)"

if ($host_.ToInt64() -ne 0) {
    [void][Win]::SendMessage($host_, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
    Start-Sleep -Seconds 2
    $p.Refresh()
    Check $p.HasExited "a close request ends the process cleanly"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# Desktop-only mode: the same program with the "scope" setting switched, as the tray menu does.
$key = "HKCU:\Software\FinnTech3\QuietSnow"
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name scope -Value 1 -Type DWord
$d = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 5
$d.Refresh()
Check (-not $d.HasExited) "desktop-only mode runs"
$dov = @([Win]::OfProcess([uint32]$d.Id) | Where-Object { [Win]::Cls($_) -eq "QuietSnowOverlay" -and [Win]::IsWindowVisible($_) })
Check ($dov.Count -ge 1) "desktop-only mode shows its overlay"
if (-not $d.HasExited) { Stop-Process -Id $d.Id -Force }
Remove-ItemProperty -Path $key -Name scope -ErrorAction SilentlyContinue

# Every snow type runs: the shaders compile and draw. The same program with the "snowType" setting switched.
foreach ($t in 0..5) {
    Set-ItemProperty -Path $key -Name snowType -Value $t -Type DWord
    $x = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Seconds 3
    $x.Refresh()
    Check (-not $x.HasExited) "snow type $t runs"
    if (-not $x.HasExited) { Stop-Process -Id $x.Id -Force }
}
Remove-ItemProperty -Path $key -Name snowType -ErrorAction SilentlyContinue

# A full screen app: desktop-only mode shows nothing while it is up; over-all mode keeps going.
Add-Type -AssemblyName System.Windows.Forms
function Pump($seconds) {
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 50 }
}
function Shown($proc) {
    @([Win]::OfProcess([uint32]$proc.Id) | Where-Object { [Win]::Cls($_) -eq "QuietSnowOverlay" -and [Win]::IsWindowVisible($_) }).Count
}
foreach ($mode in 1, 0) {
    Set-ItemProperty -Path $key -Name scope -Value $mode -Type DWord
    $m = Start-Process -FilePath $Exe -PassThru
    Pump 4
    $name = if ($mode -eq 1) { "desktop-only" } else { "over-all" }
    Check ((Shown $m) -ge 1) "$name mode shows snow with nothing full screen"
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = "None"
    $form.StartPosition = "Manual"
    $form.Bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $form.Show()
    Pump 4
    if ($mode -eq 1) { Check ((Shown $m) -eq 0) "desktop-only mode hides the snow while an app is full screen" }
    else { Check ((Shown $m) -ge 1) "over-all mode keeps the snow over a full screen app" }
    $form.Close()
    Pump 5
    Check ((Shown $m) -ge 1) "$name mode shows snow again when the full screen app closes"
    if (-not $m.HasExited) { Stop-Process -Id $m.Id -Force }
}
Remove-ItemProperty -Path $key -Name scope -ErrorAction SilentlyContinue
if ($fail -gt 0) { "$fail check(s) failed"; exit 1 }
"all checks passed"
