# Battlefield 1943 Reborn server: lets players in through Windows Firewall (run once, as an administrator; a hosted
# server such as Windows Server blocks incoming connections by default). The main backend's ports only matter on the
# PC that runs it (the official servers).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $MyInvocation.MyCommand.Path + '"')
    return
}
$rules = @(
    # Every server's port: UDP 18300 for the first, 18301 for the second and on (server_config.ps1, at most 16).
    @{ Name = 'Battlefield 1943 Reborn servers (UDP 18300 to 18315)'; Protocol = 'UDP'; Port = '18300-18315' },
    @{ Name = 'Battlefield 1943 Reborn backend login (TCP 18241)'; Protocol = 'TCP'; Port = 18241 },
    @{ Name = 'Battlefield 1943 Reborn backend games (TCP 18246)'; Protocol = 'TCP'; Port = 18246 },
    @{ Name = 'Battlefield 1943 Reborn backend server list (TCP 18250)'; Protocol = 'TCP'; Port = 18250 })
foreach ($r in $rules) {
    if (-not (Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow -Protocol $r.Protocol -LocalPort $r.Port | Out-Null
    }
}
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.MessageBox]::Show('Windows Firewall now lets players in (TCP 18241, 18246, 18250; UDP 18300 to 18315).', 'Battlefield 1943 Reborn Server') | Out-Null
