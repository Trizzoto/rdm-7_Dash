<#
.SYNOPSIS
    Install the hand-authored "Altezza" dashboard layout onto a live RDM-7 dash
    over its WiFi web API.

.DESCRIPTION
    Uploads, in order:
      1. the full-screen background image  cluster_bg.rdmimg  -> image name "cluster_bg"
      2. the two TTF fonts so font_manager resolves the layout's font strings:
           fonts/fugaz_one.ttf    -> family "Fugaz"   (layout text widget uses "Fugaz:64")
           fonts/manrope_bold.ttf -> family "Manrope"
      3. the layout JSON via POST /api/layout/save, which also activates it and
         triggers a screen reload on the device.

    Why these exact font NAMES:
      The device seeds its built-in fonts as families "Fugaz One" / "Manrope Bold"
      (boot_assets.c). The layout asks for "Fugaz:64", and font_manager_get()
      matches the family name with strcasecmp, so "Fugaz" != "Fugaz One" and the
      text would silently fall back to the tiny theme font. Uploading the TTF
      under the bare name "Fugaz" creates a family the layout can resolve.
      (These bare names are NOT protected, so the upload is accepted; the built-in
      "Fugaz One"/"Manrope Bold" families are left untouched.)

    The script is idempotent: it checks /api/image/list and /api/font/list and
    skips assets already present unless -Force is given. The layout POST always
    runs (it is the activation step) unless -SkipLayout is given.

.PARAMETER Ip
    The dash IP address or hostname (e.g. 192.168.4.1 in hotspot mode, or the
    STA-mode address). Required.

.PARAMETER AssetDir
    Directory containing layout.json + img/ + fonts/. Defaults to the authored
    source at ..\..\..\rdm7-wasm-editor\altezza relative to this script.

.PARAMETER Force
    Re-upload the image/fonts even if the device already has them.

.PARAMETER SkipLayout
    Upload assets only; do not POST/activate the layout.

.EXAMPLE
    .\install_to_device.ps1 -Ip 192.168.4.1

.EXAMPLE
    .\install_to_device.ps1 -Ip 10.0.0.61 -Force
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Ip,

    [string]$AssetDir,

    [switch]$Force,

    [switch]$SkipLayout
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve asset locations
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $AssetDir -or $AssetDir -eq '') {
    # Authored source lives in the sibling rdm7-wasm-editor repo.
    $AssetDir = Join-Path $scriptDir '..\..\..\rdm7-wasm-editor\altezza'
}
$AssetDir = (Resolve-Path $AssetDir).Path

$layoutFile = Join-Path $AssetDir 'layout.json'
$imageFile  = Join-Path $AssetDir 'img\cluster_bg.rdmimg'
$fugazFile  = Join-Path $AssetDir 'fonts\fugaz_one.ttf'
$manropeFile = Join-Path $AssetDir 'fonts\manrope_bold.ttf'

# name on device  ->  source file  (font family names chosen to match layout)
$imageName = 'cluster_bg'
$fonts = @(
    @{ Name = 'Fugaz';   File = $fugazFile  },
    @{ Name = 'Manrope'; File = $manropeFile }
)

$base = "http://$Ip"

function Write-Step    ($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok      ($m) { Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Skip    ($m) { Write-Host "    [skip] $m" -ForegroundColor DarkGray }
function Write-Warn2   ($m) { Write-Host "    [warn] $m" -ForegroundColor Yellow }

# Verify all source files exist up front.
Write-Step "Checking source assets in $AssetDir"
$missing = @()
foreach ($f in @($layoutFile, $imageFile, $fugazFile, $manropeFile)) {
    if (-not (Test-Path $f)) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Write-Host "Missing source file(s):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw "Aborting: required asset(s) not found."
}
$imgSizeKB = [math]::Round((Get-Item $imageFile).Length / 1KB, 1)
Write-Ok "layout.json, cluster_bg.rdmimg ($imgSizeKB KB), fugaz_one.ttf, manrope_bold.ttf present"

# ---------------------------------------------------------------------------
# Reachability check
# ---------------------------------------------------------------------------
Write-Step "Probing device at $base"
try {
    $ver = Invoke-RestMethod -Uri "$base/api/layout/version" -Method Get -TimeoutSec 10
    Write-Ok "device reachable (layout version $($ver.v))"
} catch {
    throw "Cannot reach device at $base/api/layout/version : $($_.Exception.Message)"
}

# ---------------------------------------------------------------------------
# 1. Upload background image  (POST /api/image/upload?name=cluster_bg, raw body)
# ---------------------------------------------------------------------------
Write-Step "Image: $imageName"
$haveImage = $false
if (-not $Force) {
    try {
        $imgList = Invoke-RestMethod -Uri "$base/api/image/list" -Method Get -TimeoutSec 15
        $haveImage = @($imgList | Where-Object { $_.name -eq $imageName }).Count -gt 0
    } catch { Write-Warn2 "image/list failed ($($_.Exception.Message)); will upload anyway" }
}
if ($haveImage -and -not $Force) {
    Write-Skip "'$imageName' already on device (use -Force to re-upload)"
} else {
    $uri = "$base/api/image/upload?name=$imageName"
    $resp = Invoke-RestMethod -Uri $uri -Method Post -InFile $imageFile `
                -ContentType 'application/octet-stream' -TimeoutSec 60
    Write-Ok "uploaded cluster_bg.rdmimg -> '$imageName' (status: $($resp.status))"
}

# ---------------------------------------------------------------------------
# 2. Upload fonts  (POST /api/font/upload?name=<Family>, raw TTF body)
# ---------------------------------------------------------------------------
Write-Step "Fonts"
$haveFonts = @()
if (-not $Force) {
    try {
        # /api/font/list returns a flat array of family-name strings by default.
        $haveFonts = @(Invoke-RestMethod -Uri "$base/api/font/list" -Method Get -TimeoutSec 15)
    } catch { Write-Warn2 "font/list failed ($($_.Exception.Message)); will upload anyway" }
}
foreach ($font in $fonts) {
    $fname = $font.Name
    if (-not $Force -and ($haveFonts -contains $fname)) {
        Write-Skip "font family '$fname' already present (use -Force to re-upload)"
        continue
    }
    $uri = "$base/api/font/upload?name=$fname"
    $resp = Invoke-RestMethod -Uri $uri -Method Post -InFile $font.File `
                -ContentType 'application/octet-stream' -TimeoutSec 60
    Write-Ok "uploaded $(Split-Path -Leaf $font.File) -> family '$fname' (status: $($resp.status))"
}

# ---------------------------------------------------------------------------
# 3. Save + activate the layout  (POST /api/layout/save, body = layout JSON)
# ---------------------------------------------------------------------------
if ($SkipLayout) {
    Write-Step "Layout: skipped (-SkipLayout)"
} else {
    Write-Step "Layout: save + activate"
    # Read the layout as raw bytes so the JSON is sent verbatim (no PowerShell
    # re-encoding / BOM). The device parses + validates it, writes it atomically
    # to LittleFS, sets it active in NVS, and schedules a screen reload.
    $jsonBytes = [System.IO.File]::ReadAllBytes($layoutFile)
    $resp = Invoke-RestMethod -Uri "$base/api/layout/save" -Method Post `
                -Body $jsonBytes -ContentType 'application/json' -TimeoutSec 30
    Write-Ok "layout saved + activated (name: '$($resp.name)', status: $($resp.status))"
    Write-Host "    The dash will reload its screen within ~1 second." -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "Done. Altezza cluster installed to $base" -ForegroundColor Green
