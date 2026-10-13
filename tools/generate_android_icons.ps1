param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\android-icons'),
    [string]$Ffmpeg = 'C:\ffmpeg\bin\ffmpeg.exe'
)

$ErrorActionPreference = 'Stop'

$densitySizes = @(
    @{ Name = 'mdpi'; Legacy = 48; Adaptive = 108 },
    @{ Name = 'hdpi'; Legacy = 72; Adaptive = 162 },
    @{ Name = 'xhdpi'; Legacy = 96; Adaptive = 216 },
    @{ Name = 'xxhdpi'; Legacy = 144; Adaptive = 324 },
    @{ Name = 'xxxhdpi'; Legacy = 192; Adaptive = 432 }
)

$sources = @{
    Legacy = Join-Path $ProjectRoot 'icon.png'
    Foreground = Join-Path $ProjectRoot 'icon_foreground.png'
    Background = Join-Path $ProjectRoot 'icon_background.png'
}

if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) {
    throw "ffmpeg not found: $Ffmpeg"
}

foreach ($source in $sources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $source.Value -PathType Leaf)) {
        throw "Icon source not found: $($source.Value)"
    }
}

foreach ($density in $densitySizes) {
    $directory = Join-Path $OutputRoot "mipmap-$($density.Name)"
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    $outputs = @(
        @{ Source = $sources.Legacy; Name = 'ic_launcher.png'; Size = $density.Legacy },
        @{ Source = $sources.Foreground; Name = 'ic_launcher_foreground.png'; Size = $density.Adaptive },
        @{ Source = $sources.Background; Name = 'ic_launcher_background.png'; Size = $density.Adaptive }
    )

    foreach ($item in $outputs) {
        $destination = Join-Path $directory $item.Name
        & $Ffmpeg -hide_banner -loglevel error -y -i $item.Source `
            -vf "scale=$($item.Size):$($item.Size):flags=lanczos" -frames:v 1 $destination
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to generate $destination"
        }
    }
}

Write-Output "Generated launcher icon density variants in $OutputRoot"
