# 内存诊断：编译到临时目录，用正在用的便签的副本，分别以软件渲染(sw)和显卡渲染(hw)启动，
# 依次做常见操作并记录每一步的内存（见 src\Diag.cs）。不碰正在用的数据。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tests\memdiag.ps1
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP ("desknote-mem-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$exe = Join-Path $work 'DeskNote.exe'
cmd /c "`"$root\build.cmd`" `"$exe`"" | Out-Host
if (-not (Test-Path $exe)) { Write-Host 'BUILD FAILED'; exit 1 }
foreach ($mode in 'sw', 'hw') {
    $data = Join-Path $work "data-$mode"
    New-Item -ItemType Directory -Force (Join-Path $data 'notes') | Out-Null
    Copy-Item (Join-Path $env:APPDATA 'DeskNote\notes\*.json') (Join-Path $data 'notes') -ErrorAction SilentlyContinue
    $out = Join-Path $work "diag-$mode.txt"
    $env:DESKNOTE_MEMDIAG = $out
    $env:DESKNOTE_RENDER = $mode
    Start-Process $exe -ArgumentList '--data', "`"$data`"" -Wait
    Write-Host "==== $mode"
    Get-Content $out -Encoding UTF8 | Write-Host
}
Remove-Item Env:DESKNOTE_MEMDIAG, Env:DESKNOTE_RENDER
Remove-Item -Recurse -Force $work
