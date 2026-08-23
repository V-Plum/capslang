# Build script for capslang (ASCII only - no BOM issues).
# Toolchain: LLVM-MinGW installed via winget (MartinStorsjo.LLVM-MinGW.UCRT).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Resolve LLVM-MinGW bin dir (winget package layout; PATH links need a fresh shell)
$pkgRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
$bin = Get-ChildItem "$pkgRoot\MartinStorsjo.LLVM-MinGW*" -Directory |
    ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter "llvm-mingw-*" } |
    Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName "bin" }
if (-not $bin -or -not (Test-Path "$bin\g++.exe")) { throw "LLVM-MinGW not found" }
$windres = Join-Path $bin "windres.exe"
$gxx     = Join-Path $bin "g++.exe"

if (-not (Test-Path "capslang.ico")) { python make_icon.py }

& $windres -i capslang.rc -o capslang_res.o
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

& $gxx capslang.cpp capslang_res.o -o capslang.exe `
    -municode -mwindows -O2 -s -static -lshell32 -lgdi32
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Write-Host "OK: capslang.exe built"
