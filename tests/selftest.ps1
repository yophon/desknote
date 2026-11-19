# DeskNote 回归测试：编译到临时目录，用临时数据目录跑两轮自测（第二轮模拟重启），同步部分连本地假 gopan。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tests\selftest.ps1
# 不碰正在用的便签数据（自测程序拒绝在默认数据目录里运行）。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP ("desknote-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$exe = Join-Path $work 'DeskNote.exe'
$data = Join-Path $work 'data'

cmd /c "`"$root\build.cmd`" `"$exe`"" | Out-Host
if (-not (Test-Path $exe)) { Write-Host 'BUILD FAILED'; exit 1 }

$port = Get-Random -Minimum 20000 -Maximum 40000
$key = 'test-' + [guid]::NewGuid().ToString('N')
$py = (Get-Command python -ErrorAction SilentlyContinue)
$mock = $null
$syncArgs = @()
if ($py) {
    $mock = Start-Process $py.Source -ArgumentList "-I", "`"$root\tests\mock_gopan.py`"", $port, $key -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $syncArgs = @('--sync-server', "http://127.0.0.1:$port", '--sync-key', $key)
} else { Write-Host 'SKIP 同步测试（没有 python）' }

$ok = $true
try {
    foreach ($phase in '1', '2') {
        $out = Join-Path $work "result$phase.txt"
        $argv = @('--data', "`"$data`"", '--selftest', $phase, "`"$out`"")
        if ($phase -eq '1') { $argv += $syncArgs }
        $p = Start-Process $exe -ArgumentList $argv -PassThru
        if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Host "FAIL 第 $phase 轮超时"; $ok = $false; break }
        if (-not (Test-Path $out)) { Write-Host "FAIL 第 $phase 轮没有结果文件"; $ok = $false; break }
        Write-Host "---- 第 $phase 轮 ----"
        $text = Get-Content $out -Encoding UTF8
        $text | Write-Host
        if (-not ($text -contains 'RESULT OK')) { $ok = $false }
    }
} finally {
    if ($mock) { Stop-Process $mock.Id -ErrorAction SilentlyContinue }
}
if ($ok) { Write-Host 'ALL PASSED'; Remove-Item -Recurse -Force $work } else { Write-Host "有失败，现场留在 $work" }
exit ([int](-not $ok))
