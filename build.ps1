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

& $windres -i capslang.rc -o capslang_res.o
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

# -fno-exceptions/-fno-rtti: GDI++ wrapper uses status codes, not throws;
# drops ~240 KB of static C++ runtime from the exe.
& $gxx capslang.cpp capslang_res.o -o capslang.exe `
    -municode -mwindows -O2 -s -static -fno-exceptions -fno-rtti `
    -lshell32 -lgdi32 -lgdiplus -lshlwapi -lole32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Write-Host "OK: capslang.exe built"
