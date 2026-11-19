# 生成 DeskNote.ico：蓝灰色圆角便签 + 三行文字线，大尺寸带左侧圆点
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1 <输出ico路径> [预览png路径]
param([string]$out, [string]$preview)
Add-Type -AssemblyName System.Drawing

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Draw-Icon([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [single]([Math]::Max(1, [Math]::Round($s * 0.06)))       # 外边距
    $w = [single]($s - 2 * $m)
    $r = [single]([Math]::Max(2, $s * 0.16))                       # 圆角
    $card = New-RoundRect $m $m $w $w $r
    $top = [System.Drawing.Color]::FromArgb(255, 0x40, 0x51, 0x5E)
    $bot = [System.Drawing.Color]::FromArgb(255, 0x2B, 0x37, 0x40)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.RectangleF $m, $m, $w, $w), $top, $bot, 90
    $g.FillPath($grad, $card)

    # 三行文字线；>=32px 时左边加圆点（对应便签里的条目）
    $lineColor = [System.Drawing.Color]::FromArgb(255, 0xD5, 0xDC, 0xE1)
    $dotColor = [System.Drawing.Color]::FromArgb(255, 0x9A, 0xA6, 0xAE)
    $lh = [single]([Math]::Max(1.6, $s * 0.072))                    # 线粗
    $withDots = $s -ge 32
    $x0 = [single]($m + $w * ($(if ($withDots) { 0.34 } else { 0.22 })))
    $x1 = [single]($m + $w * 0.80)
    $ys = @(0.33, 0.52, 0.71)
    $lens = @(1.0, 0.78, 0.9)
    $pen = New-Object System.Drawing.Pen $lineColor, $lh
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $dotBrush = New-Object System.Drawing.SolidBrush $dotColor
    for ($i = 0; $i -lt 3; $i++) {
        $y = [single]($m + $w * $ys[$i])
        $xe = [single]($x0 + ($x1 - $x0) * $lens[$i])
        $g.DrawLine($pen, $x0, $y, $xe, $y)
        if ($withDots) {
            $dr = [single]($lh * 0.62)
            $cx = [single]($m + $w * 0.22)
            $g.FillEllipse($dotBrush, $cx - $dr, $y - $dr, 2 * $dr, 2 * $dr)
        }
    }
    $g.Dispose()
    return $bmp
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$pngs = @()

# 256 用 PNG；小尺寸用传统 DIB（32 位 BGRA 自下而上 + 1 位 AND 掩码），
# 因为 System.Drawing.Icon（托盘图标要用）读不了 PNG 格式的小尺寸帧
function To-Dib([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2))
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
    $w.Write([UInt32]0); $w.Write([Int32]0); $w.Write([Int32]0); $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($y = $s - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $s; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A)
        }
    }
    $rowBytes = [int]([Math]::Ceiling($s / 32.0) * 4)
    $w.Write((New-Object byte[] ($rowBytes * $s)))   # AND 掩码全 0，透明度由 alpha 决定
    $w.Flush()
    return , $ms.ToArray()
}

foreach ($s in $sizes) {
    $bmp = Draw-Icon $s
    if ($s -ge 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngs += , $ms.ToArray()
    } else {
        $pngs += , (To-Dib $bmp)
    }
    if ($preview -and $s -eq 256) { $bmp.Save($preview, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

# ICO 文件：头 + 目录 + 每个尺寸的图像数据
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
"wrote $out"
