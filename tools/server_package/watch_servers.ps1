# Battlefield 1943 Reborn server: keeps the started servers running. start_servers.ps1 starts this (one per folder),
# stop_servers.ps1 stops it. Every 15 s each server that was started (its logs\server<n>\pids.txt: written by a start,
# removed by a stop, left behind by a crash) needs its game process and its host; when one is gone (a crash) the
# server is started again with start_servers.ps1 -Index <n>, which first takes its leftover (and so its entry in the
# server list) away. In the official package the main backend is started again when it is gone (the hosts register
# again by themselves). A server started again 3 times in 10 minutes waits 5 minutes before the next try.
# Log: logs\watch.log. (11 Nov 2026: official server 1 crashed with players on it; its host kept it in the list, and
# players joined a server that was not there until it was started by hand.)
$ErrorActionPreference = 'Continue'
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'server_config.ps1')
$log = Join-Path $ServerRoot 'logs\watch.log'
$start = Join-Path $ServerRoot 'start_servers.ps1'
function Note([string]$t) {
    try { Add-Content -Path $log -Value ('{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $t) -Encoding utf8 } catch {}
}
$official = Test-OfficialPackage
$history = @{}  # server number -> the times it was started again
$waitUntil = @{}
Note "watching the servers (PID $PID)"
while ($true) {
    Start-Sleep -Seconds 15
    try {
        $servers = @((Read-ServerConfig).servers)
        $all = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'bf1943.exe'" |
            Where-Object { $_.CommandLine -and $_.CommandLine.Contains($ServerRoot) })
        if ($official -and -not ($all | Where-Object { $_.CommandLine -match 'plasma\.py' })) {
            Note 'the main backend is gone: starting it again'
            & $start -BackendOnly -Quiet 2>&1 | ForEach-Object { Note "  $_" }
        }
        for ($n = 1; $n -le $servers.Count; $n++) {
            $dir = Get-ServerDir $n
            if (-not (Test-Path (Join-Path $dir 'pids.txt'))) { continue }  # not started, or stopped on purpose
            $mine = @($all | Where-Object { $_.CommandLine.Contains("\logs\server$n\") })
            $why = if (-not ($mine | Where-Object { $_.Name -eq 'bf1943.exe' })) { 'its game process is gone' }
                   elseif (-not ($mine | Where-Object { $_.Name -eq 'python.exe' })) { 'its host is gone' }
                   else { $null }
            if (-not $why) { continue }
            if ($waitUntil[$n] -and (Get-Date) -lt $waitUntil[$n]) { continue }
            $recent = @($history[$n] | Where-Object { $_ -gt (Get-Date).AddMinutes(-10) })
            if ($recent.Count -ge 3) {
                $waitUntil[$n] = (Get-Date).AddMinutes(5)
                Note "server ${n}: started again 3 times in 10 minutes; the next try in 5 minutes"
                continue
            }
            $crash = Get-ChildItem (Join-Path $dir 'user\crashes') -Filter '*.txt' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime | Select-Object -Last 1
            $note = if ($crash -and ((Get-Date) - $crash.LastWriteTime).TotalMinutes -lt 10) { " (crash report $($crash.Name))" } else { '' }
            Note "server $n ($($servers[$n - 1].name)): $why$note; starting it again"
            & $start -Index $n -Quiet 2>&1 | ForEach-Object { Note "  $_" }
            $history[$n] = @($recent) + @(Get-Date)
        }
    } catch {
        Note "error: $_"
    }
}
