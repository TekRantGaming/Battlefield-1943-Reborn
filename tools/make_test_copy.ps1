# A test copy for TekRantGaming to start with a double click (overnight brief, PROMPT-VULKAN-RENDERER.md): the normal build
# (bf-relwithdebinfo) in runs\testcopy\bin, with bf1943.toml choosing the native renderer (rexgpu-bfnative, Vulkan
# only); the Xenos renderer is one setting away in that file. "Play Battlefield 1943.bat" starts our backend and the
# local map servers (tools\run_local_servers.ps1) when they are not running yet, then the game, with its own user
# folder (runs\testcopy\user). Nothing in runs\ goes into git.
# Usage: powershell -File tools\make_test_copy.ps1 [-Preset bf-relwithdebinfo]
param([string]$Preset = "bf-relwithdebinfo")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "bf\out\build\$Preset"
$dir = Join-Path $root "runs\testcopy"
$bin = Join-Path $dir "bin"
New-Item -ItemType Directory -Force $bin, (Join-Path $dir "user") | Out-Null
Get-ChildItem $bin -File -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item (Join-Path $build "bf1943.exe") $bin
Get-ChildItem $build -Filter *.dll | Where-Object { $_.Name -ne "rexgpu-xenos.dll" } | Copy-Item -Destination $bin
Remove-Item (Join-Path $bin "data") -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $build "data") $bin -Recurse

Set-Content (Join-Path $bin "bf1943.toml") -Encoding ascii -Value @'
# Battlefield 1943 test copy settings (runtime settings; our menu keeps its own in user\bf1943-settings.txt).
# The renderer is Vulkan, ours (rexgpu-bfnative); there is no other (DECISIONS.md, 5 Nov 2026 evening). start.ps1
# turns NativeXRenderer on (--native_x_renderer, --bf_engine_d3d_path).
'@

Set-Content (Join-Path $dir "start.ps1") -Encoding ascii -Value @"
# Starts the local backend and map servers when they are not running, then the game (make_test_copy.ps1).
# -Watched: a watched play test. Its folder is runs\playtest-<date>-<time>: the game's log there, with its hitch lines
# (game frames over 25 ms, NativeXRenderer's frames over 25 ms with what it did in them) and FPS every 5 s; and
# tools\playtest_watch.ps1 beside the game (screenshots, R marks, system use).
param([switch]`$Watched)
`$root = '$root'
`$dir = '$dir'
`$servers = Get-ChildItem (Join-Path `$root 'runs\servers') -Directory -Filter 'testcopy-*' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not (Get-NetTCPConnection -LocalPort 18250 -State Listen -ErrorAction SilentlyContinue)) {
    `$name = 'testcopy-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
    `$servers = Join-Path `$root ('runs\servers\' + `$name)
    # Waits for the script only (the servers it starts keep running; a pipe would wait for them too).
    `$p = Start-Process powershell -ArgumentList '-ExecutionPolicy', 'Bypass', '-File',
        ('"' + (Join-Path `$root 'tools\run_local_servers.ps1') + '"'), '-Name', `$name -WindowStyle Hidden -PassThru
    `$p.WaitForExit()
}
`$env:BF_LOOPBACK_ONLY = '1'  # the local servers bind 127.0.0.1 only (no firewall prompt)
# SDL gives a controller's input only to the focused window unless told otherwise; with a server window beside the
# game, the pad went nowhere (TekRantGaming, 5 Nov).
`$env:SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS = '1'
`$bin = Join-Path `$dir 'bin'
`$log = Join-Path `$dir 'bf1943.log'
if (`$Watched) {
    `$session = Join-Path `$root ('runs\playtest-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force `$session | Out-Null
    `$log = Join-Path `$session 'bf1943.log'
    `$env:BF_HITCH_MS = '25'
    `$env:BF_MARK_FILE = (Join-Path `$session 'marks.txt')  # R in the game (bf\src\playtest_mark.cpp)
    `$env:BF_FRAME_STATS = '1'
}
`$gameArgs = @('--game_data_root="' + (Join-Path `$root 'input\pkg') + '"', '--user_data_root="' + (Join-Path `$dir 'user') + '"',
          '--log_file="' + `$log + '"',
          # NativeXRenderer draws the game (both off by default until it is the players' default).
          '--native_x_renderer=true', '--bf_engine_d3d_path=true')
if (`$Watched) { `$gameArgs += '--native_x_renderer_hitch_ms=25' }
`$game = Start-Process -FilePath (Join-Path `$bin 'bf1943.exe') -ArgumentList `$gameArgs -WorkingDirectory `$bin -PassThru
if (`$Watched) {
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-ExecutionPolicy', 'Bypass', '-File',
        ('"' + (Join-Path `$root 'tools\playtest_watch.ps1') + '"'), '-ProcessId', `$game.Id, '-Dir', ('"' + `$session + '"'),
        '-Servers', ('"' + `$servers + '"')
}
"@

Set-Content (Join-Path $dir "Play Battlefield 1943.bat") -Encoding ascii -Value @'
@echo off
rem Battlefield 1943 test copy: starts the local servers if needed, then the game (native renderer, bin\bf1943.toml).
powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0start.ps1"
'@
Set-Content (Join-Path $dir "Play Battlefield 1943 (watched).bat") -Encoding ascii -Value @'
@echo off
rem A watched play test: the same game, with hitch logging, screenshots every 10 s and the R mark key
rem (runs\playtest-<date>-<time>, tools\playtest_watch.ps1).
powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0start.ps1" -Watched
'@
# Vulkan only: no Direct3D 12 starter any more (an older copy's is removed).
Remove-Item (Join-Path $dir "Play Battlefield 1943 (Xenos D3D12).bat") -ErrorAction SilentlyContinue
"test copy in $dir"
