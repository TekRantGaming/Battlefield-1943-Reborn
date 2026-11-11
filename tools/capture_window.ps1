# Saves a PNG of one process's own main window with PrintWindow (PW_RENDERFULLCONTENT), without activating it or
# touching the desktop (GAME-RULES.md rule 7). Usage: capture_window.ps1 -ProcessId <pid> -Out <file.png>
param([Parameter(Mandatory = $true)][int]$ProcessId, [Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("BfCapture" -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Runtime.InteropServices;
public static class BfCapture {
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static Bitmap Grab(IntPtr h) {
    SetProcessDpiAwarenessContext(new IntPtr(-4));  // per monitor v2: real pixel sizes, not the scaled ones
    RECT r; GetClientRect(h, out r);
    int w = Math.Max(1, r.R - r.L), ht = Math.Max(1, r.B - r.T);
    var bmp = new Bitmap(w, ht);
    using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
    return bmp;
  }
}
"@
}
$p = Get-Process -Id $ProcessId
if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
$bmp = [BfCapture]::Grab($p.MainWindowHandle)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
