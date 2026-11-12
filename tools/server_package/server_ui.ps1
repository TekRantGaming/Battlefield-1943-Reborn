# Battlefield 1943 Reborn server: the window (BF1943 Server.bat), version 1 of the 8 Nov mockups ("list and
# editor", SERVERS-PLAN.md section 7). Installs the game once from the host's own package, keeps the servers in
# server.json (server_config.ps1), and starts or stops them (start_servers.ps1, stop_servers.ps1, headless).
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'server_config.ps1')
$root = $ServerRoot
$game = Join-Path $root 'game'
$script:config = Read-ServerConfig
$official = Test-OfficialPackage

# The launcher's Field Report colours.
$bg = [System.Drawing.Color]::FromArgb(35, 39, 28)
$panel = [System.Drawing.Color]::FromArgb(28, 32, 22)
$bar = [System.Drawing.Color]::FromArgb(20, 23, 15)
$card = [System.Drawing.Color]::FromArgb(43, 48, 34)
$cream = [System.Drawing.Color]::FromArgb(236, 228, 204)
$paper = [System.Drawing.Color]::FromArgb(241, 234, 214)
$ink = [System.Drawing.Color]::FromArgb(36, 37, 31)
$khaki = [System.Drawing.Color]::FromArgb(185, 173, 124)
$green = [System.Drawing.Color]::FromArgb(110, 154, 60)
$faint = [System.Drawing.Color]::FromArgb(157, 149, 120)
$heading = 'Bahnschrift SemiBold Condensed'

# Tests (BF_SERVER_UI_SHOT): a window that opens off screen without taking the focus, drawn to a picture.
if ($env:BF_SERVER_UI_SHOT) {
    Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition 'public class BfQuietForm : System.Windows.Forms.Form { protected override bool ShowWithoutActivation { get { return true; } } }'
    $form = New-Object BfQuietForm
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point(-5000, -5000)
    $form.ShowInTaskbar = $false
} else {
    $form = New-Object System.Windows.Forms.Form
    $form.StartPosition = 'CenterScreen'
}
$form.Text = $(if ($official) { 'Battlefield 1943 Reborn Official Servers' } else { 'Battlefield 1943 Reborn Server' })
$form.ClientSize = New-Object System.Drawing.Size(1180, 760)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.BackColor = $bg
$form.ForeColor = $cream
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)

function New-Label([string]$text, [int]$x, [int]$y, [float]$size = 10, [string]$family = 'Segoe UI', $color = $null, $parent = $form) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.AutoSize = $true
    $l.Location = New-Object System.Drawing.Point($x, $y)
    $l.Font = New-Object System.Drawing.Font($family, $size)
    $l.BackColor = [System.Drawing.Color]::Transparent
    if ($color) { $l.ForeColor = $color }
    $parent.Controls.Add($l)
    return $l
}
function New-Button([string]$text, [int]$x, [int]$y, [int]$w, [int]$h, $back, $fore, $parent = $form) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.Location = New-Object System.Drawing.Point($x, $y)
    $b.Size = New-Object System.Drawing.Size($w, $h)
    $b.FlatStyle = 'Flat'
    $b.FlatAppearance.BorderSize = 0
    $b.BackColor = $back
    $b.ForeColor = $fore
    $b.Font = New-Object System.Drawing.Font($heading, 13)
    $parent.Controls.Add($b)
    return $b
}

# ---- the top: which package, the game
$null = New-Label $(if ($official) { 'OFFICIAL SERVER PACKAGE  ·  TRG STUDIOS ONLY' } else { 'COMMUNITY SERVER PACKAGE' }) 24 14 9 'Consolas' $khaki
$null = New-Label 'YOUR SERVERS' 20 32 30 $heading $cream
$gameLabel = New-Label '' 760 20 9.5 'Segoe UI' $khaki
$install = New-Button 'INSTALL THE GAME...' 900 44 256 32 $card $cream
$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(900, 80)
$progress.Size = New-Object System.Drawing.Size(256, 8)
$progress.Visible = $false
$form.Controls.Add($progress)
$line = New-Object System.Windows.Forms.Label
$line.Location = New-Object System.Drawing.Point(0, 92)
$line.Size = New-Object System.Drawing.Size(1180, 1)
$line.BackColor = [System.Drawing.Color]::FromArgb(58, 64, 48)
$form.Controls.Add($line)

