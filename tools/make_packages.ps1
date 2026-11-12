# The release downloads (Battlefield 1943 Reborn, github.com/TekRantGaming/Battlefield-1943-Reborn, public):
#   runs\release\<version>\BF1943-Reborn-<version>-windows-x64.zip     the game for players
#   runs\release\<version>\BF1943-Reborn-Server-<version>-windows.zip  the community server package (any number of servers)
#   runs\release\<version>\BF1943-Reborn-linux-x86_64.AppImage  with -AppImage: the Linux game (tools\linux), its name the
#   same in every release (King Kong's rule since its v2.0.0): a new download keeps the name of the one a Steam shortcut
#   starts, and the updater writes each version over the file a player has, whatever its name
#   runs\release\<version>\BF1943-Reborn-OfficialServers-<version>-windows.zip  with -Official: TekRantGaming's private package
#   (the main backend and the official servers; never published, never in the release's sums)
# from the release build (bf\out\build\bf-relwithdebinfo), with the licences of everything in them. No game file goes
# in: players and hosts bring their own copy, installed once by the first start's setup (or the server's window). Nothing is uploaded: the release itself is made by hand on GitHub (reborn\docs\RELEASES.md).
# Usage: powershell -File tools\make_packages.ps1 -Version 0.1.0 -OnlineHost <address> (the main backend: the official one, or 127.0.0.1 for local tests)
param([Parameter(Mandatory = $true)][string]$Version, [string]$Preset = "bf-relwithdebinfo",
      [Parameter(Mandatory = $true)][string]$OnlineHost,  # the main backend (the official servers' address or name; no default: 0.1.5 and 0.1.6 went out pointing at 127.0.0.1)
      [switch]$Official,  # also the private official server package (TekRantGaming only; never uploaded)
      [string]$AppImage = "")  # the Linux AppImage this version built (WSL, tools\linux\build_appimage.sh): a release file
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
New-Item -ItemType Directory -Force $out | Out-Null

# Only our binaries: the game exe, its runtime and renderer, and the two vendor libraries it loads. Not the Xenos
# plugin (development only). The player download also gets data\shaders, our shader list (New-Bin -Shaders).
$binFiles = @("bf1943.exe", "rexruntime.dll", "rexgpu-bfnative.dll", "nvngx_dlss.dll", "amd_fidelityfx_vk.dll")

