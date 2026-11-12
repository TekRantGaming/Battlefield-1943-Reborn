# Watches one of TekRantGaming's play sessions (the test copy's watched start, tools\make_test_copy.ps1) until the game
# exits, into its session folder:
#   - shots\<time>.jpg: the game window every -Every seconds, and at each mark (PrintWindow, without activating it);
#   - marks.txt: each press of R on the keyboard in the game, with the time (TekRantGaming: "make the key press required for
#     the mark to be the letter R"), written by the game itself (BF_MARK_FILE, bf\src\playtest_mark.cpp); a
#     screenshot is taken when a mark appears. (Polling the keyboard from here saw none of TekRantGaming's presses.)
#   - system.csv: every 5 seconds the game's CPU use, memory, and the GPU's use, memory, temperature and clock;
#   - session.txt: what ran, and how the game ended.
# The game's log (bf1943.log, with BF hitch and native_x_renderer hitch lines) is written into the same folder by
# start.ps1. Times are the PC's local clock, as in the logs.
# Usage: playtest_watch.ps1 -ProcessId <game pid> -Dir <session folder> [-Every 10] [-Servers <servers folder>]
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Dir,
    [int]$Every = 10,
    [string]$Servers = ""
)
$ErrorActionPreference = "Stop"
$shots = Join-Path $Dir "shots"
New-Item -ItemType Directory -Force $shots | Out-Null
$marks = Join-Path $Dir "marks.txt"
$system = Join-Path $Dir "system.csv"
$session = Join-Path $Dir "session.txt"
function Stamp { (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff") }
function Note([string]$text) { Add-Content -Path $session -Value ("{0}  {1}" -f (Stamp), $text) -Encoding utf8 }

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Runtime.InteropServices;
public static class BfWatch {
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static void Init() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static bool Foreground(int pid) {
    uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p == (uint)pid;
  }
  public static Bitmap Grab(IntPtr h) {
    RECT r; GetClientRect(h, out r);
    int w = Math.Max(1, r.R - r.L), ht = Math.Max(1, r.B - r.T);
    var bmp = new Bitmap(w, ht);
    using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
    return bmp;
  }
}
"@
[BfWatch]::Init()
$jpeg = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$quality = New-Object System.Drawing.Imaging.EncoderParameters 1
$quality.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]85)

# A mark's clip: as many half-size frames as fit in 1.5 s, so a flicker of a frame or two can be stepped through.
function Clip([int]$mark) {
    try {
        $p = Get-Process -Id $ProcessId -ErrorAction Stop
        if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return }
        $dir = Join-Path $shots ("mark{0:D3}_" -f $mark + (Get-Date).ToString("HH-mm-ss.fff"))
        New-Item -ItemType Directory -Force $dir | Out-Null
        $end = (Get-Date).AddSeconds(1.5)
        $i = 0
        while ((Get-Date) -lt $end) {
            $t = (Get-Date).ToString("HH-mm-ss.fff")
            $bmp = [BfWatch]::Grab($p.MainWindowHandle)
            $half = New-Object System.Drawing.Bitmap $bmp, ([int]($bmp.Width / 2)), ([int]($bmp.Height / 2))
            $half.Save((Join-Path $dir ("{0:D3}_{1}.jpg" -f $i, $t)), $jpeg, $quality)
            $half.Dispose(); $bmp.Dispose()
            $i++
        }
        Add-Content -Path (Join-Path $Dir "shots.txt") -Value ("{0}  mark {1}: {2} frames in {3}" -f (Stamp), $mark, $i, $dir) -Encoding utf8
    } catch { Note "clip failed: $($_.Exception.Message)" }
}

function Shot([string]$why) {
    try {
        $p = Get-Process -Id $ProcessId -ErrorAction Stop
        if ($p.MainWindowHandle -eq [IntPtr]::Zero) { return }
        $name = (Get-Date).ToString("HH-mm-ss.fff") + $(if ($why) { "_$why" } else { "" }) + ".jpg"
        $bmp = [BfWatch]::Grab($p.MainWindowHandle)
        $bmp.Save((Join-Path $shots $name), $jpeg, $quality)
        $bmp.Dispose()
        # The capture itself may cost the game a frame: its time is in the log, so a hitch at that moment is known.
        Add-Content -Path (Join-Path $Dir "shots.txt") -Value ("{0}  {1}" -f (Stamp), $name) -Encoding utf8
    } catch { Note "screenshot failed: $($_.Exception.Message)" }
}

$game = Get-Process -Id $ProcessId
$null = $game.Handle  # held open, so the exit code can be read once it ends
Note "watching game PID $ProcessId; screenshots every $Every s; mark key R; servers: $Servers"
Set-Content -Path $system -Value "time,game_cpu_percent,game_working_set_mb,game_private_mb,gpu_percent,gpu_memory_mb,gpu_temp_c,gpu_clock_mhz" -Encoding ascii
$cores = [Environment]::ProcessorCount
$lastCpu = $null; $lastCpuTime = $null
$nextShot = (Get-Date).AddSeconds(5)
$nextSystem = Get-Date
$markCount = 0
while ($true) {
    if ($game.HasExited) { break }
    $game.Refresh()
    $p = $game
    $now = Get-Date
    # The game appends a line per R press (BF_MARK_FILE): a screenshot for each new one.
    if (Test-Path $marks) {
        $lines = @(Get-Content $marks -ErrorAction SilentlyContinue).Count
        # A clip for the newest mark; marks pressed during a clip are counted, and get no clip of their own.
        if ($markCount -lt $lines) {
            $markCount = $lines
            Clip $markCount
        }
    }
    if ($now -ge $nextShot) { Shot ""; $nextShot = $now.AddSeconds($Every) }
    if ($now -ge $nextSystem) {
        $nextSystem = $now.AddSeconds(5)
        $cpu = ""
        if ($lastCpu -ne $null) {
            $cpu = [math]::Round(100.0 * ($p.TotalProcessorTime - $lastCpu).TotalSeconds / ($now - $lastCpuTime).TotalSeconds / $cores, 1)
        }
        $lastCpu = $p.TotalProcessorTime; $lastCpuTime = $now
        $gpu = ",,,"
        try {
            $g = (& nvidia-smi --query-gpu=utilization.gpu,memory.used,temperature.gpu,clocks.gr --format=csv,noheader,nounits 2>$null | Select-Object -First 1)
            if ($g) { $gpu = ($g -split ",\s*") -join "," }
        } catch {}
        Add-Content -Path $system -Encoding ascii -Value ("{0},{1},{2},{3},{4}" -f (Stamp), $cpu,
            [math]::Round($p.WorkingSet64 / 1MB), [math]::Round($p.PrivateMemorySize64 / 1MB), $gpu)
    }
    Start-Sleep -Milliseconds 40
}
$code = ""
try { $code = $game.ExitCode } catch {}
Note "game ended (exit code '$code'); $markCount marks"
