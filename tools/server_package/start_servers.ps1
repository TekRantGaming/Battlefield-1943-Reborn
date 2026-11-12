# Battlefield 1943 Reborn server: starts the servers server.json lists (server_config.ps1 has its fields), headless
# (no window, no graphics), and returns. Every path quoted: a folder name may have spaces.
#   start_servers.ps1          every server (and, in the official package, the main backend)
#   start_servers.ps1 -Index 2 only the second server (started again if it runs)
#   start_servers.ps1 -BackendOnly  the official package's main backend when it is not running, nothing else
# Each server: its own dedicated game process on a loopback address of its own (127.0.0.11, .12 and on: no port to
# open), and its host (server\gmhost.py) that players reach on UDP 18300, 18301 and on. Logs, the state the window
# shows, and the game's user folder in logs\server<n>. The watchdog (watch_servers.ps1) is started too when it is not
# running: a server that crashes is started again.
param([switch]$Quiet, [int]$Index = 0, [switch]$BackendOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'server_config.ps1')
$root = $ServerRoot
$config = Read-ServerConfig
$official = Test-OfficialPackage
$game = Join-Path $root 'game'
if (-not (Test-Path (Join-Path $game 'default.xex'))) { throw 'The game is not installed: open BF1943 Server.bat and install it first.' }
$bin = Join-Path $root 'bin'
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$servers = @($config.servers)
if ($servers.Count -gt $MaxServers) { throw "At most $MaxServers servers." }
if ($Index -gt $servers.Count) { throw "There is no server $Index." }

if ($BackendOnly) {
    # The watchdog's call: only the official main backend, when it is gone; the servers are left as they are.
} elseif ($Index -eq 0) {
    # Anything already running from this folder is stopped first. If it can't be (started by another user, or as
    # administrator: Stop-Process is refused), starting a second set would leave two servers fighting over the ports
    # (TekRantGaming, 6 Nov: the second server bound the next port, and the first set kept running).
    & (Join-Path $root 'stop_servers.ps1')
    $left = Get-ServerProcesses 0
    if ($left) { throw "Servers from this folder are still running and could not be stopped (PIDs $($left.ProcessId -join ', ')): they were started by another user or as administrator. Run 'Stop servers.bat' as administrator, then start again." }
    # A server set from another folder (an older package unzipped elsewhere, or started at boot from there) keeps its
    # own maps and ports, and players may land on it (TekRantGaming, 7 Nov: Wake Island picked, the maps still cycled).
    $others = @(Get-CimInstance Win32_Process -Filter "Name = 'bf1943.exe' OR Name = 'python.exe'" |
                Where-Object { $_.CommandLine -and -not $_.CommandLine.Contains($root) -and
                               ($_.CommandLine -match '--server|gmhost\.py|plasma\.py') })
    if ($others) {
        $where = ($others | ForEach-Object {
            if ($_.CommandLine -match '([A-Za-z]:\\[^"]*?)\\(bin\\bf1943\.exe|server\\(gmhost|plasma)\.py)') { $Matches[1] } else { "PID $($_.ProcessId)" }
        } | Select-Object -Unique) -join ', '
        throw "Another Battlefield 1943 Reborn server is running from a different folder ($where). Stop it there first (Stop servers.bat in that folder), then start again."
    }
} else {
    & (Join-Path $root 'stop_servers.ps1') -Index $Index
}

# The interpreter itself, not the py launcher (its child would outlive stop_servers.ps1).
# Python 3.14's python.exe can be a launcher too (the install manager): ask whichever there is for the real one. The
# first that answers wins. Windows' own python.exe (its Microsoft Store shortcut, WindowsApps) answers nothing when no
# Python is installed: that stopped Start with "You cannot call a method on a null-valued expression" (a community
# host, 12 Nov 2026).
$python = $null
foreach ($command in 'py', 'python', 'python3') {
    $launcher = (Get-Command $command -ErrorAction SilentlyContinue).Source
    if (-not $launcher) { continue }
    $answer = $null
    try { $answer = & $launcher -c 'import sys; print(sys.executable)' | Select-Object -Last 1 } catch { $answer = $null }
    if ($answer -and (Test-Path ([string]$answer).Trim())) { $python = ([string]$answer).Trim(); break }
}
if (-not $python) {
    throw ('Python is not installed (Windows may only have its Microsoft Store shortcut): install Python 3.12 or newer ' +
           'from python.org, with "Add python.exe to PATH" ticked, then start again.')
}

$ports = @()
$keyFile = Join-Path $root 'official.key'
if ($official) {
    if (-not (Test-Path $keyFile)) { throw 'This is the official server package, and official.key is not next to it. Copy it in from your own copy (it is never in a download).' }
    $public = Get-PublicAddress $config
    $backend = '127.0.0.1'
    $backendUp = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($root) -and $_.CommandLine -match 'plasma\.py' })
    if (($Index -eq 0 -and -not $BackendOnly) -or -not $backendUp) {
        # The main backend: login (TCP 18241), Theater (18246), the server registry (18250), open to the internet.
        $b = Start-Process -FilePath $python -ArgumentList @('-I', ('"' + (Join-Path $root 'server\plasma.py') + '"'), '--host', '0.0.0.0',
            '--theater-host', $public, '--official-key-file', ('"' + $keyFile + '"'),
            '--log', ('"' + (Join-Path $logs 'backend.log') + '"')) -WindowStyle Hidden -PassThru
        Set-Content (Join-Path $logs 'backend.pid') $b.Id -Encoding ascii
        Start-Sleep 1
    }
    $ports += @(@{ Port = 18241; Protocol = 'TCP' }, @{ Port = 18246; Protocol = 'TCP' }, @{ Port = 18250; Protocol = 'TCP' })
} else {
    $backend = if ($config.backend) { $config.backend } else { '127.0.0.1' }
}
if ($BackendOnly) { return }

