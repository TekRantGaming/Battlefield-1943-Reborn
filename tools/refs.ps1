# References to addresses in ghidra\BF1943 (tools\ghidra\Refs.java), written to a text file.
#   tools\refs.ps1 -Out build\refs.txt 82AE2CE4 82020080
param([Parameter(Mandatory)][string]$Out, [Parameter(ValueFromRemainingArguments)][string[]]$Addrs)
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = "E:\kk-decomp\tools\jdk-21.0.12.1+1"
New-Item -ItemType Directory -Force (Split-Path -Parent (Join-Path $root $Out)) | Out-Null
& "E:\kk-decomp\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" (Join-Path $root "ghidra") BF1943 `
    -process default.xex -noanalysis -readOnly -scriptPath (Join-Path $root "tools\ghidra") `
    -postScript Refs.java (Join-Path $root $Out) @Addrs 2>&1 | Select-String "Refs.java>|ERROR"
