# Build script for capslang (ASCII only - no BOM issues).
# Toolchain: any mingw-w64 g++ + windres. Locally that is LLVM-MinGW installed
# via winget (MartinStorsjo.LLVM-MinGW.UCRT); on CI it comes from PATH.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Resolve-Tool([string]$name) {
    $inPath = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($inPath) { return $inPath.Source }

    # winget package layout (its PATH links only appear in a fresh shell)
    $pkgRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    $bin = Get-ChildItem "$pkgRoot\MartinStorsjo.LLVM-MinGW*" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter "llvm-mingw-*" } |
        Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName "bin" }
    if ($bin -and (Test-Path "$bin\$name.exe")) { return "$bin\$name.exe" }

    throw "$name not found. Install a mingw-w64 toolchain, e.g. winget install MartinStorsjo.LLVM-MinGW.UCRT"
}

$windres = Resolve-Tool "windres"
$gxx     = Resolve-Tool "g++"

& $windres -i capslang.rc -o capslang_res.o
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

# -fno-exceptions/-fno-rtti: GDI++ wrapper uses status codes, not throws;
# drops ~240 KB of static C++ runtime from the exe.
& $gxx capslang.cpp capslang_res.o -o capslang.exe `
    -municode -mwindows -O2 -s -static -fno-exceptions -fno-rtti `
    -lshell32 -lgdi32 -lgdiplus -lshlwapi -lole32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Write-Host "OK: capslang.exe built"
