# One test run of the recompiled game, following the playbook's game rules (starter\agent-kit\GAME-RULES.md):
#   - nothing starts while TekRantGaming holds the slot (King Kong's runs\game.lock says "user") or while another
#     bf1943.exe runs anywhere; at most one game of ours at a time (runs\game.lock here);
#   - the exe and DLLs are copied into one fixed folder, runs\bin (one program path, so Windows Firewall asks at
#     most once), with hashes in runs\<name>; user data is a fresh runs\<name>\user;
#   - windowed, never activated, kept at the back by build\bfbg\bfbg.exe;
#   - the run is stopped by its own PID after -Seconds, then every bf1943.exe under runs\ is listed by path.
# -Interactive (TekRantGaming's exception, DECISIONS.md 4 Nov 2026): a run for TekRantGaming to play. The window opens in the
# foreground with focus, the background helper is stopped, nothing captures or stops it; TekRantGaming closes it. The lock
# says "tekrantgaming <name> <pid>" and counts as free again once that process has ended.
# Usage: powershell -File tools\run_game.ps1 -Name boot1 [-Seconds 90] [-Preset bf-relwithdebinfo] [-GameArgs ...]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$Seconds = 90,
    [string]$Preset = "bf-relwithdebinfo",
    [string[]]$GameArgs = @(),
    [string]$Shots = "",  # comma separated seconds after start at which to save a PNG of the game window (tools\capture_window.ps1)
    [switch]$Interactive,
    [string]$Stacks = "",  # BF_DEV_STACKS: comma separated seconds at which to log every guest stack (bf-dev)
    [switch]$NetTrace,  # BF_NET_TRACE=1: log network and online imports (bf-dev, bf\src\net_trace.cpp)
    [string]$DevPad = "",  # BF_DEV_PAD script for the bf-dev build, e.g. "10:start,14:a" (bf\src\dev_pad.cpp)
    [switch]$WithServer,
    [string]$RuntimeDll = "",  # a rexruntime.dll to use instead of the build's (our SDK copy, for A/B tests)
    [string]$PeerArgs = "",  # extra runtime flags for the second client (-PeerPad), e.g. its frame rate
    [string]$ServerArgs = "",  # extra runtime flags for the dedicated server (-WithServer), e.g. its map rotation
    [string]$PeerPad = "",  # join tests: a second client with this BF_DEV_PAD script starts first (log in runs\<name>\peer)
    [int]$ServerWarmup = 25,
    [string]$ServerCl = "-dedicated -platform xenon",  # the server's guest command line (--cl)
    [switch]$Lldb,  # run under VS LLVM lldb in batch mode and save a backtrace of every thread on a stop
    [switch]$Internet,  # bind every address (BF_LOOPBACK_ONLY off): joins to a server on the internet
    [switch]$Launcher,  # show our menu (bf/src/launcher.cpp) even in a test run
    [switch]$LoopbackOnly,  # bind 127.0.0.1 in an interactive run too (local server tests)
    [string]$SeedNxr = "",  # a folder of NativeXRenderer storage files (.pipelines, .vkcache) copied into the client's cache first
    [string]$SeedCache = "",  # a folder of shader storage files (.xsh, .xpso) copied into the client's shader cache first
    [string]$UserFrom = "",  # a user folder copied in first (TekRantGaming's profile from an earlier interactive run)
    [int]$StopServerAt = 0,  # seconds after start at which the helper processes are stopped (a server going away during a match)
    [string]$GpuPlugin = ""  # a rexgpu-xenos.dll to use instead of the build's (our SDK copy, sdk\src-bf, for A/B tests)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$runs = Join-Path $root "runs"
$lock = Join-Path $runs "game.lock"
$build = Join-Path $root "bf\out\build\$Preset"
New-Item -ItemType Directory -Force $runs | Out-Null