$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
$started = @()
for ($n = 1; $n -le $servers.Count; $n++) {
    $s = $servers[$n - 1]
    $port = $BasePort + $n - 1
    $ports += @{ Port = $port; Protocol = 'UDP' }
    if ($Index -ne 0 -and $n -ne $Index) { continue }
    $chosen = @($s.maps | Where-Object { $MapLevels[$_] })
    if (-not $chosen) { throw "Server $n ($($s.name)) has no map." }
    $address = "127.0.0.$(10 + $n)"
    $dir = Get-ServerDir $n
    New-Item -ItemType Directory -Force (Join-Path $dir 'user') | Out-Null
    Remove-Item (Join-Path $dir 'user\current_map.txt'), (Join-Path $dir 'status.json') -ErrorAction SilentlyContinue
    # The dedicated server plays the maps in turn (server_rotation.cpp: the next map when a round ends, the players kept).
    $env:BF_LOOPBACK_ONLY = '1'
    $env:BF_LOOPBACK_ADDR = $address
    $mode = if ($s.movement -eq 'modern') { 'modern' } else { 'classic' }
    $delay = if ($s.round_end_delay) { [int]$s.round_end_delay } else { 45 }
    $a = @('--game_data_root="' + $game + '"', '--user_data_root="' + (Join-Path $dir 'user') + '"',
           '--log_file="' + (Join-Path $dir 'server.log') + '"', '--fullscreen=false', '--window_start=hidden', '--server',
           ('--cl="-dedicated -level ' + $MapLevels[$chosen[0]] + '"'), ('--bf_map_rotation=' + ($chosen -join ',')),
           "--bf_round_end_delay=$delay", "--bf_modern_mode=$($mode -eq 'modern')".ToLower())
    $server = Start-Process -FilePath (Join-Path $bin 'bf1943.exe') -ArgumentList $a -WorkingDirectory $bin -WindowStyle Hidden -PassThru
    Remove-Item Env:BF_LOOPBACK_ONLY, Env:BF_LOOPBACK_ADDR
    $title = if ($s.name) { $s.name } else { "Battlefield 1943 Reborn $n" }
    $titles = ($chosen | ForEach-Object { $MapTitles[$_] }) -join ','
    $ha = @('-I', ('"' + (Join-Path $root 'server\gmhost.py') + '"'), '--backend', "$($backend):18250",
            '--listen', "0.0.0.0:$port", '--game-server', "$($address):1003", '--name', ('"' + $title.Replace('"', "'") + '"'),
            '--map', ('"' + $MapTitles[$chosen[0]] + '"'), '--maps', ('"' + $titles + '"'),
            '--map-file', ('"' + (Join-Path $dir 'user\current_map.txt') + '"'), '--mode', $mode,
            '--status-file', ('"' + (Join-Path $dir 'status.json') + '"'), '--log', ('"' + (Join-Path $dir 'host.log') + '"'))
    if ($s.description) { $ha += @('--description', ('"' + ([string]$s.description).Replace('"', "'") + '"')) }
    if (-not $s.listed) { $ha += '--unlisted' }
    if ($s.night -and (Test-NightOpsPackage)) { $ha += '--night' }  # Night Ops: players' games draw its maps at night
    if ($official) { $ha += @('--official-key-file', ('"' + $keyFile + '"')) }
    $h = Start-Process -FilePath $python -ArgumentList $ha -WindowStyle Hidden -PassThru
    Set-Content (Join-Path $dir 'pids.txt') ("$($server.Id)`r`n$($h.Id)") -Encoding ascii
    $started += "$n $title ($titles, UDP $port)"
}

# The router's ports (UPnP, Windows' own NAT traversal object); what it could not open is listed for the hoster.
if ($config.upnp) {
    $local = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.PrefixOrigin -in 'Dhcp', 'Manual' -and $_.IPAddress -notlike '169.*' } |
        Select-Object -First 1).IPAddress
    $failed = @()
    try {
        $mappings = (New-Object -ComObject HNetCfg.NATUPnP).StaticPortMappingCollection
        if (-not $mappings) { throw 'no UPnP router' }
        foreach ($p in $ports) {
            try { $null = $mappings.Add($p.Port, $p.Protocol, $p.Port, $local, $true, "Battlefield 1943 Reborn $($p.Port)") }
            catch { $failed += "$($p.Protocol) $($p.Port)" }
        }
    } catch { $failed = $ports | ForEach-Object { "$($_.Protocol) $($_.Port)" } }
    Set-Content (Join-Path $logs 'ports.txt') -Encoding ascii -Value $(if ($failed) { "Open these on your router: $($failed -join ', ')" } else { 'Router ports opened (UPnP).' })
}
if (-not $Quiet) { $started | ForEach-Object { "started server $_" } }

# The watchdog, one for this folder: a server that crashes is started again (watch_servers.ps1). When it calls this
# script itself to start a server again, it is the one running.
$watch = Join-Path $root 'watch_servers.ps1'
$watching = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($watch) })
if (-not $watching) {
    $null = Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile',
        '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', ('"' + $watch + '"'))
    if (-not $Quiet) { 'started the watchdog (watch_servers.ps1: crashed servers are started again)' }
}
