# The release downloads (Battlefield 1943 Reborn, github.com/TekRantGaming/Battlefield-1943-Reborn, public):
#   runs\release\<version>\BF1943-Reborn-<version>-windows-x64.zip     the game for players
#   runs\release\<version>\BF1943-Reborn-Server-<version>-windows.zip  the community server package (any number of servers)
#   runs\release\<version>\BF1943-Reborn-linux-x86_64.AppImage  with -AppImage: the Linux game (tools\linux), its name the
#   same in every release (King Kong's rule since its v2.0.0): a new download keeps the name of the one a Steam shortcut
#   starts, and the updater writes each version over the file a player has, whatever its name
#   runs\release\<version>\BF1943-Reborn-OfficialServers-<version>-windows.zip  with -Official: TekRantGaming's private package
#   (the main backend and the official servers; never published, never in the release's sums)
#   with -LinuxFiles: the other Linux downloads this version built (WSL, tools\linux\build_flatpak.sh and
#   build_server.sh): the game's Flatpak, the server's Flatpak, AppImage and plain archive. They are copied in and
#   summed; a running server manager updates itself from the server's zip (Windows) or plain archive (Linux).
# from the release build (bf\out\build\bf-relwithdebinfo), with the licences of everything in them. No game file goes
# in: players and hosts bring their own copy, installed once by the first start's setup (or the server's window). Nothing is uploaded: the release itself is made by hand on GitHub (reborn\docs\RELEASES.md).
# Usage: powershell -File tools\make_packages.ps1 -Version 0.1.0 -OnlineHost <address> (the main backend: the official one, or 127.0.0.1 for local tests)
param([Parameter(Mandatory = $true)][string]$Version, [string]$Preset = "bf-relwithdebinfo",
      [Parameter(Mandatory = $true)][string]$OnlineHost,  # the main backend (the official servers' address or name; no default: 0.1.5 and 0.1.6 went out pointing at 127.0.0.1)
      [switch]$Official,  # also the private official server package (TekRantGaming only; never uploaded)
      [string]$AppImage = "",  # the Linux AppImage this version built (WSL, tools\linux\build_appimage.sh): a release file
      [string[]]$LinuxFiles = @(),  # the other Linux downloads (*.flatpak, the server's *.AppImage and *-linux.tar.gz)
      # The Python the server package carries is made from this one (python.org's Windows build).
      [string]$Python = $(if ($env:BF_PYTHON) { $env:BF_PYTHON } else { "python" }))  # Python 3.12
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "bf\out\build\$Preset"
$nxr = Join-Path $root "bf\third_party\native-x-renderer"
$sdk = Join-Path $root "sdk\src-bf"
$out = Join-Path $root "runs\release\$Version"
if (Test-Path $out) { throw "release folder exists: $out" }
# The program's own version (CMake's BF_VERSION, what BfVersion() says) must be the release's exactly: the updater
# compares the release tag with it (a tag 0.2.1-preview is older than a program 0.2.1) and the first start check
# skips a bf-files.txt of another version. 0.1.x shipped 0.1.33 inside 0.1.33-preview.
$built = (Select-String -Path (Join-Path $build "CMakeCache.txt") -Pattern '^BF_VERSION:STRING=(.*)$').Matches.Groups[1].Value
if ($built -ne $Version) { throw "the build is version '$built', not ${Version}: build it with that version first (cmake -DBF_VERSION=$Version)" }
# Night Ops (bf/src/night_ops.h) is not in releases until TekRantGaming says it is ready (12 Nov 2026): only a test
# build (a version with -test) may have it.
$nightOps = (Select-String -Path (Join-Path $build "CMakeCache.txt") -Pattern '^BF_NIGHT_OPS:BOOL=(ON|TRUE|1)$' -Quiet)
if ($nightOps -and $Version -notmatch '-test') { throw "the build has Night Ops (BF_NIGHT_OPS ON): not for a release; build it with -DBF_NIGHT_OPS=OFF" }
# The picture in picture sniper scope (bf/src/scope_pip.h): taken out of 0.4.1 at TekRantGaming's word (13 Nov 2026);
# only a test build may have it.
$scopePip = (Select-String -Path (Join-Path $build "CMakeCache.txt") -Pattern '^BF_SCOPE_PIP:BOOL=(ON|TRUE|1)$' -Quiet)
if ($scopePip -and $Version -notmatch '-test') { throw "the build has the picture in picture scope (BF_SCOPE_PIP ON): not for a release; build it with -DBF_SCOPE_PIP=OFF" }
New-Item -ItemType Directory -Force $out | Out-Null

