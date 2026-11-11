# Build the Ghidra project ghidra\BF1943 from scratch: default.xex through XEXLoaderWV, seeded with the recompiler's
# function list by SeedBF1943.java, then full auto analysis. Same settings as the King Kong decomp's
# build_ghidra_project.ps1 (TekRantGaming approved the full run on 4 Nov 2026). Run tools\make_seeds.py first.
# Logs: build\logs\ghidra-full.log and ghidra-full-scripts.log.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = "E:\kk-decomp\tools\jdk-21.0.12.1+1"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$headless = "E:\kk-decomp\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat"
$logs = Join-Path $root "build\logs"
New-Item -ItemType Directory -Force (Join-Path $root "ghidra"), $logs | Out-Null

& $headless (Join-Path $root "ghidra") BF1943 -import (Join-Path $root "input\pkg\default.xex") -overwrite `
    -scriptPath (Join-Path $root "tools\ghidra") -preScript SeedBF1943.java (Join-Path $root "build\seeds") `
    -analysisTimeoutPerFile 14400 -max-cpu 8 `
    -log (Join-Path $logs "ghidra-full.log") -scriptlog (Join-Path $logs "ghidra-full-scripts.log")
if ($LASTEXITCODE -ne 0) { throw "Ghidra import and analysis failed ($LASTEXITCODE)" }
"done"
