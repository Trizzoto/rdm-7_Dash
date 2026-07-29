<#
    setup_gt86_dash.ps1  -  one-shot GT86 / BRZ dash provisioning

    Applies the full "same car" setup to an RDM-7 dash wired to a Toyota 86 /
    Subaru BRZ / Scion FR-S (FA20). Safe to re-run - every step is idempotent.

    What it does:
      1. Finds the dash (uses -Ip if given/reachable, else sweeps the Wi-Fi /24).
      2. Binds 5 gap-fill channels to their OBD2 Mode-01 PIDs (MAP, IAT, lambda,
         short fuel trim, ignition timing) - the values the FA20 does NOT
         broadcast natively and only answers over OBD.
      3. Fixes the VEHICLE_SPEED decode: the default binds it to 0xD1 (ABS/brake
         frame) which caps ~30. This repoints it to the rear-left wheel field on
         0xD4 (scale 0.015694), which is a real 16-bit field and tracks true.
      4. Writes the CALCULATED_GEAR config (6-speed manual ratios, 4.10 final
         drive, 215/45R17 rolling circ) so the gear panel back-computes gear
         from RPM x speed.
      5. Best-effort layout touch-ups on the active layout (only if the tokens
         are present): IGNITION->TIMING_ADVANCE, FUEL_TRIM->SHORT_FUEL_TRIM_1,
         GEAR->CALCULATED_GEAR panel binding, and LAMBDA panel to 2 decimals.

    Prereq: the dash's ECU preset must already be set to "Toyota 86 / Subaru BRZ"
    (via the editor/wizard) so the native CAN signals (RPM, coolant, throttle,
    wheel speeds, ...) decode. The script checks this and warns if not.

    Usage:
      pwsh -File setup_gt86_dash.ps1                 # auto-discover on Wi-Fi
      pwsh -File setup_gt86_dash.ps1 -Ip 172.30.97.226
      pwsh -File setup_gt86_dash.ps1 -Transmission auto   # use 6AT ratios
#>

[CmdletBinding()]
param(
    [string]$Ip = "",
    [ValidateSet("manual","auto")]
    [string]$Transmission = "manual"
)

$ErrorActionPreference = "Stop"

# -- driveline constants ----------------------------------------------------
# 6MT (Aisin TL70): 3.626/2.189/1.541/1.213/1.000/0.767, final 4.100
# 6AT (Aisin A960E): 3.538/2.060/1.404/1.000/0.713/0.582, final 3.909
$ratios6MT = @(0, 3.626, 2.189, 1.541, 1.213, 1.000, 0.767)
$ratios6AT = @(0, 3.538, 2.060, 1.404, 1.000, 0.713, 0.582)
$gearRatios = if ($Transmission -eq "auto") { $ratios6AT } else { $ratios6MT }
$finalDrive = if ($Transmission -eq "auto") { 3.909 } else { 4.100 }
$wheelCirc  = 1.955     # 215/45R17 rolling circumference (m), ~512 rev/km

# Wheel/vehicle speed scale. The GT86 preconfig table shipped 0.015694, which is
# 1/64 to within 0.4% -- and raw/64 is METRES PER SECOND, not km/h. That decode
# therefore emitted m/s while every consumer (channel units, odometer,
# CALCULATED_GEAR) treated it as km/h, so speed read 3.6x low: an indicated
# "30" was really 108 km/h. That is the "speed stops at 30" bug -- it never
# capped, it just under-read. Correct km/h scale is 3.6/64.
$speedScale = 0.05625   # = 3.6/64 km/h per bit
# Frames carrying this value (all share the one scale):
#   0x0D1 (209) bit  0 : VEHICLE SPEED  <- canonical speedo value
#   0x0D4 (212) bits 0/16/32/48 : wheel speeds FL/FR/RL/RR
$speedCanId = 209
$speedBit   = 0

# -- OBD2 channel binds: channel_id, service, pid(dec), registered signal ----
$obdBinds = @(
    @{ ch="manifold_pressure";    svc=1; pid=11; sig="MAP" }               # 0x0B
    @{ ch="intake_air_temp";      svc=1; pid=15; sig="INTAKE_AIR_TEMP" }   # 0x0F
    @{ ch="lambda_bank1";         svc=1; pid=68; sig="LAMBDA" }            # 0x44
    @{ ch="short_term_fuel_trim"; svc=1; pid=6;  sig="SHORT_FUEL_TRIM_1" } # 0x06
    @{ ch="ignition_timing";      svc=1; pid=14; sig="TIMING_ADVANCE" }    # 0x0E
)

