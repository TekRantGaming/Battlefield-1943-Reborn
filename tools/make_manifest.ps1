# The updater's file lists (bf/src/update_files.h; King Kong's codex\make-manifest.ps1): bf-files.txt and
# bf-retired.txt in a player package's folder, beside bf1943.exe. Never touches anything else. Safe to run again.
#   powershell -File tools\make_manifest.ps1 -Root <package folder> -Version 0.2.0
# bf-retired.txt: every file an earlier player package shipped (the zips in runs\release\<version>\, each listed once
#   into runs\release-trees, so later runs and removed zips still count) that this one does not, with its size and
#   sha256 (one line per version of it), protected paths left out. The updater removes such a file only while it is
#   unchanged.
# bf-files.txt: every file under -Root but itself (bf-retired.txt included), with size and sha256, sorted, plus
#   "version <v>". Line format "<sha256> <size> <path>" ('/' separated, relative to the folder of bf1943.exe).
param([Parameter(Mandatory)][string]$Root,
      [Parameter(Mandatory)][string]$Version,
      [string]$Releases = (Join-Path (Split-Path -Parent $PSScriptRoot) "runs\release"),
      [string]$TreeCache = (Join-Path (Split-Path -Parent $PSScriptRoot) "runs\release-trees"))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Root = (Resolve-Path $Root).Path.TrimEnd('\')
$sha = [Security.Cryptography.SHA256]::Create()
function Hex([byte[]]$b) { -join ($b | ForEach-Object { $_.ToString('x2') }) }
# The same list as update_files.cpp's Protected: what is inside these folders is the player's.
$protFolders = 'game', 'user', 'cache', 'logs', 'crashes'
function IsProtected([string]$p) {
    $l = $p.ToLowerInvariant()
    $i = $l.IndexOf('/')
    return ($i -gt 0 -and $protFolders -contains $l.Substring(0, $i))
}

# The listing of one earlier player zip (cached: "<sha256> <size> <path>", paths under its one top folder).
function Listing([IO.FileInfo]$item) {
    New-Item -ItemType Directory $TreeCache -Force | Out-Null
    $label = ($item.Directory.Name + '_' + $item.BaseName) -replace '[^A-Za-z0-9._]', '_'
    $cache = Join-Path $TreeCache "$label-$($item.Length).txt"
    if (Test-Path $cache) { return }
    $lines = New-Object System.Collections.Generic.List[string]
    $z = [IO.Compression.ZipFile]::OpenRead($item.FullName)
    try {
        foreach ($e in $z.Entries) {
            $p = $e.FullName -replace '\\', '/'
            if ($p.EndsWith('/')) { continue }
            $i = $p.IndexOf('/'); if ($i -lt 0) { continue }
            $s = $e.Open(); try { $h = Hex $sha.ComputeHash($s) } finally { $s.Close() }
            $lines.Add("$h $($e.Length) $($p.Substring($i + 1))")
        }
    } finally { $z.Dispose() }
    $text = "# files of $($item.FullName) ($($item.Length) bytes), made by tools\make_manifest.ps1`n" + ($lines -join "`n") + "`n"
    [IO.File]::WriteAllText($cache, $text, (New-Object Text.UTF8Encoding($false)))
}

# This package's files.
$files = @(Get-ChildItem $Root -Recurse -File | ForEach-Object {
    [pscustomobject]@{ Rel = $_.FullName.Substring($Root.Length + 1).Replace('\', '/'); Full = $_.FullName; Size = $_.Length }
} | Where-Object { $_.Rel -ne 'bf-files.txt' -and $_.Rel -ne 'bf-retired.txt' })
$current = @{}
foreach ($f in $files) { $current[$f.Rel.ToLowerInvariant()] = $true }
foreach ($bad in $files | Where-Object { IsProtected $_.Rel }) { throw "a protected path is in the package: $($bad.Rel)" }

# bf-retired.txt, from every earlier player package of a release version (not test folders, not this version's).
$earlier = Get-ChildItem $Releases -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+' -and $_.Name -ne $Version } |
    ForEach-Object { Get-ChildItem $_.FullName -File -Filter '*windows-x64.zip' } | Where-Object { $_.Name -notmatch 'Server' }
foreach ($z in $earlier) { Listing $z }
$retired = [ordered]@{}
foreach ($list in Get-ChildItem $TreeCache -Filter '*.txt' -ErrorAction SilentlyContinue) {
    foreach ($line in [IO.File]::ReadAllLines($list.FullName)) {
        if ($line -match '^([0-9a-f]{64}) (\d+) (.+)$') {
            $p = $Matches[3]; $key = $p.ToLowerInvariant()
            if ($current.ContainsKey($key) -or (IsProtected $p) -or $key -eq 'bf-files.txt' -or $key -eq 'bf-retired.txt') { continue }
            $retired["$key|$($Matches[1])"] = "$($Matches[1]) $($Matches[2]) $p"
        }
    }
}
$nl = "`n"
$text = "# Battlefield 1943 Reborn: files earlier releases shipped that v$Version does not (sha256, size, path).$nl" +
        "# The updater removes one only while it is unchanged. Made by tools\make_manifest.ps1.$nl" +
        "version $Version$nl" + (($retired.Values | Sort-Object { ($_ -split ' ', 3)[2] }, { $_ }) -join $nl) + $nl
[IO.File]::WriteAllText("$Root\bf-retired.txt", $text, (New-Object Text.UTF8Encoding($false)))
"bf-retired.txt: $($retired.Count) entries ($(@($retired.Values | ForEach-Object { ($_ -split ' ', 3)[2].ToLowerInvariant() } | Sort-Object -Unique).Count) paths) from $(@($earlier).Count) earlier packages"

# bf-files.txt (bf-retired.txt is one of the files now).
$files += [pscustomobject]@{ Rel = 'bf-retired.txt'; Full = "$Root\bf-retired.txt"; Size = (Get-Item "$Root\bf-retired.txt").Length }
$lines = foreach ($f in $files | Sort-Object Rel -CaseSensitive) {
    $s = [IO.File]::OpenRead($f.Full); try { $h = Hex $sha.ComputeHash($s) } finally { $s.Close() }
    "$h $($f.Size) $($f.Rel)"
}
$text = "# Battlefield 1943 Reborn: the files of v$Version (sha256, size, path), every file of the release but this one.$nl" +
        "# The updater installs these and checks them on the first start. Made by tools\make_manifest.ps1.$nl" +
        "version $Version$nl" + ($lines -join $nl) + $nl
[IO.File]::WriteAllText("$Root\bf-files.txt", $text, (New-Object Text.UTF8Encoding($false)))
"bf-files.txt: $(@($lines).Count) files, version $Version"
