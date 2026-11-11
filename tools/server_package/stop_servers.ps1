# Battlefield 1943 Reborn server: stops what start_servers.ps1 started.
#   stop_servers.ps1          every server and the main backend
#   stop_servers.ps1 -Index 2 only the second server
# Only processes whose command line names this folder (and, for one server, its logs\server<n> folder) are stopped:
# a PID saved before a restart of the PC may belong to another program now.
param([int]$Index = 0)
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'server_config.ps1')
Get-ServerProcesses $Index | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
if ($Index -eq 0) {
    Remove-Item (Join-Path $ServerRoot 'logs\pids.txt'), (Join-Path $ServerRoot 'logs\backend.pid') -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $ServerRoot 'logs') -Directory -Filter 'server*' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item (Join-Path $_.FullName 'pids.txt'), (Join-Path $_.FullName 'status.json') -ErrorAction SilentlyContinue }
} else {
    Remove-Item (Join-Path (Get-ServerDir $Index) 'pids.txt'), (Join-Path (Get-ServerDir $Index) 'status.json') -ErrorAction SilentlyContinue
}
