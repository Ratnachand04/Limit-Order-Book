<#
.SYNOPSIS
  Verifies that every tool this project needs is present and actually works.

.DESCRIPTION
  Checking that cl.exe exists is not enough: a Visual Studio install can carry
  the compiler binary while missing the Windows SDK, in which case cl.exe fails
  on `#include <cstdio>`.  This script therefore compiles and runs a real
  program before reporting success.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\check_toolchain.ps1
#>
[CmdletBinding()]
param(
  [switch]$Quiet
)

$ErrorActionPreference = 'Continue'
$script:Failures = @()

function Report {
  param([string]$Name, [bool]$Ok, [string]$Detail)
  $mark = if ($Ok) { '[ OK ]' } else { '[FAIL]' }
  Write-Host ("{0} {1,-28} {2}" -f $mark, $Name, $Detail)
  if (-not $Ok) { $script:Failures += $Name }
}

Write-Host "`n=== lob_sim toolchain check ===`n"

# --- Visual Studio / MSVC ---------------------------------------------------
# Note the absence of -all: vswhere hides INCOMPLETE installs by default, and an
# incomplete install is exactly what we must not accept. A partially installed
# Build Tools instance can carry cl.exe while missing the Windows SDK, which
# fails at the first #include rather than at configure time.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
$incomplete = @()
if (Test-Path $vswhere) {
  $vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1

  # Surface incomplete instances so the diagnosis is not "nothing is installed"
  # when the truth is "something is half installed".
  $all = & $vswhere -all -products * -prerelease -format json | Out-String
  if ($all.Trim()) {
    foreach ($inst in ($all | ConvertFrom-Json)) {
      if (-not $inst.isComplete) { $incomplete += "$($inst.displayName) at $($inst.installationPath)" }
    }
  }
}
Report 'Visual Studio (C++)' ([bool]$vsPath) $(if ($vsPath) { $vsPath } else { 'no COMPLETE instance with the VC++ toolset' })
foreach ($inc in $incomplete) {
  Write-Host ("       incomplete install found: {0}" -f $inc) -ForegroundColor Yellow
}

# --- Windows SDK ------------------------------------------------------------
$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
$sdks = if (Test-Path $sdkRoot) { (Get-ChildItem $sdkRoot -Directory).Name } else { @() }
Report 'Windows SDK' ($sdks.Count -gt 0) $(if ($sdks) { $sdks -join ', ' } else { 'MISSING - cl.exe cannot find stdio.h without it' })

# --- Compile-and-run smoke test --------------------------------------------
$compileOk = $false
$compileDetail = 'skipped (no VC++ toolset)'
if ($vsPath) {
  $tmp = Join-Path $env:TEMP ("lob_toolcheck_" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  @'
#include <cstdio>
#include <map>
#include <string>
#include <vector>
int main() {
  std::map<int, long long> m;
  m[1] = 2;
  std::vector<std::string> v{"c++20"};
  std::printf("%ld %lld %s\n", static_cast<long>(__cplusplus), m[1], v[0].c_str());
  return 0;
}
'@ | Set-Content -Path (Join-Path $tmp 'probe.cpp') -Encoding ascii

  $bat = Join-Path $tmp 'build.bat'
  @"
@echo off
call "$vsPath\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
cd /d "$tmp"
cl /nologo /std:c++20 /EHsc /W4 probe.cpp >compile.log 2>&1 || exit /b 1
probe.exe
"@ | Set-Content -Path $bat -Encoding ascii

  $out = & cmd /c $bat 2>&1
  $compileOk = ($LASTEXITCODE -eq 0)
  $compileDetail = if ($compileOk) { "compiled and ran: $out" } else { (Get-Content (Join-Path $tmp 'compile.log') -ErrorAction SilentlyContinue | Select-Object -First 3) -join ' / ' }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Report 'cl.exe compile + run' $compileOk $compileDetail

# --- CMake / Ninja ----------------------------------------------------------
function Find-Tool {
  param([string]$Exe, [string[]]$ExtraDirs)
  $c = Get-Command $Exe -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($d in $ExtraDirs) { if (Test-Path $d) { return $d } }
  return $null
}

$cmakeCandidates = @()
$ninjaCandidates = @()
if ($vsPath) {
  $cmakeCandidates += "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  $ninjaCandidates += "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
}
$cmake = Find-Tool 'cmake' $cmakeCandidates
$ninja = Find-Tool 'ninja' $ninjaCandidates
Report 'cmake' ([bool]$cmake) $(if ($cmake) { "$cmake  ($(& $cmake --version | Select-Object -First 1))" } else { 'not found' })
Report 'ninja' ([bool]$ninja) $(if ($ninja) { $ninja } else { 'not found (VS generator will be used instead)' })

# --- git --------------------------------------------------------------------
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
Report 'git' ([bool]$git) $(if ($git) { (& git --version) } else { 'not found' })

# --- Python (recorder + analysis) ------------------------------------------
$pythons = @(
  "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
  "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
  "C:\Python312\python.exe"
) | Where-Object { Test-Path $_ }
$pyOk = $false
$pyDetail = 'no working interpreter found'
foreach ($p in @($pythons) + @((Get-Command python -ErrorAction SilentlyContinue).Source)) {
  if (-not $p) { continue }
  $v = & $p -c "import sys,asyncio,gzip;print(sys.version.split()[0])" 2>$null
  if ($LASTEXITCODE -eq 0 -and $v) { $pyOk = $true; $pyDetail = "$p  (Python $v)"; break }
}
Report 'python (3.11+, stdlib ok)' $pyOk $pyDetail

# --- verdict ----------------------------------------------------------------
Write-Host ''
if ($script:Failures.Count -eq 0) {
  Write-Host 'All checks passed. Configure the build with:' -ForegroundColor Green
  Write-Host '    cmake --preset msvc-debug'
  Write-Host '    cmake --build --preset msvc-debug'
  Write-Host '    ctest --preset msvc-debug'
  exit 0
}
Write-Host ("FAILED: " + ($script:Failures -join ', ')) -ForegroundColor Red
if ($script:Failures -contains 'Windows SDK' -or $script:Failures -contains 'cl.exe compile + run') {
  Write-Host 'Run tools\install_cpp_toolchain.cmd as Administrator to fix the C++ toolchain.'
}
exit 1
