# install_mirror.ps1 - one-time install of the WASM live-mirror engine onto
# the dash's LittleFS. After this, http://<dash-ip>/mirror works fully
# offline (hotspot mode included) and survives firmware OTA updates.
#
# Artifacts come from the rdm7-wasm-editor build (../rdm7-wasm-editor/build
# relative to this firmware repo by default). Re-run after rebuilding the
# WASM to update the installed engine.
#
# NOTE: keep this file pure ASCII - PowerShell 5.1 reads BOM-less files as
# ANSI and non-ASCII punctuation corrupts the parse.
#
# Usage:  powershell -File tools\install_mirror.ps1 -DeviceIp 192.168.4.61
param(
    [string]$DeviceIp = "192.168.4.61",
    [string]$WasmBuildDir = (Join-Path $PSScriptRoot "..\..\rdm7-wasm-editor\build")
)

$ErrorActionPreference = "Stop"
$base = "http://$DeviceIp"

$jsSrc = Join-Path $WasmBuildDir "index.js"
$wasmSrc = Join-Path $WasmBuildDir "index.wasm"
if (-not (Test-Path $jsSrc) -or -not (Test-Path $wasmSrc)) {
    Write-Error "WASM build artifacts not found in $WasmBuildDir - build rdm7-wasm-editor first (emcmake cmake, then emmake make)."
}

function GzipBytes([string]$path) {
    $raw = [IO.File]::ReadAllBytes($path)
    $ms = New-Object IO.MemoryStream
    $gz = New-Object IO.Compression.GZipStream($ms, [IO.Compression.CompressionLevel]::Optimal, $true)
    $gz.Write($raw, 0, $raw.Length)
    $gz.Dispose()
    return $ms.ToArray()
}

function UploadAsset([string]$name, [byte[]]$bytes) {
    $kb = [math]::Round($bytes.Length / 1024)
    Write-Host "uploading $name ($kb KB)..."
    $resp = Invoke-WebRequest -Uri "$base/api/mirror/upload?name=$name" -Method POST `
        -Body $bytes -ContentType "application/octet-stream" -TimeoutSec 120 -UseBasicParsing
    if ($resp.StatusCode -ne 200) { throw "upload of $name failed: $($resp.StatusCode)" }
}

UploadAsset "index.js.gz"   (GzipBytes $jsSrc)
UploadAsset "index.wasm.gz" (GzipBytes $wasmSrc)

$status = (Invoke-WebRequest -Uri "$base/api/mirror/status" -TimeoutSec 10 -UseBasicParsing).Content | ConvertFrom-Json
if (-not $status.installed) { throw "device reports mirror NOT installed after upload" }
$jsKb = [math]::Round($status.js_bytes / 1024)
$wasmKb = [math]::Round($status.wasm_bytes / 1024)
Write-Host "installed: js=$jsKb KB wasm=$wasmKb KB"
Write-Host "Live mirror ready: $base/mirror"