# Only our binaries: the game exe, its runtime and renderer, and the vendor libraries it loads: NVIDIA's DLSS, AMD's FSR
# SDK 2.4.0 loader and upscaler, Intel's XeSS (0.4.1, pull request 16: NativeXRenderer D167). Not the Xenos plugin
# (development only), nor AMD's FidelityFX SDK 1.1.4 library (frame generation's, not offered since 9 Nov 2026 and no
# longer fetched). The player download also gets data\shaders, our shader list (New-Bin -Shaders).
$binFiles = @("bf1943.exe", "rexruntime.dll", "rexgpu-bfnative.dll", "nvngx_dlss.dll", "amd_fidelityfx_loader_vk.dll",
              "amd_fidelityfx_upscaler_vk.dll", "libxess.dll")

function Copy-Licences([string]$dest) {
    $l = Join-Path $dest "licences"
    New-Item -ItemType Directory -Force $l | Out-Null
    Copy-Item (Join-Path $nxr "LICENSE") (Join-Path $l "NativeXRenderer.txt")
    Copy-Item (Join-Path $nxr "THIRD_PARTY_NOTICES.md") (Join-Path $l "NativeXRenderer third party notices.md")
    foreach ($p in @(@("diligent-core\License.txt", "DiligentCore.txt"), @("diligent-core\ThirdParty\SPIRV-Cross\LICENSE", "SPIRV-Cross.txt"),
                     @("diligent-core\ThirdParty\volk\LICENSE.md", "volk.txt"), @("fsr1\LICENSE.txt", "AMD FSR 1.txt"),
                     @("cas\LICENSE.txt", "AMD CAS.txt"), @("ffx_api\LICENSE.txt", "AMD FidelityFX SDK.txt"),
                     @("ffx_api2\LICENSE.txt", "AMD FSR SDK.txt"), @("xess\LICENSE.txt", "Intel XeSS.txt"),
                     @("nis\LICENSE.txt", "NVIDIA Image Scaling.txt"), @("smaa\LICENSE.txt", "SMAA.txt"),
                     @("king-kong-recompiled\LICENSE", "King Kong Recompiled.txt"))) {
        Copy-Item (Join-Path $nxr "thirdparty\$($p[0])") (Join-Path $l $p[1])
    }
    $dlss = Join-Path $build "_deps\dlss-headers-src\LICENSE.txt"
    if (Test-Path $dlss) { Copy-Item $dlss (Join-Path $l "NVIDIA DLSS.txt") } else { throw "the DLSS licence is missing: $dlss" }
    # XeGTAO's notice is in the shader port's header.
    (Get-Content (Join-Path $nxr "src\gtao_shaders.h") -TotalCount 30 | Where-Object { $_ -like "//*" }) -replace '^// ?', '' |
        Set-Content (Join-Path $l "XeGTAO.txt") -Encoding utf8
    Copy-Item (Join-Path $root "bf\fonts\OFL.txt") (Join-Path $l "Oswald font.txt")
    # The menu music's EA block layout follows vgmstream (bf\src\game_music.cpp; ISC licence).
    Copy-Item (Join-Path $root "bf\licences\vgmstream.txt") (Join-Path $l "vgmstream.txt")
    Copy-Item (Join-Path $root "bf\glyphs\LICENSE.txt") (Join-Path $l "Button prompts.txt")
    # Intel's CMAA2 (Apache License 2.0, Copyright (c) 2018 Intel Corporation), ported into the renderer's shaders (pull
    # request 16, src/cmaa_shaders.h): the licence's text, from the Vulkan headers' copy.
    Copy-Item (Join-Path $sdk "thirdparty\vulkan-headers\LICENSES\Apache-2.0.txt") (Join-Path $l "CMAA2 (Apache License 2.0).txt")
    Copy-Item (Join-Path $sdk "LICENSE") (Join-Path $l "ReXGlue SDK.txt")
    # Everything the SDK builds into its runtime: each third party folder's licence.
    $sdkl = Join-Path $l "ReXGlue SDK third party"
    New-Item -ItemType Directory -Force $sdkl | Out-Null
    Get-ChildItem (Join-Path $sdk "thirdparty") -Directory | ForEach-Object {
        $f = Get-ChildItem $_.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' } | Select-Object -First 1
        if ($f) { Copy-Item $f.FullName (Join-Path $sdkl "$($_.Name).txt") }
    }
    # FFmpeg is built with its decoders only, no GPL parts (LGPL 2.1); libmspack (the executable's LZX) is LGPL 2.1.
    Copy-Item (Join-Path $sdk "thirdparty\FFmpeg\COPYING.LGPLv2.1") (Join-Path $sdkl "FFmpeg.txt") -Force
    Copy-Item (Join-Path $sdk "thirdparty\libmspack\libmspack\COPYING.LIB") (Join-Path $sdkl "libmspack.txt")
}

