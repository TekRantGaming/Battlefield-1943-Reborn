# Local dedicated servers, one per map, for TekRantGaming's play tests on this PC: our backend (server\plasma.py), and per
# map a dedicated server (bf1943.exe --server, its own loopback address so every server can use port 1003) with
# its GameManager host (server\gmhost.py, registered with the backend, listed in the game's server browser). Binds
# are loopback only (BF_LOOPBACK_ONLY), so Windows Firewall never asks. The servers run from runs\servers\bin, a
# copy of the build, so tools\run_game.ps1 can still refresh runs\bin for the client.
# Usage: powershell -File tools\run_local_servers.ps1 -Name maps1 [-Preset bf-relwithdebinfo] [-Maps wake,guadalcanal]
#        [-ServerArgs --bf_map_rotation=wake] (more flags for every dedicated server)
#        A map's Night Ops copy, <map>_night (wake_night: "Wake Island Night Ops (local)"), has its own addresses, so
#        it runs beside the same map's day server: -Maps wake,wake_night.
# Stop: the PIDs are in runs\servers\<name>\pids.txt.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Preset = "bf-relwithdebinfo",
    [string[]]$Maps = @("wake", "guadalcanal", "iwo", "coral"),
    [string[]]$ServerArgs = @(),
    [string[]]$NightMaps = @()  # these maps' servers are Night Ops servers (gmhost.py --night)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$python = if ($env:BF_PYTHON) { $env:BF_PYTHON } else { "python" }  # Python 3.12
$levels = @{
    wake        = @{ Level = "Levels/Wake_island_s"; Title = "Wake Island"; Last = 11 }
    guadalcanal = @{ Level = "Levels/Guadal_Canal"; Title = "Guadalcanal"; Last = 12 }
    iwo         = @{ Level = "Levels/Iwo_Jima_s"; Title = "Iwo Jima"; Last = 13 }
    coral       = @{ Level = "Levels/Coral_sea"; Title = "Coral Sea"; Last = 14 }
}
foreach ($k in @($levels.Keys)) {  # the Night Ops copies: servers on 127.0.0.15 to 18, hosts on 127.0.0.25 to 28
    $l = $levels[$k]
    $levels["${k}_night"] = @{ Level = $l.Level; Title = "$($l.Title) Night Ops"; Map = $l.Title; Last = $l.Last + 4; Night = $true }
}
$dir = Join-Path $root "runs\servers\$Name"
if (Test-Path $dir) { throw "run folder exists: $dir" }
New-Item -ItemType Directory -Force $dir | Out-Null
$bin = Join-Path $root "runs\servers\bin"
$build = Join-Path $root "bf\out\build\$Preset"
$running = Get-CimInstance Win32_Process -Filter "Name = 'bf1943.exe'" | Where-Object { $_.ExecutablePath -like "$bin\*" }
if ($running) { throw "servers already running from $bin (PIDs $($running.ProcessId -join ', '))" }
New-Item -ItemType Directory -Force $bin | Out-Null
Get-ChildItem $bin -File -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item (Join-Path $build "bf1943.exe") $bin
Get-ChildItem $build -Filter *.dll | Copy-Item -Destination $bin
$data = Join-Path $build "data"
if (Test-Path $data) { Remove-Item (Join-Path $bin "data") -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item $data $bin -Recurse }

$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"  # server windows never take the focus
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
$pids = @()
$backend = Start-Process -FilePath $python -ArgumentList "-I", (Join-Path $root "server\plasma.py"), "--log", (Join-Path $dir "plasma.log") `
    -WindowStyle Hidden -RedirectStandardOutput (Join-Path $dir "plasma.out") -PassThru
$pids += "backend $($backend.Id)"
Start-Sleep 1
foreach ($m in $Maps) {
    $map = $levels[$m]
    if (-not $map) { throw "unknown map '$m' (wake, guadalcanal, iwo, coral, or one of them with _night)" }
    $address = "127.0.0.$($map.Last)"
    $gateway = "127.0.0.$($map.Last + 10):1000"
    $sdir = Join-Path $dir $m
    New-Item -ItemType Directory -Force (Join-Path $sdir "user") | Out-Null
    $env:BF_LOOPBACK_ONLY = "1"
    $env:BF_LOOPBACK_ADDR = $address
    $args = @("--game_data_root=`"$(Join-Path $root 'input\pkg')`"", "--user_data_root=`"$(Join-Path $sdir 'user')`"",
              "--log_file=`"$(Join-Path $sdir 'bf1943.log')`"", "--fullscreen=false", "--server",
              "--cl=`"-dedicated -level $($map.Level)`"") + $ServerArgs
    $server = Start-Process -FilePath (Join-Path $bin "bf1943.exe") -ArgumentList $args -WorkingDirectory $bin -WindowStyle Minimized `
        -RedirectStandardError (Join-Path $sdir "stderr.txt") -RedirectStandardOutput (Join-Path $sdir "stdout.txt") -PassThru
    Remove-Item Env:BF_LOOPBACK_ONLY, Env:BF_LOOPBACK_ADDR
    $hostArgs = @("-I", (Join-Path $root "server\gmhost.py"), "--backend", "127.0.0.1:18250",
        "--listen", $gateway, "--game-server", "$($address):1003", "--name", "`"$($map.Title) (local)`"",
        "--map", "`"$(if ($map.Map) { $map.Map } else { $map.Title })`"", "--log", (Join-Path $sdir "gmhost.log"))
    if ($NightMaps -contains $m -or $map.Night) { $hostArgs += "--night" }  # Start-Process takes no empty argument
    $host_ = Start-Process -FilePath $python -ArgumentList $hostArgs -WindowStyle Hidden -RedirectStandardOutput (Join-Path $sdir "gmhost.out") -PassThru
    $pids += "$m server $($server.Id)"
    $pids += "$m host $($host_.Id)"
    "started $($map.Title): server PID $($server.Id) on $address, host PID $($host_.Id) on $gateway"
}
Set-Content (Join-Path $dir "pids.txt") $pids -Encoding ascii
"PIDs in $(Join-Path $dir 'pids.txt')"