# -- gear value->label map -----------------------------------------------------
# CALCULATED_GEAR emits 0 for neutral/stationary (speed <5 km/h or rpm <500) and
# 1..6 for the engaged gear, so a bare readout shows a meaningless "0" in
# neutral. The firmware supports a value->label map authored on a signals[]
# entry in the layout; it attaches to the SIGNAL, so every widget rendering it
# (panel/text/arc) picks the labels up for free and drops the unit suffix.
# No "R" entry: the ratio-matching method cannot detect reverse (it has no sign).
$gearSigEntry = '{"name":"CALCULATED_GEAR","value_map":[{"v":0,"label":"N"},{"v":1,"label":"1"},{"v":2,"label":"2"},{"v":3,"label":"3"},{"v":4,"label":"4"},{"v":5,"label":"5"},{"v":6,"label":"6"}]}'

# -- layout string patches (applied only when the 'from' token is present) ---
$layoutPatches = @(
    @{ from='"signal_name":"IGNITION"';  to='"signal_name":"TIMING_ADVANCE"' }
    @{ from='"signal_name":"FUEL_TRIM"'; to='"signal_name":"SHORT_FUEL_TRIM_1"' }
    @{ from='"signal_name":"GEAR"';      to='"signal_name":"CALCULATED_GEAR"' }
    @{ from='"label":"LAMBDA","custom_text":"","decimals":0'; to='"label":"LAMBDA","custom_text":"","decimals":2' }
)

# -- helpers -----------------------------------------------------------------
function Test-Dash([string]$addr) {
    try {
        $j = (Invoke-WebRequest -Uri "http://$addr/api/device/info" -TimeoutSec 3 -UseBasicParsing).Content | ConvertFrom-Json
        return $j.serial
    } catch { return $null }
}

function Find-Dash([string]$preferred) {
    if ($preferred) {
        $s = Test-Dash $preferred
        if ($s) { Write-Host "  dash at $preferred (serial $s)" -ForegroundColor Green; return $preferred }
        Write-Host "  $preferred not reachable, sweeping subnet..." -ForegroundColor Yellow
    }
    $myip = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.InterfaceAlias -eq 'Wi-Fi' } | Select-Object -First 1).IPAddress
    if (-not $myip) { throw "No Wi-Fi adapter IP found." }
    $subnet = ($myip -split '\.')[0..2] -join '.'
    Write-Host "  sweeping $subnet.0/24 ..." -ForegroundColor Cyan
    $clients = @()
    1..254 | ForEach-Object {
        $a = "$subnet.$_"; $c = New-Object System.Net.Sockets.TcpClient
        $clients += [pscustomobject]@{ ip=$a; client=$c; task=$c.ConnectAsync($a,80) }
    }
    Start-Sleep -Milliseconds 900
    foreach ($e in $clients) {
        $hit = $null
        if ($e.task.IsCompleted -and -not $e.task.IsFaulted -and $e.client.Connected) {
            $s = Test-Dash $e.ip
            if ($s) { $hit = $e.ip; $serial = $s }
        }
        $e.client.Close()
        if ($hit) { Write-Host "  dash at $hit (serial $serial)" -ForegroundColor Green; return $hit }
    }
    throw "No dash found on $subnet.0/24. Check power/Wi-Fi and try -Ip."
}

# GET/POST with retries - the phone-hotspot link drops frequently.
function Invoke-Dash([string]$base, [string]$path, [string]$method="GET", $body=$null) {
    $uri = "http://$base$path"
    for ($i=1; $i -le 5; $i++) {
        try {
            if ($method -eq "POST") {
                $bytes = [System.Text.Encoding]::UTF8.GetBytes(($body | ConvertTo-Json -Depth 6 -Compress))
                return Invoke-WebRequest -Uri $uri -Method POST -Body $bytes -ContentType "application/json" -TimeoutSec 10 -UseBasicParsing
            }
            return Invoke-WebRequest -Uri $uri -TimeoutSec 8 -UseBasicParsing
        } catch {
            if ($i -eq 5) { throw }
            Start-Sleep -Milliseconds 700
        }
    }
}