function New-Bin([string]$dest, [switch]$Shaders, [switch]$Flat) {
    # The player's download keeps bf1943.exe at its top, started by itself (TekRantGaming, 7 Nov 2026: an exe, never a .bat or
    # a script; King Kong's layout). The server package keeps bin\ (its scripts start bf1943.exe from there).
    $bin = if ($Flat) { $dest } else { Join-Path $dest "bin" }
    New-Item -ItemType Directory -Force $bin | Out-Null
    foreach ($f in $binFiles) { Copy-Item (Join-Path $build $f) $bin }
    Copy-Item (Join-Path $build "fonts") (Join-Path $bin "fonts") -Recurse  # our menus' font (Oswald, OFL)
    # Our shader list (data\shaders: where each shader is in the player's game files, and the pipelines' render
    # state; no shader code), read at the first start (bf\src\shader_prep.cpp). Missing from every package up to
    # 0.1.5, so players' first sessions built all pipelines during play. Not for a server (it draws nothing).
    if ($Shaders) {
        # The launcher's button glyphs (Xelu's and Kenney's CC0 prompts, glyphs\LICENSE.txt): the player's only.
        Copy-Item (Join-Path $build "glyphs") (Join-Path $bin "glyphs") -Recurse
        $list = Join-Path $build "data\shaders\bf1943-shaders.list"
        if (-not (Test-Path $list)) { throw "no shader list in the build: $list" }
        # The list and NativeXRenderer's translations and pipelines (players draw with Vulkan only); not the Direct3D
        # 12 pipeline file, a type the check below keeps out of packages.
        $shaderDir = Join-Path $bin "data\shaders"
        New-Item -ItemType Directory -Force $shaderDir | Out-Null
        Get-ChildItem (Join-Path $build "data\shaders") -File | Where-Object { $_.Extension -in ".list", ".nxr" } |
            Copy-Item -Destination $shaderDir
        # The TRG Studios logo (TekRantGaming's own studio logo, his card's ink only): the startup card made from it on the
        # player's own card paper (bf\src\shader_prep.cpp MakeTrgCard).
        $logo = Join-Path $build "data\startup\trg_studios.png"
        if (-not (Test-Path $logo)) { throw "no TRG Studios logo in the build: $logo" }
        New-Item -ItemType Directory -Force (Join-Path $bin "data\startup") | Out-Null
        Copy-Item $logo (Join-Path $bin "data\startup")
    }
    # The main backend players and servers use (bf_online_host): the official one, or this PC for tests.
    Set-Content (Join-Path $bin "bf1943.toml") -Encoding ascii -Value @"
# Battlefield 1943 Reborn: runtime settings. The menu keeps its own in user\bf1943-settings.txt.
bf_online_host = "$OnlineHost"
"@
}

# ---- the player download: the first start's setup asks for the player's package and installs it (game_menu.cpp)
# The name ends windows-x64 (the zip's name then ends windows-x64.zip): the asset the launcher's updater looks for in
# a GitHub release (bf\src\update.cpp).
$name = "BF1943-Reborn-$Version-windows-x64"
$c = Join-Path $out $name
New-Bin $c -Shaders -Flat
Copy-Licences $c
Set-Content (Join-Path $c "README.txt") -Encoding ascii -Value @"
Battlefield 1943 Reborn $Version (github.com/TekRantGaming/Battlefield-1943-Reborn)

A fan project: not made, endorsed or supported by Electronic Arts or DICE. No game files are included.

1. Start bf1943.exe (Battlefield 1943 Reborn). Your settings, log and crash reports go in the folder 'user'.
2. The first time, select your own Battlefield 1943 (Xbox 360) package: the single file in 5841097E\000D0000 from
   your console's storage, or a folder that holds it. It is installed into the folder 'game', once.
3. Quick Play joins the busiest server with room; the server browser lists every official and community server.
4. Flying with mouse and keyboard is not right yet: take a boat to get to land, or use a controller to fly.
5. Updates install themselves when the game starts (from GitHub, checked before anything is changed).

