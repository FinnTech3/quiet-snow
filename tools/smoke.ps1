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
}
# The tray host is a message-only window, which EnumWindows does not list: ask for it by class under HWND_MESSAGE.
$host_ = [IntPtr]::Zero
$why = [Win]::FindMessageWindow("QuietSnowHost", [ref]$host_)
"      message-only windows on this desktop: $([Win]::CountMessageWindows()); lookup said: $why"
Check ($host_.ToInt64() -ne 0) "tray host window exists"

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
if ($fail -gt 0) { "$fail check(s) failed"; exit 1 }
"all checks passed"
