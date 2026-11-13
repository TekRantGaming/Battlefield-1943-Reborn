# One join test against a dedicated server: our backend (server\plasma.py), the GameManager host
# (server\gmhost.py, its own process as in the server package, registered with the backend) on 127.0.0.2:1000, the
# dedicated server (bf1943.exe --server --cl=-dedicated, UDP 1003) and one client that picks Quick Match with the
# developer pad. -InProcess runs the host inside the backend instead (plasma.py --dedicated). Logs:
# runs\plasma_<name>.log and runs\gmhost_<name>.log.
# Usage: powershell -File tools\run_dedicated_test.ps1 -Name ded2 [-Seconds 120] [-Shots "70,90,115"] [-DevPad ...]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$Seconds = 120,
    [string]$Shots = "70,90,115",
    [string]$DevPad = "20:start/0.5,24:start/0.5,28:start/0.5,36:a/0.5,48:a/0.5",
    [switch]$Dump,  # BF_NET_DUMP=1: whole datagrams in the client and server logs
    [switch]$InProcess,
    [string]$SeedCache = "",  # shader storage files for the client's cache (run_game.ps1 -SeedCache)
    [string[]]$GameArgs = @(),  # extra runtime flags for the client, e.g. --video_mode_refresh_rate=120
    [switch]$Launcher,  # show our menu on the main client
    [string]$RuntimeDll = "",  # run_game.ps1 -RuntimeDll
    [string]$PeerArgs = "",  # run_game.ps1 -PeerArgs: the second client's own flags
    [string]$ServerArgs = "",  # run_game.ps1 -ServerArgs: the dedicated server's own flags
    [string]$PeerPad = "",  # a second client with this pad script (three game processes; it uses 127.0.0.2)
    [int]$StopServerAt = 0,  # stop the dedicated server this many seconds after the client starts
    [string]$GpuPlugin = "",  # run_game.ps1 -GpuPlugin
    [string]$Preset = "bf-dev",  # the client's build (bf-relwithdebinfo: the players' one; no dev pad there)
    [switch]$NoNetTrace,  # no network trace in the client's log (as players run; for frame time tests)
    [string]$Level = "",  # the server's level, e.g. Levels/Guadal_Canal (both teams start on land); Wake by default
    [string]$UserFrom = "",  # run_game.ps1 -UserFrom: an earlier run's user folder (a finished launcher setup, say)
    [string[]]$BackendArgs = @(),  # extra backend flags, e.g. --official-key-file <file> (browser tests)
    [switch]$BuiltInHost,  # the dedicated server runs our own GameManager host (bf\src\gm_host.cpp), no gmhost.py
    [string]$Stacks = "",  # run_game.ps1 -Stacks: seconds at which the client logs every guest thread (bf-dev)
    [string[]]$HostArgs = @(),  # more options for gmhost.py: --region gb, --broadcast-file <file>...
    [switch]$Night,  # the server is a Night Ops server (gmhost.py --night): players' games draw its map at night
    [switch]$FollowRestart,  # run_game.ps1 -FollowRestart: go on with a game that started itself again
    [switch]$ClientOnMain,  # with -BackendHost: the client stays on the main backend (127.0.0.1), so the server is one
                            # on a backend of its own (the server package's LAN only mode: Direct connect finds it)
    [string]$BackendHost = "127.0.0.1"  # the backend's address: another loopback one (127.0.0.5) runs the test beside
                                        # the play test's servers, whose backend keeps 127.0.0.1 (run_local_servers.ps1)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$python = if ($env:BF_PYTHON) { $env:BF_PYTHON } else { "python" }  # Python 3.12
if ($Dump) { $env:BF_NET_DUMP = "1" }
$log = Join-Path $root "runs\plasma_$Name.log"
$gateway = $(if ($PeerPad) { "127.0.0.3:1000" } else { "127.0.0.2:1000" })
$plasmaArgs = @("-I", (Join-Path $root "server\plasma.py"), "--seconds", "$($Seconds + 60)", "--log", $log)
if ($BackendHost -ne "127.0.0.1") {
    $plasmaArgs += @("--host", $BackendHost, "--theater-host", $BackendHost)
    if (-not $ClientOnMain) { $GameArgs += "--bf_online_host=$BackendHost" }
    $PeerArgs = ("--bf_online_host=$BackendHost " + $PeerArgs).Trim()
}
if ($InProcess) { $plasmaArgs += @("--dedicated", "$gateway=127.0.0.1:1003") }
$plasmaArgs += $BackendArgs
$p = Start-Process -FilePath $python -ArgumentList $plasmaArgs -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $root "runs\plasma_$Name.out") -PassThru
$gm = $null
if ($BuiltInHost) {
    # The server registers itself (bf_gm_backend) once its level runs; listed, so the client's Quick Match finds it.
    $ServerArgs = ("--bf_gm_backend=$($BackendHost):18250 --bf_gm_listen=$gateway --bf_gm_name=`"Local test server`" " +
                   "--bf_gm_listed=true " + $ServerArgs).Trim()
} elseif (-not $InProcess) {
    $gmLog = Join-Path $root "runs\gmhost_$Name.log"
    $gmArgs = @("-I", (Join-Path $root "server\gmhost.py"), "--backend", "$($BackendHost):18250", "--listen", $gateway,
                "--game-server", "127.0.0.1:1003", "--name", "`"Local test server`"", "--log", $gmLog)
    if ($Night) { $gmArgs += "--night" }  # Start-Process takes no empty argument
    $gmArgs += $HostArgs
    $gm = Start-Process -FilePath $python -ArgumentList $gmArgs -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $root "runs\gmhost_$Name.out") -PassThru
    $log = $gmLog
}
Start-Sleep 1
try {
    & (Join-Path $PSScriptRoot "run_game.ps1") -Name $Name -Seconds $Seconds -Preset $Preset -NetTrace:(-not $NoNetTrace) -WithServer `
        -ServerCl $(if ($Level) { "-dedicated -level $Level" } else { "-dedicated" }) -ServerWarmup 25 -DevPad $DevPad -Shots $Shots -SeedCache $SeedCache -PeerPad $PeerPad -PeerArgs $PeerArgs -ServerArgs $ServerArgs -RuntimeDll $RuntimeDll -GameArgs $GameArgs -Launcher:$Launcher -StopServerAt $StopServerAt -GpuPlugin $GpuPlugin -UserFrom $UserFrom -Stacks $Stacks -FollowRestart:$FollowRestart | Select-Object -Last 3
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    if ($gm) { Stop-Process -Id $gm.Id -Force -ErrorAction SilentlyContinue }
    if ($Dump) { Remove-Item Env:BF_NET_DUMP }
}
if ($BuiltInHost) {
    Get-Content (Join-Path $root "runs\$Name\server\bf1943.log") | Select-String "BF GM host" | Select-Object -Last 30 |
        ForEach-Object { $_.Line }
} else {
    Get-Content $log | Select-String "GMHOST" | Select-Object -Last 30 | ForEach-Object { $_.Line }
}
