# Decompile functions of ghidra\BF1943 to a text file for reading (the King Kong decomp's scripts\decompile.ps1,
# with this project's paths).
#   tools\decompile.ps1 -Out build\dc\online.c 82455BB8 tree:8249A908:2
param([Parameter(Mandatory)][string]$Out, [Parameter(ValueFromRemainingArguments)][string[]]$Addrs)
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = "E:\kk-decomp\tools\jdk-21.0.12.1+1"
New-Item -ItemType Directory -Force (Split-Path -Parent (Join-Path $root $Out)) | Out-Null
& "E:\kk-decomp\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" (Join-Path $root "ghidra") BF1943 `
    -process default.xex -noanalysis -readOnly -scriptPath (Join-Path $root "tools\ghidra") `
    -postScript Decompile.java (Join-Path $root $Out) @Addrs 2>&1 | Select-String "Decompile.java>|ERROR"