function Copy-Licences([string]$dest) {
    $l = Join-Path $dest "licences"
    New-Item -ItemType Directory -Force $l | Out-Null
    Copy-Item (Join-Path $nxr "LICENSE") (Join-Path $l "NativeXRenderer.txt")
    Copy-Item (Join-Path $nxr "THIRD_PARTY_NOTICES.md") (Join-Path $l "NativeXRenderer third party notices.md")
    foreach ($p in @(@("diligent-core\License.txt", "DiligentCore.txt"), @("diligent-core\ThirdParty\SPIRV-Cross\LICENSE", "SPIRV-Cross.txt"),
                     @("diligent-core\ThirdParty\volk\LICENSE.md", "volk.txt"), @("fsr1\LICENSE.txt", "AMD FSR 1.txt"),
                     @("cas\LICENSE.txt", "AMD CAS.txt"), @("ffx_api\LICENSE.txt", "AMD FidelityFX SDK.txt"),
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
3. Quick Match or the server browser: first pick Classic or Modern movement (every time; the screen says what
   each one changes), then the busiest server that plays it, or one from the list. You play what the server plays.
4. Flying with mouse and keyboard is not right yet: take a boat to get to land, or use a controller to fly.
5. Updates install themselves when the game starts (from GitHub, checked before anything is changed).

Settings: our menu (Settings). Crash and hang reports go to user\crashes. Licences: the folder 'licences'.

If Windows says "rexruntime.dll is either not designed to run on Windows or it contains an error" (Bad Image,
0xc0e90002), Windows 11's Smart App Control has not cleared this build's files yet. It is not a broken download.
"@

# ---- the server downloads (SERVERS-PLAN.md section 7): the community package for everyone (its servers only in the
# Community list; the main backend is the official one, $OnlineHost), and with -Official the private one for TekRantGaming
# (this PC runs the main backend; servers official with official.key, which is never in a package). The window
# (server_ui.ps1) installs the game once and runs any number of servers, each with its own name, maps and settings.
function New-ServerPackage([string]$sname, [bool]$official) {
    $s = Join-Path $out $sname
    New-Bin $s
    Copy-Licences $s
    $sv = Join-Path $s "server"
    New-Item -ItemType Directory -Force $sv | Out-Null
    Copy-Item (Join-Path $root "server\plasma.py"), (Join-Path $root "server\gmhost.py") $sv
    Copy-Item (Join-Path $root "tools\server_package\*.ps1") $s
    [ordered]@{ version = 2; upnp = $true; backend = $OnlineHost
                servers = @([ordered]@{ name = "Battlefield 1943 Reborn"; description = ""; maps = @("wake", "guadalcanal", "iwo", "coral")
                                        movement = "classic"; round_end_delay = 45; listed = $true }) } |
        ConvertTo-Json -Depth 5 | Set-Content (Join-Path $s "server.default.json") -Encoding ascii
    # package.json: the official package (TekRantGaming only), and a game with Night Ops (test builds): the window's Night Ops
    # box and the hosts' --night only then (server_config.ps1).
    $pkg = [ordered]@{}
    if ($official) { $pkg.official = $true }
    if ($nightOps) { $pkg.night_ops = $true }
    if ($pkg.Count) { $pkg | ConvertTo-Json -Compress | Set-Content (Join-Path $s "package.json") -Encoding ascii }
    Set-Content (Join-Path $s "BF1943 Server.bat") -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"%~dp0server_ui.ps1`""
    Set-Content (Join-Path $s "Start servers (no window).bat") -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0start_servers.ps1`""
    Set-Content (Join-Path $s "Stop servers.bat") -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0stop_servers.ps1`""
    Set-Content (Join-Path $s "Open firewall ports.bat") -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0open_firewall.ps1`""
    $readme = if ($official) { @"
Battlefield 1943 Reborn Official Servers $Version (TRG Studios only: never share this package)

The main backend players log in to, and the official servers (the Official list in the game's server browser).
Needs Python 3.12 (python.org), your own Battlefield 1943 (Xbox 360) package, and official.key.

1. Copy official.key next to this file (your own copy; it is never in a download).
2. Open 'BF1943 Server.bat': install the game from your package, add your servers (each its own name, description,
   maps, movement and pause between rounds), and Start all.
3. On a hosted server (Windows Server), run 'Open firewall ports.bat' once, and open the same ports in your host's
   own firewall: TCP 18241, 18246 and 18250 (the main backend), UDP 18300 and one more for each extra server.
'Start servers (no window).bat' starts them with the saved settings (for a PC that starts them by itself).
"@ } else { @"
Battlefield 1943 Reborn Server $Version (github.com/TekRantGaming/Battlefield-1943-Reborn)

Run your own Battlefield 1943 servers, listed in the game's server browser under Community.
Needs Python 3.12 (python.org) and your own Battlefield 1943 (Xbox 360) package, installed once.

1. Open 'BF1943 Server.bat': install the game from your package.
2. Add your servers: each has its own name, description, maps, movement and pause between rounds.
3. 'Show in the server browser' lists it; nothing to type, the game finds where your server is. Untick it for a
   private server: players then join it by its address (Direct connect in the game's server browser).
4. Start all. They run in the background with no window; Stop all (or Stop servers.bat) ends them.
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
        @(Get-ChildItem $out -Filter *.AppImage) + @($ours | ForEach-Object { Get-Item $_ })
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
