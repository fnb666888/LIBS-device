[CmdletBinding()]
param([switch]$Release, [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$savedEnvironment = @{}
foreach ($name in @('PATH','CARGO_HOME','RUSTUP_HOME','RUSTFLAGS','LIBS_FAKE_AVASPEC_DLL')) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$mappedDrive = $null
$locationPushed = $false
try {
$root = Split-Path $PSScriptRoot -Parent
$localCargo = Join-Path $root '.tools/cargo/bin/cargo.exe'
if (Test-Path -LiteralPath $localCargo) {
    $env:CARGO_HOME = Join-Path $root '.tools/cargo'
    $env:RUSTUP_HOME = Join-Path $root '.tools/rustup'
    $env:PATH = (Join-Path $root '.tools/cargo/bin') + ';' + $env:PATH
}
$mingw = Join-Path (Split-Path $root -Parent) 'LIBS-device/.local-tools/Qt/Tools/mingw1310_64'
$portableMingw = Join-Path $env:TEMP 'libs-rust-mingw'
if ((Test-Path -LiteralPath $mingw) -and -not (Test-Path -LiteralPath $portableMingw)) {
    # MinGW 13 cannot resolve its runtime libraries from a Unicode installation path.
    Copy-Item -LiteralPath $mingw -Destination $portableMingw -Recurse
}
if (Test-Path -LiteralPath $portableMingw) { $env:PATH = (Join-Path $portableMingw 'bin') + ';' + $env:PATH }
$sysroot = & rustc --print sysroot
if ($LASTEXITCODE -ne 0) { throw 'Install a stable Rust toolchain first' }
$buildRoot = $root
if ((& rustc -vV) -match 'host: .*windows-gnu') {
    if ($root -match '[^\x00-\x7F]') {
        foreach ($letter in @('R', 'S', 'U', 'V', 'W', 'X', 'Y', 'Z')) {
            if (-not (Test-Path "${letter}:\")) {
                & subst "${letter}:" $root
                if ($LASTEXITCODE -eq 0) { $mappedDrive = "${letter}:"; break }
            }
        }
        if (-not $mappedDrive) { throw 'No free drive letter for the GNU toolchain Unicode-path workaround' }
        $buildRoot = "$mappedDrive/"
        if (Test-Path -LiteralPath $localCargo) {
            $env:CARGO_HOME = Join-Path $buildRoot '.tools/cargo'
            $env:RUSTUP_HOME = Join-Path $buildRoot '.tools/rustup'
            $env:PATH = (Join-Path $buildRoot '.tools/cargo/bin') + ';' + $env:PATH
            $sysroot = & rustc --print sysroot
        }
    }
    $env:PATH = (Join-Path $sysroot 'lib/rustlib/x86_64-pc-windows-gnu/bin/gcc-ld') + ';' + $env:PATH
    $env:RUSTFLAGS = "$env:RUSTFLAGS -C link-arg=-fuse-ld=lld".Trim()
}
Push-Location (Join-Path $buildRoot 'backend')
$locationPushed = $true
    & cargo fmt --check
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed; run cargo fmt first (rustfmt component required)' }
    $flags = @('--locked')
    if ($Release) { $flags += '--release' }
    if (-not $SkipTests) {
        $testFlags = @('--test-threads=1')
        if (Get-Command g++ -ErrorAction SilentlyContinue) {
            $env:LIBS_FAKE_AVASPEC_DLL = Join-Path $env:TEMP 'libs-rust-fake-avaspec.dll'
            & g++ -shared -static-libgcc -static-libstdc++ (Join-Path $buildRoot 'src/tests/fake_avaspec_sdk.cpp') -o $env:LIBS_FAKE_AVASPEC_DLL
            if ($LASTEXITCODE -ne 0) { throw 'SDK fixture compilation failed' }
            $testFlags += '--include-ignored'
        } else { Write-Warning 'SDK fixture tests require g++; other tests will run.' }
        & cargo test @flags -- @testFlags
        if ($LASTEXITCODE -ne 0) { throw 'Rust tests failed' }
    }
    & cargo build @flags
    if ($LASTEXITCODE -ne 0) { throw 'Rust build failed' }
} finally {
    if ($locationPushed) { Pop-Location }
    if ($mappedDrive) { & subst $mappedDrive /d }
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
