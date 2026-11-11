# Checks a release folder before it is published (King Kong's codex\check-package.ps1) and FAILS (exit 1) on any
# problem. Reads only; changes nothing.
#   powershell -File tools\check_package.ps1 -Release runs\release\0.2.0 -Version 0.2.0
# 1. The player zip's tree equals its bf-files.txt exactly (names, sizes, sha256), bf-files.txt itself aside; every
#    path safe; the version is -Version.
# 2. The assets (every file in the folder): only the player zip's name contains "windows-x64.zip" and only the
#    AppImage's "linux-x86_64.AppImage" (an updater takes the name that ends so; old ones took any containing it);
#    SHA256SUMS.txt has a matching line for each of them.
# 3. Required files: the program, its runtime and renderer, the backend setting, the shader list, the fonts and
#    glyphs, bf-retired.txt.
# 4. No stale files (*.pdb, *.map, *.old, *.new, the Xenos plugin); bf-retired.txt names no file the release ships.
param([Parameter(Mandatory)][string]$Release, [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$problems = New-Object System.Collections.Generic.List[string]
function Bad([string]$m) { $problems.Add($m); Write-Host "FAIL: $m" }
$sha = [Security.Cryptography.SHA256]::Create()
function Hex([byte[]]$b) { -join ($b | ForEach-Object { $_.ToString('x2') }) }
function SafePath([string]$p) {
    if (!$p -or $p.Length -gt 1024 -or $p.StartsWith('/') -or $p -match '[\\:\x00-\x1f]') { return $false }
    foreach ($part in $p.Split('/')) { if ($part -eq '' -or $part -eq '.' -or $part -eq '..') { return $false } }
    return $true
}
function ParseList([string]$text, [string]$name) {
    $r = @{ Version = ''; Files = [ordered]@{} }
    $n = 0
    foreach ($line in $text -split "`n") {
        $n++; $line = $line.TrimEnd("`r")
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        if ($line.StartsWith('version ')) { $r.Version = $line.Substring(8); continue }
        if ($line -notmatch '^([0-9a-f]{64}) (\d+) (.+)$') { Bad "$name line ${n}: broken: $line"; continue }
        if (!(SafePath $Matches[3])) { Bad "$name line ${n}: unsafe path $($Matches[3])" }
        $r.Files[$Matches[3]] = @{ Sha = $Matches[1]; Size = [int64]$Matches[2] }
    }
    return $r
}

$Release = (Resolve-Path $Release).Path
$zips = @(Get-ChildItem $Release -File -Filter '*-windows-x64.zip')
if ($zips.Count -ne 1) { Bad "expected one player zip (*-windows-x64.zip) in $Release, found $($zips.Count)" }
$zipItem = $zips | Select-Object -First 1
$tree = [ordered]@{}
$listText = $null; $retiredText = $null
if ($zipItem) {
    $z = [IO.Compression.ZipFile]::OpenRead($zipItem.FullName)
    try {
        foreach ($e in $z.Entries) {
            $p = $e.FullName -replace '\\', '/'
            if ($p.EndsWith('/')) { continue }
            $i = $p.IndexOf('/')
            if ($i -lt 0) { Bad "entry outside the zip's folder: $p"; continue }
            $rel = $p.Substring($i + 1)
            $s = $e.Open(); $m = New-Object IO.MemoryStream; $s.CopyTo($m); $s.Close(); $m.Position = 0
            $tree[$rel] = @{ Sha = (Hex $sha.ComputeHash($m)); Size = $e.Length }
            if ($rel -eq 'bf-files.txt') { $listText = [Text.Encoding]::UTF8.GetString($m.ToArray()) }
            if ($rel -eq 'bf-retired.txt') { $retiredText = [Text.Encoding]::UTF8.GetString($m.ToArray()) }
        }
    } finally { $z.Dispose() }
}

# 1. tree == bf-files.txt
if ($null -eq $listText) { Bad 'bf-files.txt missing beside bf1943.exe in the zip' } else {
    $list = ParseList $listText 'bf-files.txt'
    if ($list.Version -ne $Version) { Bad "bf-files.txt version '$($list.Version)', expected '$Version'" }
    foreach ($p in $tree.Keys) {
        if ($p -eq 'bf-files.txt') { continue }
        if (!$list.Files.Contains($p)) { Bad "in the zip but not in bf-files.txt: $p"; continue }
        $want = $list.Files[$p]
        if ($want.Size -ne $tree[$p].Size -or $want.Sha -ne $tree[$p].Sha) { Bad "size or sha256 differs from bf-files.txt: $p" }
    }
    foreach ($p in $list.Files.Keys) { if (!$tree.Contains($p)) { Bad "in bf-files.txt but not in the zip: $p" } }
    if ($list.Files.Contains('bf-files.txt')) { Bad 'bf-files.txt lists itself' }
    "bf-files.txt: $($list.Files.Count) files, version $($list.Version); zip: $($tree.Count) files"
}

# 2. assets
$assets = Get-ChildItem $Release -File
$appimages = @($assets | Where-Object { $_.Name -like '*linux-x86_64.AppImage*' })
foreach ($a in $assets) {
    if ($a.Name -like '*windows-x64.zip*' -and $zipItem -and $a.Name -ne $zipItem.Name) { Bad "asset name contains windows-x64.zip: $($a.Name)" }
    if ($a.Name -like '*linux-x86_64.AppImage*' -and -not $a.Name.EndsWith('linux-x86_64.AppImage')) { Bad "asset name contains linux-x86_64.AppImage: $($a.Name)" }
}
if ($appimages.Count -gt 1) { Bad "more than one AppImage: $(($appimages | ForEach-Object Name) -join ', ')" }
# The AppImage's name is the same in every release (no version: a Steam shortcut keeps starting the file of that name).
foreach ($a in $appimages) { if ($a.Name -ne 'BF1943-Reborn-linux-x86_64.AppImage') { Bad "the AppImage must be named BF1943-Reborn-linux-x86_64.AppImage, not $($a.Name)" } }
$sums = Join-Path $Release 'SHA256SUMS.txt'
if (!(Test-Path $sums)) { Bad 'SHA256SUMS.txt missing' } else {
    $sumLines = Get-Content $sums
    foreach ($a in @($zipItem) + $appimages) {
        if (!$a) { continue }
        $h = (Get-FileHash $a.FullName -Algorithm SHA256).Hash.ToLower()
        $line = $sumLines | Where-Object { $_ -match "^([0-9a-f]{64})\s+\*?$([regex]::Escape($a.Name))$" } | Select-Object -First 1
        if (!$line) { Bad "SHA256SUMS.txt has no line for $($a.Name)" } elseif ($line.Substring(0, 64) -ne $h) { Bad "SHA256SUMS.txt does not match $($a.Name)" }
    }
}
foreach ($a in $appimages) {
    $b = [IO.File]::ReadAllBytes($a.FullName)
    if ($b.Length -lt 1MB -or $b[0] -ne 0x7f -or $b[1] -ne 0x45 -or $b[2] -ne 0x4c -or $b[3] -ne 0x46) { Bad "$($a.Name) is not a program (ELF)" }
}
"assets: $(($assets | ForEach-Object Name) -join ', ')"

# 3. required
foreach ($r in 'bf1943.exe', 'rexruntime.dll', 'rexgpu-bfnative.dll', 'bf1943.toml', 'data/shaders/bf1943-shaders.list',
               'data/startup/trg_studios.png', 'bf-retired.txt', 'README.txt') {
    if (!$tree.Contains($r)) { Bad "required file missing: $r" }
}
foreach ($prefix in 'fonts/', 'glyphs/', 'licences/') {
    if (!($tree.Keys | Where-Object { $_.StartsWith($prefix) })) { Bad "no files under $prefix" }
}

# 4. stale
foreach ($p in $tree.Keys) {
    $l = $p.ToLowerInvariant()
    if ($l -eq 'rexgpu-xenos.dll' -or $l -match '\.(map|pdb|old|new)$') { Bad "stale file in the zip: $p" }
}
if ($null -ne $retiredText) {
    $ret = ParseList $retiredText 'bf-retired.txt'
    foreach ($p in $ret.Files.Keys) { if ($tree.Keys | Where-Object { $_ -ieq $p }) { Bad "bf-retired.txt names a file the release ships: $p" } }
    "bf-retired.txt: $($ret.Files.Count) paths"
}

if ($problems.Count) { Write-Host "PACKAGE CHECK FAILED: $($problems.Count) problems"; exit 1 }
'PACKAGE CHECK PASSED'
