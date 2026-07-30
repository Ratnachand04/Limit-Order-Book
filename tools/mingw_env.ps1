<#
.SYNOPSIS
  Puts the portable MinGW-w64 / CMake / Ninja toolchain on PATH for this shell.

.DESCRIPTION
  A no-administrator alternative to installing the Visual Studio C++ workload.
  Everything lives under one directory in the user profile and nothing is
  written to the registry or the machine PATH, so undoing it is `rm -r` of that
  directory.

  mingw64\bin MUST be on PATH to *run* anything built with this toolchain, not
  only to build: the executables link against libstdc++-6.dll, libgcc_s_seh-1.dll
  and libwinpthread-1.dll, which live there.  Without it every binary exits with
  0xC0000135 (STATUS_DLL_NOT_FOUND) and no error message.

.EXAMPLE
  . tools\mingw_env.ps1          # note the leading dot: dot-source it
  cmake --preset mingw-release
  cmake --build --preset mingw-release
  ctest --preset mingw-release --output-on-failure

.NOTES
  Set LOB_TOOLCHAIN_ROOT to override the default location.
#>
[CmdletBinding()]
param(
  [string]$Root = $(if ($env:LOB_TOOLCHAIN_ROOT) { $env:LOB_TOOLCHAIN_ROOT } else { "$env:USERPROFILE\toolchains" })
)

if (-not (Test-Path $Root)) {
  Write-Host "Toolchain root not found: $Root" -ForegroundColor Red
  Write-Host "Install it with tools\install_portable_toolchain.ps1, or set LOB_TOOLCHAIN_ROOT."
  return
}

$mingwBin = Join-Path $Root "mingw64\bin"
$cmakeBin = (Get-ChildItem -Path $Root -Directory -Filter "cmake-*" -ErrorAction SilentlyContinue |
             Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName "bin" })
$ninjaDir = Join-Path $Root "ninja"

$added = @()
foreach ($dir in @($mingwBin, $cmakeBin, $ninjaDir)) {
  if ($dir -and (Test-Path $dir) -and ($env:PATH -notlike "*$dir*")) {
    $env:PATH = "$dir;$env:PATH"
    $added += $dir
  }
}

Write-Host "Toolchain on PATH:" -ForegroundColor Green
foreach ($dir in $added) { Write-Host "  $dir" }
foreach ($exe in @('g++', 'gcc', 'cmake', 'ninja', 'gdb')) {
  $cmd = Get-Command $exe -ErrorAction SilentlyContinue
  if ($cmd) {
    $ver = (& $exe --version 2>$null | Select-Object -First 1)
    Write-Host ("  {0,-6} {1}" -f $exe, $ver)
  } else {
    Write-Host ("  {0,-6} NOT FOUND" -f $exe) -ForegroundColor Yellow
  }
}