# POST a raw pre-serialized JSON string (used for the layout save).
function Invoke-DashRaw([string]$base, [string]$path, [string]$json) {
    $uri = "http://$base$path"
    for ($i=1; $i -le 5; $i++) {
        try {
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
            return Invoke-WebRequest -Uri $uri -Method POST -Body $bytes -ContentType "application/json" -TimeoutSec 15 -UseBasicParsing
        } catch {
            if ($i -eq 5) { throw }
            Start-Sleep -Milliseconds 700
        }
    }
}

# -- run ---------------------------------------------------------------------
Write-Host "`n=== GT86 / BRZ dash setup ($Transmission transmission) ===" -ForegroundColor White
Write-Host "[1/6] locating dash..."
$base = Find-Dash $Ip

Write-Host "[2/6] preflight: ECU preset check"
try {
    $ecu = (Invoke-Dash $base "/api/ecu/current").Content | ConvertFrom-Json
    $isGt86 = ("$($ecu.make) $($ecu.version)" -match '86|BRZ|GT86|FR-S')
    if ($isGt86) { Write-Host "      ECU = $($ecu.make) / $($ecu.version)  OK" -ForegroundColor Green }
    else {
        Write-Host "      WARNING: ECU preset is '$($ecu.make) / $($ecu.version)', not a GT86/BRZ." -ForegroundColor Yellow
        Write-Host "      Set the 'Toyota 86 / Subaru BRZ' preset in the editor first, then re-run." -ForegroundColor Yellow
    }
} catch { Write-Host "      (couldn't read ECU preset - continuing)" -ForegroundColor Yellow }

Write-Host "[3/6] binding OBD2 gap-fill PIDs"
foreach ($b in $obdBinds) {
    $body = @{ channel_id=$b.ch; source_type="obd2"; obd2_service=$b.svc; obd2_pid=$b.pid; signal_name=$b.sig }
    try {
        $r = Invoke-Dash $base "/api/channels/bind-source" "POST" $body
        Write-Host ("      {0,-22} <- OBD svc{1} pid 0x{2:X2} ({3})  OK" -f $b.ch,$b.svc,$b.pid,$b.sig) -ForegroundColor Green
    } catch { Write-Host ("      {0,-22} FAILED: {1}" -f $b.ch,$_.Exception.Message) -ForegroundColor Red }
}

Write-Host "[4/6] fixing speed scale (m/s -> km/h, x3.6)"
# vehicle_speed: pin to the canonical 0x0D1 speedo field AND correct the scale.
$spd = @{ id="vehicle_speed"; fields=@{ decode=@{ can_id=$speedCanId; bit_start=$speedBit; bit_length=16; scale=$speedScale; offset=0; is_signed=$true; endian=1 } } }
try {
    Invoke-Dash $base "/api/channels/update" "POST" $spd | Out-Null
    Write-Host ("      vehicle_speed -> 0x{0:X} bit{1} scale {2}  OK" -f $speedCanId,$speedBit,$speedScale) -ForegroundColor Green
} catch { Write-Host "      vehicle_speed FAILED: $($_.Exception.Message)" -ForegroundColor Red }

# The four wheel-speed channels share the same bad scale. Fix any that exist,
# preserving each one's own bit_start (FL=0, FR=16, RL=32, RR=48).
try {
    $present = ((Invoke-Dash $base "/api/channels").Content | ConvertFrom-Json).channels
    foreach ($w in @("wheel_speed_fl","wheel_speed_fr","wheel_speed_rl","wheel_speed_rr")) {
        $c = $present | Where-Object { $_.id -eq $w }
        if (-not $c) { continue }
        $body = @{ id=$w; fields=@{ decode=@{ scale=$speedScale } } }   # partial patch: scale only
        try {
            Invoke-Dash $base "/api/channels/update" "POST" $body | Out-Null
            Write-Host ("      {0,-16} scale -> {1}  OK" -f $w,$speedScale) -ForegroundColor Green
        } catch { Write-Host ("      {0} FAILED: {1}" -f $w,$_.Exception.Message) -ForegroundColor Red }
    }
} catch { Write-Host "      (couldn't enumerate wheel channels)" -ForegroundColor Yellow }

