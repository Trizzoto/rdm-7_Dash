# Build and run every native-host test. Windows-friendly alternative to `make`.
#
# Usage:
#   .\run.ps1                    # build & run every test
#   .\run.ps1 test_can_decode    # build & run one test
#   .\run.ps1 test_widget_rules  # build & run one test
#   .\run.ps1 -Clean             # delete build artifacts

param(
	[string]$Test = "",
	[switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if ($Clean) {
	if (Test-Path "build") { Remove-Item -Recurse -Force build }
	Write-Host "Cleaned." -ForegroundColor Green
	exit 0
}

# Discover tests by pattern match on file names.
$tests = if ($Test) {
	@($Test)
} else {
	Get-ChildItem -Filter "test_*.c" | ForEach-Object { $_.BaseName }
}

if (-not (Test-Path "build")) { New-Item -ItemType Directory -Path "build" | Out-Null }

$cflags = @("-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-O0", "-g")
$ldflags = @("-lm")

# Per-test extra build inputs. Tests not listed use the default
# (test_<name>.c + unity.c, no extra include paths).
function Get-ExtraSources($name) {
	switch ($name) {
		"test_widget_rules" { return @("cjson/cJSON.c") }
		default              { return @() }
	}
}
function Get-ExtraIncludes($name) {
	switch ($name) {
		"test_widget_rules" { return @("-Imocks", "-Icjson", "-I.") }
		default              { return @() }
	}
}

$totalFailed = 0
foreach ($t in $tests) {
	$src = "$t.c"
	if (-not (Test-Path $src)) {
		Write-Host "Test source not found: $src" -ForegroundColor Red
		exit 2
	}
	$out = "build\$t.exe"
	$extraSrc = Get-ExtraSources $t
	$extraInc = Get-ExtraIncludes $t

	Write-Host "▶ Building $t" -ForegroundColor Cyan
	$args = @() + $cflags + $extraInc + @($src, "unity.c") + $extraSrc + @("-o", $out) + $ldflags
	& gcc @args
	if ($LASTEXITCODE -ne 0) {
		Write-Host "  Build failed for $t" -ForegroundColor Red
		exit 3
	}
	Write-Host "▶ Running $t" -ForegroundColor Cyan
	& $out
	if ($LASTEXITCODE -ne 0) { $totalFailed++ }
}

if ($totalFailed -gt 0) {
	Write-Host "`n$totalFailed test binar(y/ies) failed." -ForegroundColor Red
	exit 1
}
Write-Host "`nAll tests passed." -ForegroundColor Green
exit 0
