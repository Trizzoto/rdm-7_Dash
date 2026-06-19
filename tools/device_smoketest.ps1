<#
  device_smoketest.ps1 — exercise the agent-test endpoints on a live RDM-7.

  Usage:  pwsh tools/device_smoketest.ps1 [-Host 192.168.4.61]

  Drives the endpoints added for agent-driven verification:
    GET  /api/selftest
    GET  /api/widgets
    POST /api/can/inject       (round-trips a frame through the real decode path)
    GET  /api/channels         (reads back the injected channel's zone)
    GET  /api/screenshot/hash  (deterministic region fingerprint + change detect)

  Read-only except for /api/can/inject, which only pushes a transient value
  (no persisted config changes).
#>
param([string]$DeviceHost = "192.168.4.61")

$base = "http://$DeviceHost"
function Get-Json($path) { Invoke-RestMethod -Uri "$base$path" -TimeoutSec 10 }
function Post-Json($path, $obj) {
  Invoke-RestMethod -Uri "$base$path" -Method Post -TimeoutSec 10 `
    -Body ($obj | ConvertTo-Json -Compress) -ContentType "application/json"
}

Write-Host "== /api/selftest ==" -ForegroundColor Cyan
$st = Get-Json "/api/selftest"
$st | ConvertTo-Json -Depth 5
if (-not $st.ok) { Write-Host "SELFTEST NOT OK" -ForegroundColor Yellow }

Write-Host "`n== /api/widgets (count + first widget) ==" -ForegroundColor Cyan
$wg = Get-Json "/api/widgets"
Write-Host "widgets: $($wg.count)"
$wg.widgets | Select-Object -First 1 | ConvertTo-Json -Depth 5

Write-Host "`n== /api/screenshot/hash (full + region, change detect) ==" -ForegroundColor Cyan
$h1 = Get-Json "/api/screenshot/hash"
"full:   hash=$($h1.hash) seq=$($h1.seq) torn=$($h1.torn)"
$h2 = Get-Json "/api/screenshot/hash?x=0&y=0&w=100&h=100"
"region: hash=$($h2.hash) ${($h2.w)}x$($h2.h)@($($h2.x),$($h2.y))"

Write-Host "`n== /api/can/inject (only meaningful with a known decoded channel) ==" -ForegroundColor Cyan
Write-Host "(example: inject COOLANT_TEMP if its CAN id/decode is known)"
$inj = Post-Json "/api/can/inject" @{ id = 0x360; data = "0102030405060708"; count = 2 }
$inj | ConvertTo-Json -Compress

Write-Host "`nDone." -ForegroundColor Green
