# Battlefield 1943 Reborn server: the settings every script and the window share (dot sourced).
# server.json, version 2 (SERVERS-PLAN.md section 7):
#   servers   a list, one entry each server this PC runs:
#     name             its name in the server browser
#     description      shown when a player selects it (200 characters)
#     maps             the rotation in order: wake, guadalcanal, iwo, coral (the next one when a round ends)
#     movement         "classic" (the original game) or "modern" (faster movement and aiming); every player on it
#     round_end_delay  seconds of the end of round screen before the next map (45)
#     listed           true: in the server browser; false: joined by Direct connect only
#   upnp      ask the router to open the servers' ports
#   backend   the main backend's address (the community package: the official one; set by the package)
# The official package (package.json "official": true, TekRantGaming only) runs the main backend on this PC and marks its
# servers official with official.key (next to this script; never in a download).
# A version 1 file (one server: name, maps, movement, backend, public_address, upnp) is read as one server.

$ServerRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ConfigPath = Join-Path $ServerRoot 'server.json'
$MapTitles = [ordered]@{ wake = 'Wake Island'; guadalcanal = 'Guadalcanal'; iwo = 'Iwo Jima'; coral = 'Coral Sea' }
$MapLevels = @{ wake = 'Levels/Wake_island_s'; guadalcanal = 'Levels/Guadal_Canal'; iwo = 'Levels/Iwo_Jima_s'; coral = 'Levels/Coral_sea' }
$BasePort = 18300
$MaxServers = 16

function Test-OfficialPackage {
    $p = Join-Path $ServerRoot 'package.json'
    if (-not (Test-Path $p)) { return $false }
    try { return [bool]((Get-Content $p -Raw | ConvertFrom-Json).official) } catch { return $false }
}

# Night Ops (package.json "night_ops": true, test packages only until it is released): the window's Night Ops box
# and the hosts' --night. Without it a server is never a night one, whatever server.json says.
function Test-NightOpsPackage {
    $p = Join-Path $ServerRoot 'package.json'
    if (-not (Test-Path $p)) { return $false }
    try { return [bool]((Get-Content $p -Raw | ConvertFrom-Json).night_ops) } catch { return $false }
}

function New-ServerEntry([string]$name) {
    [pscustomobject]@{ name = $name; description = ''; maps = @('wake', 'guadalcanal', 'iwo', 'coral'); movement = 'classic'
                       round_end_delay = 45; listed = $true; night = $false }
}

function Read-ServerConfig {
    $default = Join-Path $ServerRoot 'server.default.json'
    if (-not (Test-Path $ConfigPath) -and (Test-Path $default)) { Copy-Item $default $ConfigPath }
    $raw = if (Test-Path $ConfigPath) { Get-Content $ConfigPath -Raw | ConvertFrom-Json } else { $null }
    $c = [pscustomobject]@{ version = 2; servers = @(); upnp = $true; backend = '127.0.0.1'; public_address = '' }
    if (-not $raw) {
        $c.servers = @(New-ServerEntry 'Battlefield 1943 Reborn')
        return $c
    }
    if ($null -ne $raw.upnp) { $c.upnp = [bool]$raw.upnp }
    if ($raw.backend) { $c.backend = [string]$raw.backend }
    if ($raw.public_address) { $c.public_address = [string]$raw.public_address }
    if ($raw.servers) {
        $c.servers = @($raw.servers | ForEach-Object {
            $s = New-ServerEntry ([string]$_.name)
            if ($null -ne $_.description) { $s.description = [string]$_.description }
            if ($_.maps) { $s.maps = @($_.maps | Where-Object { $MapTitles.Contains([string]$_) }) }
            if ($_.movement -eq 'modern') { $s.movement = 'modern' }
            if ($_.round_end_delay) { $s.round_end_delay = [int]$_.round_end_delay }
            if ($null -ne $_.listed) { $s.listed = [bool]$_.listed }
            if ($null -ne $_.night) { $s.night = [bool]$_.night }  # Night Ops: its maps at night in players' games
            $s
        })
    } else {
        # Version 1: one server.
        $s = New-ServerEntry $(if ($raw.name) { [string]$raw.name } else { 'Battlefield 1943 Reborn' })
        if ($raw.maps) { $s.maps = @($raw.maps | Where-Object { $MapTitles.Contains([string]$_) }) }
        if ($raw.movement -eq 'modern') { $s.movement = 'modern' }
        if ($raw.round_end_delay) { $s.round_end_delay = [int]$raw.round_end_delay }
        $c.servers = @($s)
    }
    if (-not $c.servers) { $c.servers = @(New-ServerEntry 'Battlefield 1943 Reborn') }
    return $c
}

function Save-ServerConfig($c) {
    $out = [ordered]@{ version = 2; servers = @($c.servers); upnp = [bool]$c.upnp; backend = $c.backend }
    if ($c.public_address) { $out.public_address = $c.public_address }
    $out | ConvertTo-Json -Depth 5 | Set-Content $ConfigPath -Encoding utf8
}

# This PC's public address, for the official package's main backend (players' games connect to it): the router's
# (UPnP), else this PC's own public one (a hosted server's). The community package needs none: the main backend sees
# where each server registers from.
function Get-PublicAddress($c) {
    if ($c.public_address) { return $c.public_address }
    $ip = $null
    try { $ip = (New-Object -ComObject HNetCfg.NATUPnP).StaticPortMappingCollection | ForEach-Object { $_.ExternalIPAddress } | Select-Object -First 1 } catch {}
    if (-not $ip) {
        $ip = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notmatch '^(10\.|127\.|169\.254\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)' } |
            Select-Object -First 1).IPAddress
    }
    if ($ip) { return $ip } else { return '127.0.0.1' }
}

function Get-ServerDir([int]$n) { Join-Path $ServerRoot "logs\server$n" }  # n from 1

# A server's state as its host writes it (gmhost.py --status-file), or $null.
function Get-ServerStatus([int]$n) {
    $f = Join-Path (Get-ServerDir $n) 'status.json'
    if (-not (Test-Path $f)) { return $null }
    try { return Get-Content $f -Raw | ConvertFrom-Json } catch { return $null }
}

# The processes of server n (its folder is in their command lines), or of the whole package with n = 0.
function Get-ServerProcesses([int]$n = 0) {
    $mark = if ($n -gt 0) { "\logs\server$n\" } else { $ServerRoot }
    @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'bf1943.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($ServerRoot) -and $_.CommandLine.Contains($mark) })
}