Settings: our menu (Settings). Crash and hang reports go to user\crashes. Licences: the folder 'licences'.

If Windows says "rexruntime.dll is either not designed to run on Windows or it contains an error" (Bad Image,
0xc0e90002), Windows 11's Smart App Control has not cleared this build's files yet. It is not a broken download.
"@

# ---- the server downloads (SERVERS-PLAN.md section 7): the community package for everyone (its servers only in the
# Community list; the main backend is the official one, $OnlineHost), and with -Official the private one for TekRantGaming
# (this PC runs the main backend; servers official with official.key, which is never in a package). The server
# manager (server\manager.py, TekRantGaming's new one of 13 Nov 2026: a page in the browser) installs the game once
# and runs any number of servers; both packages have the same manager and everything it does.
function New-ServerPackage([string]$sname, [bool]$official) {
    $s = Join-Path $out $sname
    New-Bin $s
    Copy-Licences $s
    $sv = Join-Path $s "server"
    New-Item -ItemType Directory -Force (Join-Path $sv "manager_ui") | Out-Null
    Copy-Item (Join-Path $root "server\plasma.py"), (Join-Path $root "server\gmhost.py"), (Join-Path $root "server\manager.py") $sv
    Copy-Item (Join-Path $root "server\manager_ui\index.html") (Join-Path $sv "manager_ui")
    Copy-Item (Join-Path $root "tools\server_package\open_firewall.ps1") $s
    # The package's own Python (tools\server_package\make_python_runtime.py): a host installs nothing. python.org's
    # files, signed by the Python Software Foundation; their licence goes with the others.
    & $Python -I (Join-Path $root "tools\server_package\make_python_runtime.py") (Join-Path $s "python") | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $s "python\python.exe"))) { throw "the server package's Python could not be made from $Python" }
    Move-Item (Join-Path $s "python\LICENSE.txt") (Join-Path $s "licences\Python.txt")
    Set-Content (Join-Path $s "version.txt") $Version -Encoding ascii
    [ordered]@{ version = 3; upnp = $true; backend = $OnlineHost
                servers = @([ordered]@{ name = "Battlefield 1943 Reborn"; description = ""; maps = @("wake", "guadalcanal", "iwo", "coral")
                                        round_end_delay = 45; listed = $true; port = 18300 }) } |
        ConvertTo-Json -Depth 5 | Set-Content (Join-Path $s "server.default.json") -Encoding ascii
    # package.json: the official package (TekRantGaming only), and a game with Night Ops (test builds): the window's Night Ops
    # box and the hosts' --night only then (server/manager.py package_flags).
    $pkg = [ordered]@{}
    if ($official) { $pkg.official = $true }
    if ($nightOps) { $pkg.night_ops = $true }
    if ($pkg.Count) { $pkg | ConvertTo-Json -Compress | Set-Content (Join-Path $s "package.json") -Encoding ascii }
    # "BF1943 Server.bat" opens the manager's page (and starts the manager in the background when it is not running:
    # it has no window of its own); a failure stays on screen. The other two are for a PC that runs unattended.
    Set-Content (Join-Path $s "BF1943 Server.bat") -Encoding ascii -Value "@echo off`r`n`"%~dp0python\python.exe`" `"%~dp0server\manager.py`" open`r`nif errorlevel 1 pause"
    Set-Content (Join-Path $s "Start servers (no window).bat") -Encoding ascii -Value "@echo off`r`n`"%~dp0python\python.exe`" `"%~dp0server\manager.py`" start`r`nif errorlevel 1 pause"
    Set-Content (Join-Path $s "Stop servers.bat") -Encoding ascii -Value "@echo off`r`n`"%~dp0python\python.exe`" `"%~dp0server\manager.py`" quit"
    Set-Content (Join-Path $s "Open firewall ports.bat") -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0open_firewall.ps1`""
    $readme = if ($official) { @"
Battlefield 1943 Reborn Official Servers $Version (TRG Studios only: never share this package)

The main backend players log in to, and the official servers (the Official list in the game's server browser).
Needs your own Battlefield 1943 (Xbox 360) package, and official.key. Nothing else to install.

1. Copy official.key next to this file (your own copy; it is never in a download).
2. Open 'BF1943 Server.bat': the server manager opens in your browser. Install the game from your package (Game
   files), add your servers (each its own name, description, maps, players and port), and Start all.
3. On a hosted server (Windows Server), run 'Open firewall ports.bat' once, and open the same ports in your host's
   own firewall: TCP 18241, 18246 and 18250 (the main backend), UDP 18300 and one more for each extra server.
The manager keeps running in the background and starts a server again if it stops; 'Stop servers.bat' ends it.
'Start servers (no window).bat' starts them with the saved settings (for a PC that starts them by itself).
Everything the community package has is here too: the region, messages to the players with a countdown, updates
(on your word only: the Updates page's button; official servers never update by themselves), the logs.
The main backend lets in only players on the newest release: it reads the game's release feed on GitHub every
five minutes, and an older game gets a pop-up with Update now. In server.json, "version_gate": false lets every
version in, and "required_version": "0.4.0" names the version by hand in place of the feed.
"@ } else { @"
Battlefield 1943 Reborn Server $Version (github.com/TekRantGaming/Battlefield-1943-Reborn)

Run your own Battlefield 1943 servers, listed in the game's server browser under Community.
Needs your own Battlefield 1943 (Xbox 360) package, installed once. Nothing else to install.

1. Open 'BF1943 Server.bat': the server manager opens in your browser (http://127.0.0.1:18280/).
2. Game files: install the game from your package.
3. Servers: each has its own name, description, maps, players and port. Start all.
4. Network: pick your region (players see it with its flag). 'LAN only' keeps your servers off the main list:
   players then join with Direct connect, by this PC's address.
5. Messages: tell everyone on a server about an update or a restart, with a countdown, or type your own.
The manager keeps running in the background and starts a server again if it stops; new releases install
themselves (Updates). 'Stop servers.bat' ends everything; 'Start servers (no window).bat' is for a PC that starts
its servers by itself.
Players reach your servers on UDP 18300, 18301 and on (one port each server): leave 'Open the ports' on behind a
home router, or open them yourself. On a hosted server run 'Open firewall ports.bat' once.
"@ }
    Set-Content (Join-Path $s "README.txt") -Encoding ascii -Value $readme
    return $s
}
$sname = "BF1943-Reborn-Server-$Version-windows"
$s = New-ServerPackage $sname $false
$serverDirs = @($s)
if ($Official) {
    $oname = "BF1943-Reborn-OfficialServers-$Version-windows"
    $serverDirs += New-ServerPackage $oname $true
}

# bf1943.exe's file details must be this release's (bf\src\bf1943_version.rc.in, BF_VERSION when building): an
# anonymous unsigned exe looks worse to Smart App Control and scanners (King Kong's lesson).
# Its version is the release's exactly (as BF_VERSION above: from 0.2.0 the program and the release carry one version).
$details = (Get-Item (Join-Path $c "bf1943.exe")).VersionInfo
if ($details.CompanyName -ne 'TekRantGaming' -or $details.FileVersion -ne $Version) {
    throw "bf1943.exe's file details are '$($details.CompanyName)' $($details.FileVersion), not TekRantGaming $Version (build with -DBF_VERSION=$Version)"
}

# The player's download is started from bf1943.exe only: no batch file or script in it.
$scripts = Get-ChildItem $c -Recurse -File | Where-Object { $_.Extension -in @('.bat', '.cmd', '.ps1', '.vbs', '.js') }
if ($scripts) { throw "scripts in the player download: $($scripts.FullName -join ', ')" }
if (-not (Test-Path (Join-Path $c "bf1943.exe"))) { throw "bf1943.exe is not at the top of the player download" }

# No game file may be in a package.
# No official key either, and nothing extracted from the game (pictures or sounds: our CC0 glyphs and the font only).
foreach ($dir in @($c) + $serverDirs) {
    $bad = Get-ChildItem $dir -Recurse -File | Where-Object { $_.Extension -in @('.xex', '.fbrb', '.zz', '.xpso', '.xmp', '.key', '.wav', '.res') -or
        $_.Length -gt 200MB -or ($_.Extension -eq '.png' -and $_.FullName -notlike '*\glyphs\*' -and
        $_.FullName -notlike '*\data\startup\trg_studios.png') }
    if ($bad) { throw "not allowed in a package: $($bad.FullName -join ', ')" }
}
# The updater's lists (bf\src\update_files.h): every file of the player download with its hash (bf-files.txt), and the
# files earlier downloads had that this one does not (bf-retired.txt). An installed update follows them exactly.
& (Join-Path $PSScriptRoot "make_manifest.ps1") -Root $c -Version $Version
Compress-Archive -Path $c -DestinationPath (Join-Path $out "$name.zip")
# The Linux game, one file: its name ENDS with linux-x86_64.AppImage (the updater's asset, bf\src\update.cpp) and has no
# version, the same in every release (see the top).
if ($AppImage) {
    if (-not (Test-Path $AppImage)) { throw "no AppImage at $AppImage" }
    Copy-Item $AppImage (Join-Path $out "BF1943-Reborn-linux-x86_64.AppImage")
}
foreach ($dir in $serverDirs) { Compress-Archive -Path $dir -DestinationPath "$dir.zip" }
# The other Linux downloads, as built (their names are the build scripts': none ends with the game AppImage's).
foreach ($f in $LinuxFiles) {
    if (-not (Test-Path $f)) { throw "no Linux file at $f" }
    if ((Split-Path -Leaf $f) -notmatch '\.(flatpak|AppImage|tar\.gz)$') { throw "not a Linux download: $f" }
    Copy-Item $f $out
}
Get-ChildItem $out -Filter *.zip | ForEach-Object { "{0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }

# For the Microsoft developer submission (microsoft.com/wdsi/filesubmission, "Software developer"), which clears new
# files for Smart App Control and Defender (King Kong's procedure; testers got "Bad Image 0xc0e90002" without it):
# our three unsigned files in a zip with the password "infected", and the hashes of everything for VirusTotal.
$sub = Join-Path $out "ms-submission"
New-Item -ItemType Directory -Force $sub | Out-Null
$ours = @("bf1943.exe", "rexruntime.dll", "rexgpu-bfnative.dll") | ForEach-Object { Join-Path $c $_ }
# A DLL whose hash changed since the last release has no reputation yet: it needs this submission before testers get
# it (Kong: put the released DLL back unless the code really changed). The exe changes every release.
$last = Get-ChildItem (Join-Path $root "runs\release") -Directory | Where-Object { $_.Name -ne $Version -and (Test-Path (Join-Path $_.FullName "SHA256SUMS.txt")) } |
    Sort-Object LastWriteTime | Select-Object -Last 1
if ($last) {
    $before = Get-Content (Join-Path $last.FullName "SHA256SUMS.txt")
    foreach ($dll in @("rexruntime.dll", "rexgpu-bfnative.dll")) {
        $h = (Get-FileHash (Join-Path $c $dll) -Algorithm SHA256).Hash.ToLower()
        if ($before -notcontains "$h  $dll") { "NEW $dll since $($last.Name) ($($h.Substring(0, 8))): submit it to Microsoft before testers get this build" }
        else { "$dll unchanged since $($last.Name) ($($h.Substring(0, 8)))" }
    }
}
$sevenZip = @((Get-Command 7z -ErrorAction SilentlyContinue).Source, "$env:ProgramFiles\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($sevenZip) {
    & $sevenZip a -tzip -pinfected (Join-Path $sub "BF1943-Reborn-$Version-binaries.zip") $ours | Out-Null
} else {
    Compress-Archive -Path $ours -DestinationPath (Join-Path $sub "BF1943-Reborn-$Version-binaries.zip")
}
# The release's sums: the public downloads only (the official server package is never published).
$sums = @(Get-ChildItem $out -Filter *.zip | Where-Object { $_.Name -notlike '*OfficialServers*' }) +
        @(Get-ChildItem $out -Filter *.AppImage) + @(Get-ChildItem $out -Filter *.flatpak) +
        @(Get-ChildItem $out -Filter *.tar.gz) + @($ours | ForEach-Object { Get-Item $_ })
$sums | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
    Set-Content (Join-Path $out "SHA256SUMS.txt") -Encoding ascii
"Microsoft submission: $(Join-Path $sub "BF1943-Reborn-$Version-binaries.zip") (password: infected)"
# Nothing goes out unless the release folder passes (King Kong's check): the zip equals its bf-files.txt, the asset
# names and SHA256SUMS.txt are right for the updaters, nothing stale. The official package (never published) is moved
# out of the release folder first.
$officialFiles = @(Get-ChildItem $out -File | Where-Object { $_.Name -like '*OfficialServers*' })  # not $official: -Official
if ($officialFiles) {
    $aside = Join-Path $root "runs\release-official\$Version"
    New-Item -ItemType Directory -Force $aside | Out-Null
    $officialFiles | Move-Item -Destination $aside -Force
    "official package moved to $aside"
}
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "check_package.ps1") -Release $out -Version $Version
if ($LASTEXITCODE -ne 0) { throw "the release folder failed its check (tools\check_package.ps1)" }
