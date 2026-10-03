param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing
$source = Join-Path $ProjectRoot 'build\windows'
$output = Join-Path $ProjectRoot 'build\windows-encrypted'
if (!(Test-Path -LiteralPath $source)) { throw 'Windows export directory is missing.' }
$files = @('UnFalsusOnline.exe', 'UnFalsusOnline.pck', 'libunfalsus.windows.template_release.x86_64.dll', 'libgozen.windows.template_release.x86_64.dll')
foreach ($name in $files) {
    if (!(Test-Path -LiteralPath (Join-Path $source $name))) { throw "Export file missing: $name" }
}
if (!(Test-Path -LiteralPath $output)) { New-Item -ItemType Directory -Path $output | Out-Null }
$memory = New-Object System.IO.MemoryStream
$zip = New-Object System.IO.Compression.ZipArchive($memory, [System.IO.Compression.ZipArchiveMode]::Create, $true)
foreach ($name in $files) {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $source $name), $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
}
$zip.Dispose()
$plain = $memory.ToArray()
$memory.Dispose()
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$key = New-Object byte[] 32
$macKey = New-Object byte[] 32
$rng.GetBytes($key)
$rng.GetBytes($macKey)
$rng.Dispose()
$aes = [System.Security.Cryptography.Aes]::Create()
$aes.Key = $key
$aes.GenerateIV()
$encryptor = $aes.CreateEncryptor()
$encrypted = $encryptor.TransformFinalBlock($plain, 0, $plain.Length)
$encryptor.Dispose()
[Array]::Clear($plain, 0, $plain.Length)
$body = New-Object byte[] ($aes.IV.Length + $encrypted.Length)
[Array]::Copy($aes.IV, 0, $body, 0, 16)
[Array]::Copy($encrypted, 0, $body, 16, $encrypted.Length)
$aes.Dispose()
$mac = New-Object System.Security.Cryptography.HMACSHA256(,$macKey)
$signature = $mac.ComputeHash($body)
$mac.Dispose()
$payload = Join-Path $output 'client.bin'
$stream = [System.IO.File]::Create($payload)
$stream.Write($body, 0, $body.Length)
$stream.Write($signature, 0, $signature.Length)
$stream.Dispose()
$launcher = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'secure_launcher.cs.in'))
$launcher = $launcher.Replace('@ENCRYPTION_KEY@', [Convert]::ToBase64String($key)).Replace('@MAC_KEY@', [Convert]::ToBase64String($macKey))
$temp = Join-Path $env:TEMP ('unfalsus-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $code = Join-Path $temp 'launcher.cs'
    $iconPath = Join-Path $temp 'icon.ico'
    [System.IO.File]::WriteAllText($code, $launcher)
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $source 'UnFalsusOnline.exe'))
    $iconStream = [System.IO.File]::Create($iconPath)
    $icon.Save($iconStream)
    $iconStream.Dispose()
    $icon.Dispose()
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:winexe /platform:x64 /optimize+ "/win32icon:$iconPath" "/out:$(Join-Path $output 'UnFalsusOnline.exe')" /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll /reference:System.Windows.Forms.dll $code
    if ($LASTEXITCODE -ne 0) { throw 'Secure launcher compilation failed.' }
} finally {
    [System.IO.Directory]::Delete($temp, $true)
    [Array]::Clear($key, 0, $key.Length)
    [Array]::Clear($macKey, 0, $macKey.Length)
}
"Encrypted Windows package: $output"