Write-Host "[5/6] writing CALCULATED_GEAR config"
$gcfg = @{ wheel_circumference_m=$wheelCirc; final_drive=$finalDrive; rpm_signal="RPM"; speed_signal="VEHICLE_SPEED"; enabled=$true; ratios=$gearRatios }
try {
    Invoke-Dash $base "/api/gear/config" "POST" $gcfg | Out-Null
    Write-Host ("      final={0} circ={1} ratios=[{2}]  OK" -f $finalDrive,$wheelCirc,($gearRatios -join ",")) -ForegroundColor Green
} catch { Write-Host "      gear config FAILED: $($_.Exception.Message)" -ForegroundColor Red }

Write-Host "[6/6] layout touch-ups (only if tokens present)"
try {
    $raw = (Invoke-Dash $base "/api/layout/current").Content
    $new = $raw; $changed = 0
    foreach ($p in $layoutPatches) {
        if ($new.Contains($p.from)) { $new = $new.Replace($p.from, $p.to); $changed++; Write-Host "      patched: $($p.from)" -ForegroundColor Green }
    }

    # Gear labels: inject the CALCULATED_GEAR value_map into signals[] so 0 shows
    # as "N". Skipped when an entry for that signal already exists (idempotent).
    # Surgical string insert, NOT ConvertTo-Json -- round-tripping the whole
    # layout through PS 5.1 JSON would reformat floats and risk mangling it.
    $marker = '"signals":['
    if ($new.Contains('"name":"CALCULATED_GEAR"')) {
        Write-Host "      gear value_map already present - skipped" -ForegroundColor DarkGray
    } elseif ($new.Contains($marker)) {
        $at  = $new.IndexOf($marker) + $marker.Length
        $sep = if ($new.Substring($at,1) -eq ']') { '' } else { ',' }   # empty array needs no comma
        $new = $new.Insert($at, $gearSigEntry + $sep)
        $changed++
        Write-Host "      gear value_map added (0 -> N)" -ForegroundColor Green
    } elseif (-not $new.Contains('"signals"')) {
        $new = $new.Insert(1, $marker + $gearSigEntry + '],')
        $changed++
        Write-Host "      gear value_map added via new signals[] (0 -> N)" -ForegroundColor Green
    } else {
        Write-Host "      signals[] in unexpected format - gear map skipped" -ForegroundColor Yellow
    }

    if ($changed -gt 0) {
        Invoke-DashRaw $base "/api/layout/save" $new | Out-Null
        Write-Host "      saved layout ($changed change(s))  OK" -ForegroundColor Green
    } else { Write-Host "      no layout tokens matched (already patched or different layout) - skipped" -ForegroundColor DarkGray }
} catch { Write-Host "      layout touch-up FAILED: $($_.Exception.Message)" -ForegroundColor Red }

# -- verify ------------------------------------------------------------------
Write-Host "`n=== verification ===" -ForegroundColor White
try {
    $vs = ((Invoke-Dash $base "/api/channels").Content | ConvertFrom-Json).channels | Where-Object { $_.id -eq "vehicle_speed" }
    Write-Host ("  vehicle_speed decode: can_id=0x{0:X} bit={1} scale={2}" -f $vs.decode.can_id,$vs.decode.bit_start,[math]::Round($vs.decode.scale,6))
    $g = (Invoke-Dash $base "/api/gear/config").Content | ConvertFrom-Json
    Write-Host ("  gear: enabled={0} final={1} ratios=[{2}]" -f $g.enabled,$g.final_drive,($g.ratios -join ","))
    $vals = ((Invoke-Dash $base "/api/signals/values").Content | ConvertFrom-Json).signals
    foreach ($n in @("MAP","INTAKE_AIR_TEMP","LAMBDA","SHORT_FUEL_TRIM_1","TIMING_ADVANCE","VEHICLE_SPEED","WHEEL_SPD_RL","CALCULATED_GEAR","RPM")) {
        $s = $vals | Where-Object { $_.name -eq $n }
        if ($s) { Write-Host ("  {0,-18} = {1,-8} {2}" -f $n,[math]::Round($s.value,2),$(if($s.stale){"stale"}else{"fresh"})) }
    }
} catch { Write-Host "  verify read failed: $($_.Exception.Message)" -ForegroundColor Red }

Write-Host "`nDone. Rev-check: VEHICLE_SPEED should now track WHEEL_SPD_RL past 30 km/h." -ForegroundColor White
