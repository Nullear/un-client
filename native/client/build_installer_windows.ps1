param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$package = Join-Path $ProjectRoot 'build\windows-encrypted'
$launcher = Join-Path $package 'UnFalsusOnline.exe'
$payload = Join-Path $package 'client.bin'
$sourceIcon = Join-Path $ProjectRoot 'icon.png'
$script = Join-Path $PSScriptRoot 'installer_windows.iss'
$compiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
foreach ($path in @($launcher, $payload, $sourceIcon, $script, $compiler)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required installer input is missing: $path" }
}

$temp = Join-Path $env:TEMP ('un-client-installer-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $iconPath = Join-Path $temp 'client.ico'
    $source = [System.Drawing.Image]::FromFile($sourceIcon)
    try {
        $frames = @()
        foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
            $bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($source, 0, 0, $size, $size)
            $graphics.Dispose()
            $frame = New-Object System.IO.MemoryStream
            $bitmap.Save($frame, [System.Drawing.Imaging.ImageFormat]::Png)
            $bitmap.Dispose()
            $frames += ,$frame.ToArray()
            $frame.Dispose()
        }

        $stream = [System.IO.File]::Create($iconPath)
        $writer = New-Object System.IO.BinaryWriter($stream)
        $writer.Write([UInt16]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]$frames.Count)
        $offset = 6 + 16 * $frames.Count
        for ($i = 0; $i -lt $frames.Count; $i++) {
            $size = @(16, 24, 32, 48, 64, 128, 256)[$i]
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]32)
            $writer.Write([UInt32]$frames[$i].Length)
            $writer.Write([UInt32]$offset)
            $offset += $frames[$i].Length
        }
        foreach ($frame in $frames) { $writer.Write($frame) }
        $writer.Dispose()
        $stream.Dispose()
    } finally {
        $source.Dispose()
    }

    & $compiler "/DSetupIconPath=$iconPath" $script
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
} finally {
    [System.IO.Directory]::Delete($temp, $true)
}

$output = Join-Path $ProjectRoot 'build\Un-Client-Setup.exe'
"Un Client installer created: $output"
