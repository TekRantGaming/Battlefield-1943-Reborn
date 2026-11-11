# Watches for test windows coming over the user's: every 100 ms, whether any visible bf1943.exe window is the
# foreground window or above it in the z order. Prints a count at the end (and the first few times it happened).
# Usage: powershell -File tools\window_watch.ps1 -Seconds 60
param([int]$Seconds = 60)
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public static class Z {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    // Visible bf1943 windows above the foreground window, or the foreground itself; -1 when none.
    public static int Over(HashSet<uint> pids) {
        IntPtr fg = GetForegroundWindow();
        uint fgpid; GetWindowThreadProcessId(fg, out fgpid);
        if (pids.Contains(fgpid)) return 2;
        for (IntPtr h = GetTopWindow(IntPtr.Zero); h != IntPtr.Zero; h = GetWindow(h, 2)) {
            if (h == fg) return 0;
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pids.Contains(pid) && IsWindowVisible(h)) return 1;
        }
        return 0;
    }
}
'@
$over = 0; $focus = 0; $samples = 0; $first = @()
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-Process bf1943 -ErrorAction SilentlyContinue | ForEach-Object { [void]$pids.Add([uint32]$_.Id) }
    if ($pids.Count) {
        $samples++
        $r = [Z]::Over($pids)
        if ($r -eq 1) { $over++; if ($first.Count -lt 5) { $first += "above $(Get-Date -Format HH:mm:ss.fff)" } }
        if ($r -eq 2) { $focus++; if ($first.Count -lt 5) { $first += "focus $(Get-Date -Format HH:mm:ss.fff)" } }
    }
    Start-Sleep -Milliseconds 100
}
"samples with a game running: $samples; above the user's window: $over; focused: $focus"
$first