$hold = "F:\KK-native-renderer\runs\game.lock"
if ((Test-Path $hold) -and ((Get-Content $hold -Raw).Trim() -eq "user")) { throw "TekRantGaming holds the game slot (user): not launching" }
# Local servers started by tools\run_local_servers.ps1 (runs\servers\bin) do not count. A game the last test stopped
# can take a few seconds to close (a RenderDoc run's most of all), so one still there gets up to 30 s first.
function Get-OtherGames {
    Get-CimInstance Win32_Process -Filter "Name = 'bf1943.exe'" |
        Where-Object { $_.ExecutablePath -notlike (Join-Path $runs "servers\bin\*") -and $_.CommandLine -notlike "*--server*" }
}
$others = Get-OtherGames
for ($i = 0; $others -and $i -lt 30; $i++) { Start-Sleep 1; $others = Get-OtherGames }
if ($others) { throw "bf1943.exe already running: $($others.ExecutablePath -join ', ') (PIDs $($others.ProcessId -join ', '))" }
if (Test-Path $lock) {
    $held = (Get-Content $lock -Raw).Trim()
    if ($held -match '^tekrantgaming \S+ (\d+)' -and -not (Get-Process -Id $Matches[1] -ErrorAction SilentlyContinue)) { $held = "free" }
    if ($held -ne "free") { throw "runs\game.lock is taken: $held" }
}
Set-Content -Path $lock -Value "$Name $(Get-Date -Format s)" -Encoding ascii