function Update-Game {
    $ok = Test-Path (Join-Path $game 'default.xex')
    $gameLabel.Text = if ($ok) { 'Game: installed from your package' } else { 'Game: not installed yet (your Battlefield 1943 package, in 5841097E\000D0000)' }
    $install.Text = if ($ok) { 'INSTALL AGAIN...' } else { 'INSTALL THE GAME...' }
    $progress.Visible = $false
}
$install.Add_Click({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your Battlefield 1943 package (in 5841097E\000D0000)'
    $dialog.Filter = 'Battlefield 1943 package (any file)|*.*'
    if ($dialog.ShowDialog() -ne 'OK') { return }
    $size = (Get-Item $dialog.FileName).Length
    $progress.Visible = $true
    $progress.Value = 0
    $install.Enabled = $false
    $out = Join-Path $env:TEMP 'bf1943_install.txt'
    $p = Start-Process -FilePath (Join-Path $root 'bin\bf1943.exe') -ArgumentList @('--install_game="' + $dialog.FileName + '"',
        '--game_data_root="' + $game + '"', '--user_data_root="' + (Join-Path $root 'logs\install') + '"', '--server') `
        -WindowStyle Hidden -RedirectStandardOutput $out -PassThru
    while (-not $p.HasExited) {
        $done = (Get-ChildItem $game -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        $progress.Value = [Math]::Min(100, [int](100 * $done / [Math]::Max(1, $size)))
        [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 200
    }
    $result = Get-Content $out -ErrorAction SilentlyContinue
    $install.Enabled = $true
    Update-Game
    if (-not (Test-Path (Join-Path $game 'default.xex'))) {
        [System.Windows.Forms.MessageBox]::Show("The game could not be installed.`r`n$result", $form.Text) | Out-Null
    }
})

# ---- the left: the servers
$left = New-Object System.Windows.Forms.Panel
$left.Location = New-Object System.Drawing.Point(0, 93)
$left.Size = New-Object System.Drawing.Size(320, 601)
$left.BackColor = $panel
$form.Controls.Add($left)
$list = New-Object System.Windows.Forms.ListBox
$list.Location = New-Object System.Drawing.Point(14, 14)
$list.Size = New-Object System.Drawing.Size(292, 430)
$list.BorderStyle = 'None'
$list.BackColor = $panel
$list.ForeColor = $cream
$list.DrawMode = 'OwnerDrawFixed'
$list.ItemHeight = 58
$left.Controls.Add($list)
$add = New-Button '+  ADD A SERVER' 14 452 292 36 $card $cream $left
$remove = New-Button 'REMOVE THIS SERVER' 14 494 292 30 $panel $khaki $left
$remove.Font = New-Object System.Drawing.Font('Segoe UI', 9)
$memory = New-Label '' 14 536 8.5 'Segoe UI' $faint $left
$memory.MaximumSize = New-Object System.Drawing.Size(292, 60)

$script:states = @{}
function Update-States {
    $procs = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'bf1943.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($root) })
    $total = 0
    for ($n = 1; $n -le $script:config.servers.Count; $n++) {
        $mine = @($procs | Where-Object { $_.CommandLine.Contains("\logs\server$n\") })
        $total += ($mine | Measure-Object WorkingSetSize -Sum).Sum
        $script:states[$n] = [pscustomobject]@{ running = ($mine.Count -ge 2); status = (Get-ServerStatus $n) }
    }
    $running = @($script:states.Values | Where-Object { $_.running }).Count
    $memory.Text = if ($running) { "$running running, $([int]($total / 1MB)) MB of memory in all." } else { 'None running.' }
}
$list.Add_DrawItem({
    param($sender, $e)
    if ($e.Index -lt 0) { return }
    $n = $e.Index + 1
    $s = $script:config.servers[$e.Index]
    $sel = ($e.State -band [System.Windows.Forms.DrawItemState]::Selected) -ne 0
    $r = $e.Bounds
    $back = if ($sel) { $green } else { $card }
    $e.Graphics.FillRectangle((New-Object System.Drawing.SolidBrush $back), $r.X, $r.Y + 3, $r.Width, $r.Height - 6)
    $st = $script:states[$n]
    $dot = if ($st -and $st.running -and $st.status -and $st.status.state -eq 'registered') { [System.Drawing.Color]::FromArgb(127, 191, 74) }
           elseif ($st -and $st.running) { [System.Drawing.Color]::FromArgb(201, 162, 39) } else { [System.Drawing.Color]::FromArgb(138, 134, 114) }
    $e.Graphics.FillEllipse((New-Object System.Drawing.SolidBrush $dot), $r.X + 10, $r.Y + 16, 9, 9)
    $fg = if ($sel) { [System.Drawing.Color]::White } else { $cream }
    $e.Graphics.DrawString($(if ($s.name) { $s.name } else { "Server $n" }), (New-Object System.Drawing.Font('Segoe UI Semibold', 10.5)),
        (New-Object System.Drawing.SolidBrush $fg), $r.X + 26, $r.Y + 9)
    $maps = (@($s.maps) | ForEach-Object { $MapTitles[$_] }) -join ', '
    $what = if (-not $st -or -not $st.running) { 'stopped' } elseif (-not $st.status) { 'starting' }
            elseif ($st.status.state -eq 'refused') { 'refused by the backend' }
            else { "$([int]$st.status.players) playing" + $(if ($st.status.reachable -eq $false) { ', NOT reachable' } else { '' }) }
    $sub = "$maps  ·  $what  ·  UDP $($BasePort + $n - 1)"
    $e.Graphics.DrawString($sub, (New-Object System.Drawing.Font('Segoe UI', 8.5)),
        (New-Object System.Drawing.SolidBrush $(if ($sel) { [System.Drawing.Color]::FromArgb(230, 240, 220) } else { $khaki })), $r.X + 26, $r.Y + 31)
})

# ---- the right: the selected server
$right = New-Object System.Windows.Forms.Panel
$right.Location = New-Object System.Drawing.Point(320, 93)
$right.Size = New-Object System.Drawing.Size(860, 601)
$right.BackColor = $bg
$form.Controls.Add($right)
function New-Field($control, [int]$x, [int]$y, [int]$w, [int]$h) {
    $control.Location = New-Object System.Drawing.Point($x, $y)
    $control.Size = New-Object System.Drawing.Size($w, $h)
    $control.BackColor = $paper
    $control.ForeColor = $ink
    $right.Controls.Add($control)
    return $control
}
$null = New-Label 'Server name, shown in the server browser' 28 18 9.5 'Segoe UI' $khaki $right
$name = New-Field (New-Object System.Windows.Forms.TextBox) 28 42 800 28
$name.MaxLength = 48
$name.BorderStyle = 'None'
$name.Font = New-Object System.Drawing.Font('Segoe UI', 12)
$null = New-Label 'Description, shown when players select it (200 characters)' 28 84 9.5 'Segoe UI' $khaki $right
$desc = New-Field (New-Object System.Windows.Forms.TextBox) 28 108 800 52
$desc.Multiline = $true
$desc.MaxLength = 200
$desc.BorderStyle = 'None'

$null = New-Label 'Maps, played in this order' 28 176 9.5 'Segoe UI' $khaki $right
$maps = New-Field (New-Object System.Windows.Forms.CheckedListBox) 28 200 330 124
$maps.CheckOnClick = $true
$maps.BorderStyle = 'None'
$maps.BackColor = $card
$maps.ForeColor = $cream
$maps.Font = New-Object System.Drawing.Font('Segoe UI', 11)
$up = New-Button 'UP' 366 200 70 30 $card $cream $right
$down = New-Button 'DOWN' 366 236 70 30 $card $cream $right

$null = New-Label 'Movement and aiming, for every player on it' 470 176 9.5 'Segoe UI' $khaki $right
$movement = New-Field (New-Object System.Windows.Forms.ComboBox) 470 200 358 28
$movement.DropDownStyle = 'DropDownList'
[void]$movement.Items.Add('Classic (the original game)')
[void]$movement.Items.Add('Modern (faster movement and aiming)')
$null = New-Label 'Pause between rounds' 470 240 9.5 'Segoe UI' $khaki $right
$delay = New-Field (New-Object System.Windows.Forms.ComboBox) 470 264 358 28
$delay.DropDownStyle = 'DropDownList'
$delays = @(30, 45, 60, 90)
foreach ($d in $delays) { [void]$delay.Items.Add("$d seconds") }
# Night Ops (TekRantGaming, 11 Nov 2026): the maps at night in players' games (0.2.4 and newer).
$night = New-Object System.Windows.Forms.CheckBox
$night.Text = 'Night Ops: players see the maps at night'
$night.Location = New-Object System.Drawing.Point(470, 304)
$night.Size = New-Object System.Drawing.Size(358, 28)
$night.ForeColor = $cream
$night.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$night.Visible = (Test-NightOpsPackage)  # test packages only until Night Ops is released (server_config.ps1)
$right.Controls.Add($night)

$box = New-Object System.Windows.Forms.Panel
$box.Location = New-Object System.Drawing.Point(28, 346)
$box.Size = New-Object System.Drawing.Size(800, 150)
$box.BackColor = $card
$right.Controls.Add($box)
$listed = New-Object System.Windows.Forms.CheckBox
$listed.Text = $(if ($official) { 'Show in the server browser (the Official list)' } else { 'Show in the server browser (the Community list)' })
$listed.Location = New-Object System.Drawing.Point(14, 12)
$listed.Size = New-Object System.Drawing.Size(760, 26)
$box.Controls.Add($listed)
$null = New-Label 'No address to type: the main backend sees where the server is, and players never see it in the list. Unticked: only players who type its address (Direct connect) can join.' 34 40 8.5 'Segoe UI' $faint $box
$box.Controls[$box.Controls.Count - 1].MaximumSize = New-Object System.Drawing.Size(740, 40)
$upnp = New-Object System.Windows.Forms.CheckBox
$upnp.Text = 'Open the ports on my router automatically (UPnP), for every server'
$upnp.Location = New-Object System.Drawing.Point(14, 82)
$upnp.Size = New-Object System.Drawing.Size(760, 26)
$box.Controls.Add($upnp)
$keyNote = New-Label '' 14 114 8.5 'Segoe UI' $khaki $box
if ($official) {
    $keyNote.Text = 'This PC runs the main backend. official.key: ' +
        $(if (Test-Path (Join-Path $root 'official.key')) { 'found, your servers show as official.' } else { 'MISSING: copy it next to this window from your own copy.' })
} else {
    $keyNote.Text = 'Port of this server: UDP ' + $BasePort + ' and on, one each server (open them on your router if UPnP cannot).'
}
$statusLine = New-Label '' 28 512 9.5 'Segoe UI' $khaki $right
$statusLine.MaximumSize = New-Object System.Drawing.Size(800, 80)

# ---- the bottom
$bottom = New-Object System.Windows.Forms.Panel
$bottom.Location = New-Object System.Drawing.Point(0, 694)
$bottom.Size = New-Object System.Drawing.Size(1180, 66)
$bottom.BackColor = $bar
$form.Controls.Add($bottom)
$startAll = New-Button 'START ALL' 24 14 150 38 $green ([System.Drawing.Color]::White) $bottom
$stopAll = New-Button 'STOP ALL' 184 14 150 38 ([System.Drawing.Color]::FromArgb(58, 64, 48)) $cream $bottom
$startOne = New-Button 'START OR RESTART THIS SERVER' 344 14 300 38 $bar $cream $bottom
$startOne.FlatAppearance.BorderSize = 1
$startOne.FlatAppearance.BorderColor = $khaki
$stopOne = New-Button 'STOP THIS SERVER' 654 14 200 38 $bar $cream $bottom
$stopOne.FlatAppearance.BorderSize = 1
$stopOne.FlatAppearance.BorderColor = $khaki
$portsLine = New-Label '' 870 24 9 'Segoe UI' $khaki $bottom

# ---- the editor bound to the selected server
$script:loading = $false
function Show-Server {
    $i = $list.SelectedIndex
    if ($i -lt 0) { return }
    $s = $script:config.servers[$i]
    $script:loading = $true
    $name.Text = $s.name
    $desc.Text = $s.description
    $maps.Items.Clear()
    $order = @($s.maps) + @($MapTitles.Keys | Where-Object { @($s.maps) -notcontains $_ })
    foreach ($k in $order) { $j = $maps.Items.Add($MapTitles[$k]); $maps.SetItemChecked($j, @($s.maps) -contains $k) }
    $movement.SelectedIndex = $(if ($s.movement -eq 'modern') { 1 } else { 0 })
    $d = [Array]::IndexOf($delays, [int]$s.round_end_delay)
    $delay.SelectedIndex = $(if ($d -ge 0) { $d } else { 1 })
    $listed.Checked = [bool]$s.listed
    $night.Checked = [bool]$s.night
    $script:loading = $false
    Update-StatusLine
}
function Read-Editor {
    if ($script:loading) { return }
    $i = $list.SelectedIndex
    if ($i -lt 0) { return }
    $s = $script:config.servers[$i]
    $s.name = $name.Text.Trim()
    $s.description = ($desc.Text -replace "[`r`n]+", ' ').Trim()
    $keys = @()
    for ($j = 0; $j -lt $maps.Items.Count; $j++) {
        if ($maps.GetItemChecked($j)) { $keys += @($MapTitles.Keys | Where-Object { $MapTitles[$_] -eq $maps.Items[$j] }) }
    }
    $s.maps = $keys
    $s.movement = $(if ($movement.SelectedIndex -eq 1) { 'modern' } else { 'classic' })
    $s.round_end_delay = $delays[[Math]::Max(0, $delay.SelectedIndex)]
    $s.listed = $listed.Checked
    $s.night = $night.Checked
    $script:config.upnp = $upnp.Checked
    Save-ServerConfig $script:config
    $list.Invalidate()
}
foreach ($c in @($name, $desc)) { $c.Add_TextChanged({ Read-Editor }) }
$maps.Add_ItemCheck({ if (-not $script:loading -and $form.IsHandleCreated) { $form.BeginInvoke([Action] { Read-Editor }) | Out-Null } })
foreach ($c in @($movement, $delay)) { $c.Add_SelectedIndexChanged({ Read-Editor }) }
foreach ($c in @($listed, $upnp, $night)) { $c.Add_CheckedChanged({ Read-Editor }) }
function Move-Map([int]$by) {
    $j = $maps.SelectedIndex
    $k = $j + $by
    if ($j -lt 0 -or $k -lt 0 -or $k -ge $maps.Items.Count) { return }
    $text = $maps.Items[$j]; $on = $maps.GetItemChecked($j)
    $script:loading = $true
    $maps.Items.RemoveAt($j); $maps.Items.Insert($k, $text); $maps.SetItemChecked($k, $on); $maps.SelectedIndex = $k
    $script:loading = $false
    Read-Editor
}
$up.Add_Click({ Move-Map -1 })
$down.Add_Click({ Move-Map 1 })
$list.Add_SelectedIndexChanged({ Show-Server })
function Update-List([int]$select) {
    $list.Items.Clear()
    for ($n = 1; $n -le $script:config.servers.Count; $n++) { [void]$list.Items.Add("server $n") }
    if ($list.Items.Count) { $list.SelectedIndex = [Math]::Min([Math]::Max(0, $select), $list.Items.Count - 1) }
}
$add.Add_Click({
    if ($script:config.servers.Count -ge $MaxServers) { [System.Windows.Forms.MessageBox]::Show("At most $MaxServers servers.", $form.Text) | Out-Null; return }
    $n = $script:config.servers.Count + 1
    $first = $script:config.servers[0]
    $s = New-ServerEntry $(if ($first.name) { "$($first.name) $n" } else { "Server $n" })
    $script:config.servers = @($script:config.servers) + $s
    Save-ServerConfig $script:config
    Update-List ($n - 1)
})
$remove.Add_Click({
    $i = $list.SelectedIndex
    if ($i -lt 0 -or $script:config.servers.Count -le 1) { return }
    $n = $i + 1
    if ([System.Windows.Forms.MessageBox]::Show("Remove server $n ($($script:config.servers[$i].name))? It stops if it runs; the servers after it move up one port.",
        $form.Text, 'YesNo') -ne 'Yes') { return }
    & (Join-Path $root 'stop_servers.ps1') -Index $n
    $script:config.servers = @($script:config.servers | Select-Object -Index (@(0..($script:config.servers.Count - 1)) | Where-Object { $_ -ne $i }))
    Save-ServerConfig $script:config
    Update-List ($i - 1)
})
function Update-StatusLine {
    $i = $list.SelectedIndex
    $ports = Join-Path $root 'logs\ports.txt'
    $portsLine.Text = if (Test-Path $ports) { (Get-Content $ports -Raw).Trim() } else { '' }
    if ($i -lt 0) { $statusLine.Text = ''; return }
    $n = $i + 1
    $st = $script:states[$n]
    $statusLine.Text = if (-not $st -or -not $st.running) { "Server $n is stopped." }
        elseif (-not $st.status) { "Server $n is starting." }
        elseif ($st.status.state -eq 'refused') { "Server $n was refused by the main backend: $($st.status.why)" }
        else {
            "Server $n runs on UDP $($BasePort + $n - 1): $([int]$st.status.players) playing" +
            $(if ($st.status.map) { " on $($st.status.map)" } else { '' }) + '. ' +
            $(if (-not $st.status.listed) { 'Not listed (Direct connect only). ' } elseif ($st.status.official) { 'Listed as official. ' } else { 'Listed in Community. ' }) +
            $(if ($st.status.reachable -eq $true) { 'Players can reach it.' } elseif ($st.status.reachable -eq $false) {
                "Players CANNOT reach it: open UDP $($BasePort + $n - 1) on your router or firewall." }
              elseif ($st.status.reachable_local) {
                "It answers, but this PC runs the main backend, so whether players outside can reach it cannot be checked from here: open UDP $($BasePort + $n - 1) in every firewall and try the game's server browser from another network." }
              else { 'Checking whether players can reach it...' })
        }
}
function Start-Servers([int]$index) {
    Read-Editor
    if (-not (Test-Path (Join-Path $game 'default.xex'))) { [System.Windows.Forms.MessageBox]::Show('Install the game first.', $form.Text) | Out-Null; return }
    $form.Cursor = 'WaitCursor'
    try { & (Join-Path $root 'start_servers.ps1') -Quiet -Index $index } catch { [System.Windows.Forms.MessageBox]::Show("$_", $form.Text) | Out-Null }
    $form.Cursor = 'Default'
    Update-States; $list.Invalidate(); Update-StatusLine
}
$startAll.Add_Click({ Start-Servers 0 })
$startOne.Add_Click({ if ($list.SelectedIndex -ge 0) { Start-Servers ($list.SelectedIndex + 1) } })
$stopAll.Add_Click({ & (Join-Path $root 'stop_servers.ps1'); Update-States; $list.Invalidate(); Update-StatusLine })
$stopOne.Add_Click({ if ($list.SelectedIndex -ge 0) { & (Join-Path $root 'stop_servers.ps1') -Index ($list.SelectedIndex + 1); Update-States; $list.Invalidate(); Update-StatusLine } })

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2000
$timer.Add_Tick({ Update-States; $list.Invalidate(); Update-StatusLine })
$timer.Start()
$form.Add_FormClosing({ Read-Editor; $timer.Stop() })

$upnp.Checked = [bool]$script:config.upnp
Update-Game
Update-States
Update-List 0
# Tests: BF_SERVER_UI_SHOT=<png> saves the window as a picture and closes it.
if ($env:BF_SERVER_UI_SHOT) {
    if ($env:BF_SERVER_UI_SELECT) { $list.SelectedIndex = [int]$env:BF_SERVER_UI_SELECT }
    $form.Show()
    for ($k = 0; $k -lt 20; $k++) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 50 }
    $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
    $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
    $bmp.Save($env:BF_SERVER_UI_SHOT)
    $timer.Stop()
    $form.Close()
    return
}
[void]$form.ShowDialog()
