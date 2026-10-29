# SPDX-License-Identifier: GPL-2.0-or-later
# VocalDress の声のビルド用：インストールされている OBS の obs.dll から、リンク用の obs.lib を作る（VERIFY-04 の試作から写した）。
# 「x64 Native Tools Command Prompt for VS」（または Developer PowerShell）で動かす（dumpbin と lib を使う）。
#   powershell -ExecutionPolicy Bypass -File tools\make-obs-lib.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make-obs-lib.ps1 -ObsDll "D:\obs-studio\bin\64bit\obs.dll"
param(
    [string]$ObsDll = "C:\Program Files\obs-studio\bin\64bit\obs.dll",
    [string]$OutDir = "obs-lib"
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $ObsDll)) { throw "obs.dll が見つかりません: $ObsDll" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# 日本語の Visual Studio では dumpbin の見出しが日本語になり、表の始まりを見つけられない（2026-10-23、本人のPC）。
# 出力を英語にして読む（このスクリプトの中だけ。終わったら元に戻す）
$oldVsLang = $env:VSLANG
$env:VSLANG = "1033"
try { $dump = & dumpbin /exports $ObsDll } finally { $env:VSLANG = $oldVsLang }
if ($LASTEXITCODE -ne 0) { throw "dumpbin が失敗しました（終了コード $LASTEXITCODE）。Developer PowerShell for VS で動かしてください" }
$names = @()
$inTable = $false
foreach ($line in $dump) {
    if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if (-not $inTable) { continue }
    if ($line -match '^\s*Summary') { break }
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+(?:[0-9A-Fa-f]+\s+)?([A-Za-z_][A-Za-z0-9_@?$]*)') { $names += $Matches[1] }
}
if ($names.Count -lt 100) { throw "obs.dll から読めた関数が少なすぎます（$($names.Count) 個）" }
$def = Join-Path $OutDir "obs.def"
@("LIBRARY obs", "EXPORTS") + $names | Set-Content -Encoding ascii $def
$out = Join-Path $OutDir "obs.lib"
# "/out:" とパスは1つの引数で渡す（分けると lib が LNK1146 で失敗する）
& lib /nologo "/def:$def" /machine:x64 "/out:$out"
if ($LASTEXITCODE -ne 0) { throw "lib が失敗しました（終了コード $LASTEXITCODE）。obs.lib は作れていません" }
Write-Host "作りました: $out（関数 $($names.Count) 個）"