$proc = $null; $helperProcs = @()
try {
    $bg = Join-Path $root "build\bfbg\bfbg.exe"
    $bgRunning = Get-CimInstance Win32_Process -Filter "Name = 'bfbg.exe'" | Where-Object { $_.ExecutablePath -eq $bg }
    if ($Interactive) { foreach ($p in $bgRunning) { Stop-Process -Id $p.ProcessId -Force } }
    # Only the test runs' and the local servers' copies: runs\testcopy is TekRantGaming's, played in front (5 Nov: the
    # watcher kept pushing it to the back).
    elseif (-not $bgRunning) {
        Start-Process -FilePath $bg -ArgumentList "`"$(Join-Path $runs 'bin')`"", "`"$(Join-Path $runs 'servers')`"" -WindowStyle Hidden | Out-Null
        Start-Sleep 1
    }

    $dir = Join-Path $runs $Name
    if (Test-Path $dir) { throw "run folder exists: $dir" }
    $bin = Join-Path $runs "bin"; $user = Join-Path $dir "user"
    New-Item -ItemType Directory -Force $dir, $bin, $user | Out-Null
    Get-ChildItem $bin -File -ErrorAction SilentlyContinue | Remove-Item -Force
    Copy-Item (Join-Path $build "bf1943.exe") $bin
    $pdb = Join-Path $build "bf1943.pdb"; if (($Lldb -or $env:BF_DEV_PROFILE -or $env:BF_DEV_WATCH -or $env:BF_DEV_WATCH_POSE -or $env:BF_DEV_PDB) -and (Test-Path $pdb)) { Copy-Item $pdb $bin }
    Get-ChildItem $build -Filter *.dll | Copy-Item -Destination $bin
    $map = Join-Path $build "bf1943.map"; if (Test-Path $map) { Copy-Item $map $bin }
    if ($RuntimeDll) { Copy-Item $RuntimeDll (Join-Path $bin "rexruntime.dll") -Force; "runtime from $RuntimeDll" }
    if ($GpuPlugin) {
        Copy-Item $GpuPlugin (Join-Path $bin "rexgpu-xenos.dll") -Force; "GPU plugin from $GpuPlugin"
        # Our native renderer (rexgpu-bfnative, --gpu_plugin=bfnative) is installed beside it.
        $native = Join-Path (Split-Path $GpuPlugin) "rexgpu-bfnative.dll"; if (Test-Path $native) { Copy-Item $native $bin -Force }; $nativePdb = Join-Path (Split-Path $GpuPlugin) "rexgpu-bfnative.pdb"; if (Test-Path $nativePdb) { Copy-Item $nativePdb $bin -Force }
    }
    $fonts = Join-Path $build "fonts"; if (Test-Path $fonts) { Remove-Item (Join-Path $bin "fonts") -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item $fonts $bin -Recurse }  # our menus' font
    $data = Join-Path $build "data"; if (Test-Path $data) { Remove-Item (Join-Path $bin "data") -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item $data $bin -Recurse }
    $glyphs = Join-Path $build "glyphs"; if (Test-Path $glyphs) { Remove-Item (Join-Path $bin "glyphs") -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item $glyphs $bin -Recurse }  # the launcher's button glyphs
    if ($UserFrom) { Copy-Item (Join-Path $UserFrom "*") $user -Recurse -Force }  # settings, name, caches
    if ($SeedNxr) {
        $nxr = Join-Path $user "cache\native_x_renderer"; New-Item -ItemType Directory -Force $nxr | Out-Null
        Get-ChildItem $SeedNxr -File | Where-Object { $_.Extension -in ".pipelines", ".vkcache" } | Copy-Item -Destination $nxr
    }
    if ($SeedCache) {
        $shareable = Join-Path $user "cache\shaders\shareable"; New-Item -ItemType Directory -Force $shareable | Out-Null
        Get-ChildItem $SeedCache -File | Where-Object { $_.Extension -in ".xsh", ".xpso" } | Copy-Item -Destination $shareable
    }
    Get-ChildItem $bin | Get-FileHash -Algorithm SHA256 | ForEach-Object { "$($_.Hash)  $(Split-Path -Leaf $_.Path)" } |
        Set-Content (Join-Path $dir "hashes.txt") -Encoding ascii

    if (-not $Interactive) {
        $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
        $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    }
    $env:BF_DEV_PAD = $DevPad
    $env:BF_DEV_STACKS = $Stacks
    $env:BF_LOOPBACK_ONLY = $(if (($Interactive -and -not $LoopbackOnly) -or $Internet) { "" } else { "1" })  # test runs bind 127.0.0.1 (no firewall prompt)
    $env:BF_NET_TRACE = $(if ($NetTrace) { "1" } else { "" })
    # BF_TEST_GAME_ROOT: another game folder for the client (an empty one: the launcher's first page, game files).
    $gameRoot = $(if ($env:BF_TEST_GAME_ROOT) { $env:BF_TEST_GAME_ROOT } else { Join-Path $root 'input\pkg' })
    $launchArgs = @("--game_data_root=`"$gameRoot`"", "--user_data_root=`"$user`"",
                  "--log_file=`"$(Join-Path $dir 'bf1943.log')`"") + $GameArgs
    # The SDK starts fullscreen by default; a fullscreen test window can change the display mode over whatever TekRantGaming
    # is watching, even at the back (bfbg). Test runs stay in a window.
    # Test windows open behind everything without focus (our SDK copy's window_start), never over TekRantGaming's screen.
    if (-not $Interactive) { $launchArgs += "--fullscreen=false"; $launchArgs += "--window_start=background" }
    if (-not $Interactive -and -not $Launcher) { $launchArgs += "--bf_launcher=false" }  # test runs start the game at once (bf\src\launcher.cpp)
    # Test runs never send their pipelines to the live server (bf\src\pipeline_upload.cpp; the Kong coordinator: no test
    # uploads there). BF_TEST_PIPELINES=1 keeps the setting for a test against a local Worker (BF_DEV_PIPELINES_URL).
    # A saved "Share graphics data" in the profile wins over --bf_share_pipelines (run tekrantgaming_8 uploaded so), and an
    # empty --bf_pipelines_url stops the whole command line (CLI11: "1 required TEXT missing"), so the address is a
    # closed local port: any upload fails on this PC.
    if ($env:BF_TEST_PIPELINES -ne "1") { $launchArgs += @("--bf_share_pipelines=false", "--bf_pipelines_url=http://127.0.0.1:9/no-upload") }
# Test runs never ask GitHub for updates (its 60 an hour per address are shared with TekRantGaming): a closed local feed, or
# BF_TEST_UPDATE_FEED (a local test feed, scratchpad update_feed).
if (-not $Interactive) { $launchArgs += "--bf_update_feed=$(if ($env:BF_TEST_UPDATE_FEED) { $env:BF_TEST_UPDATE_FEED } else { 'http://127.0.0.1:9/no-feed' })" }
    # Keyboard & mouse is on by default for players (it locks the pointer to the game window while it has the focus);
    # test runs leave it off, so a test window that gets the focus never takes the mouse. BF_TEST_MNK=1 keeps it.
    if (-not $Interactive -and $env:BF_TEST_MNK -ne "1") { $launchArgs += "--mnk_mode=false" }
    Set-Content (Join-Path $dir "args.txt") ($launchArgs -join " ") -Encoding ascii
    $exe = Join-Path $bin "bf1943.exe"; $procArgs = $launchArgs
    # Helper processes before the main client: the dedicated server (-WithServer) and a second client (-PeerPad).
    # Both together make three game processes, allowed for multiplayer tests (DECISIONS.md 4 Nov 2026).
    $helpers = @()
    if ($WithServer) { $helpers += "server" }
    if ($PeerPad) { $helpers += "peer" }
    foreach ($kind in $helpers) {
        $sdir = Join-Path $dir $kind; New-Item -ItemType Directory -Force (Join-Path $sdir "user") | Out-Null
        $helperArgs = @("--game_data_root=`"$(Join-Path $root 'input\pkg')`"", "--user_data_root=`"$(Join-Path $sdir 'user')`"",
                        "--log_file=`"$(Join-Path $sdir 'bf1943.log')`"", "--fullscreen=false", "--window_start=background")
        if ($kind -eq "server") { $helperArgs += @("--server", "--cl=`"$ServerCl`""); if ($ServerArgs) { $helperArgs += $ServerArgs } } else { if ($PeerPad -ne "driver") { $helperArgs += "--bf_launcher=false" }; if ($PeerArgs) { $helperArgs += $PeerArgs } }
        Set-Content (Join-Path $sdir "args.txt") ($helperArgs -join " ") -Encoding ascii
        # -PeerPad driver: the peer gets no pad script, so our menu driver signs it in and joins (BF_AUTO_JOIN=1).
        $devPadSaved = $env:BF_DEV_PAD; $env:BF_DEV_PAD = $(if ($kind -eq "peer" -and $PeerPad -ne "driver") { $PeerPad } else { "" })
        if ($kind -eq "peer") { $env:BF_LOOPBACK_ADDR = "127.0.0.2" }  # its own address and XUID (bf\src\live_layers.cpp)
        # BF_PEER_CLEAR (tests): environment variables the second client does not get, names separated by commas (the
        # host's own pause test in a private match test, say).
        $cleared = @{}
        if ($kind -eq "peer" -and $env:BF_PEER_CLEAR) { foreach ($n in $env:BF_PEER_CLEAR.Split(",")) { $cleared[$n] = [Environment]::GetEnvironmentVariable($n); [Environment]::SetEnvironmentVariable($n, $null) } }
        $hp = Start-Process -FilePath $exe -ArgumentList $helperArgs -WorkingDirectory $bin -WindowStyle Minimized `
            -RedirectStandardError (Join-Path $sdir "stderr.txt") -RedirectStandardOutput (Join-Path $sdir "stdout.txt") -PassThru
        $env:BF_DEV_PAD = $devPadSaved; $env:BF_LOOPBACK_ADDR = ""
        foreach ($n in $cleared.Keys) { [Environment]::SetEnvironmentVariable($n, $cleared[$n]) }
        $helperProcs += $hp
        "started $kind PID $($hp.Id), waiting $ServerWarmup s"
        if ($hp.WaitForExit($ServerWarmup * 1000)) { throw "$kind exited by itself, code $($hp.ExitCode)" }
    }
    if ($Lldb) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
        $procArgs = @("-b", "-o", "`"process handle SIGSEGV -s false -p true`"", "-o", "run", "-o", "`"thread backtrace all -c 40`"",
                      "-o", "kill", "--", "`"$exe`"") + $launchArgs
        $exe = Join-Path $vs "VC\Tools\Llvm\x64\bin\lldb.exe"
    }
    # BF_RENDERDOC=<renderdoccmd.exe>: the client starts under RenderDoc (it injects its capture layer); the game
    # triggers the capture itself at BF_RENDERDOC_AT seconds (bf\src\dev_renderdoc.cpp). The capture goes to
    # runs\<name>\rdoc*.rdc. The process watched is renderdoccmd, which waits for the game (-w).
    $launchExe = $exe
    if ($env:BF_RENDERDOC) {
        $procArgs = @("capture", "-w", "-d", "`"$bin`"", "-c", "`"$(Join-Path $dir 'rdoc')`"", "`"$exe`"") + $procArgs
        $launchExe = $env:BF_RENDERDOC
    }
    $proc = Start-Process -FilePath $launchExe -ArgumentList $procArgs -WorkingDirectory $bin `
        -WindowStyle $(if ($Interactive) { "Normal" } else { "Minimized" }) -RedirectStandardError (Join-Path $dir "stderr.txt") `
        -RedirectStandardOutput (Join-Path $dir "stdout.txt") -PassThru
    $null = $proc.Handle  # keeps the exit code readable after exit
    "started PID $($proc.Id)"
    if ($Interactive) {
        Set-Content -Path $lock -Value "tekrantgaming $Name $($proc.Id) $(Get-Date -Format s)" -Encoding ascii
        "interactive run for TekRantGaming: left running, he closes it"
        return
    }
    $waited = 0; $exited = $false
    $times = @($Shots -split "," | Where-Object { $_ } | ForEach-Object { [int]$_ })
    if ($StopServerAt -gt 0) { $times += $StopServerAt }
    foreach ($t in ($times | Sort-Object -Unique)) {
        if ($t -ge $Seconds) { break }
        if ($proc.WaitForExit(($t - $waited) * 1000)) { $exited = $true; break }
        $waited = $t
        if ($t -eq $StopServerAt) {
            foreach ($hp in $helperProcs) { if (-not $hp.HasExited) { Stop-Process -Id $hp.Id -Force; "stopped helper PID $($hp.Id) at $t s" } }
            if (-not ($Shots -split "," | Where-Object { [int]$_ -eq $t })) { continue }
        }
        $proc.Refresh()
        try { & (Join-Path $PSScriptRoot "capture_window.ps1") -ProcessId $proc.Id -Out (Join-Path $dir "shot_$t.png"); "shot at $t s" }
        catch { "shot at $t s failed: $_" }
    }
    if (-not $exited) { $exited = $proc.WaitForExit(($Seconds - $waited) * 1000) }
    if ($exited) { "exited by itself, code $($proc.ExitCode) (0x{0:X8})" -f $proc.ExitCode }
    else {
        "time up after $Seconds s, closing"
        $null = $proc.CloseMainWindow()
        if (-not $proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force; "stopped PID $($proc.Id)" }
    }
    foreach ($hp in $helperProcs) { if (-not $hp.HasExited) { Stop-Process -Id $hp.Id -Force; "stopped helper PID $($hp.Id)" } }
} finally {
  if (-not $Interactive -or -not $proc) {
    try { $bl = Join-Path $runs "bin\break.log"; if (Test-Path $bl) { Move-Item $bl (Join-Path $runs "$Name\break.log") -Force } } catch { "could not move break.log: $_" }
    $left = Get-CimInstance Win32_Process -Filter "Name = 'bf1943.exe'" |
        Where-Object { $_.ExecutablePath -like "$runs\*" -and $_.ExecutablePath -notlike "$runs\servers\bin\*" -and $_.CommandLine -notlike "*--server*" }
    foreach ($p in $left) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue; "stopped leftover PID $($p.ProcessId)" }
    # The slot is free once they are gone (the next run checks for them).
    if ($left) { Wait-Process -Id @($left.ProcessId) -Timeout 30 -ErrorAction SilentlyContinue }
    Set-Content -Path $lock -Value "free" -Encoding ascii
  }
}

