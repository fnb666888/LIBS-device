<#
.SYNOPSIS
    编译并运行 libs_core 的全部离线测试。不需要 CMake，一条命令搞定。

.DESCRIPTION
    刻意不依赖 CMake：光谱仪替身 DLL 的加载路径必须是绝对 UTF-8，而 Windows 的
    argv 是 ANSI 代码页字节，任何生成器都救不了，只能在调用侧绕开。这里直接按文件编译，
    只用 g++ 和一个 PowerShell 循环，因此不挑生成器、不挑工程路径。

    产物都落在 build/ 下：
      build\libs_protocols.a / libs_transport.a / libs_devices.a   三个静态库
      build\test_protocols.exe / test_devices.exe / test_spectrometer.exe
      build\fake_avaspec_test.dll                                  光谱仪测试替身

.PARAMETER Compiler
    g++ 路径。默认用 PATH 里的 g++。

.PARAMETER SkipTests
    只编译，不运行测试。

.EXAMPLE
    pwsh -File build.ps1
    pwsh -File build.ps1 -SkipTests
#>
[CmdletBinding()]
param(
    [string]$Compiler = "g++",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$outDir = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# 静态库用 ar；MinGW 的 g++ 目录里就有。
$ar = Join-Path (Split-Path -Parent (Get-Command $Compiler).Source) "ar.exe"
if (-not (Test-Path $ar)) { $ar = "ar" }

$flags = @("-std=c++17", "-Wall", "-Wextra", "-Iinclude", "-Itests")

# 三层。列表顺序即依赖顺序：protocols <- transport <- devices
$layers = [ordered]@{
    "libs_protocols" = @(
        "src/error.cpp", "src/hex.cpp",
        "src/protocols/modbus_rtu.cpp", "src/protocols/laser.cpp",
        "src/protocols/rangefinder.cpp", "src/protocols/indicator.cpp",
        "src/protocols/air_conditioner.cpp", "src/protocols/su300.cpp")
    "libs_transport" = @("src/transport/serial.cpp")
    "libs_devices"   = @(
        "src/devices/laser.cpp", "src/devices/rangefinder.cpp",
        "src/devices/indicator.cpp", "src/devices/spectrometer.cpp",
        "src/devices/air_conditioner.cpp", "src/devices/su300.cpp")
    # 第 4 层：业务层。只在 devices 之上组装语义，不反向改驱动。
    "libs_business"  = @(
        "src/business/time_util.cpp",
        "src/business/json/json.cpp",
        "src/business/config/site_config.cpp",
        "src/business/integration/heike_events.cpp",
        "src/business/session/device_session.cpp",
        "src/business/policy/detection_config.cpp",
        "src/business/policy/gate.cpp",
        "src/business/logging/run_log.cpp",
        "src/business/services/ac_interlock.cpp",
        "src/business/services/business_event.cpp",
        "src/business/services/capture.cpp",
        "src/business/services/composition.cpp",
        "src/business/services/data_delivery.cpp",
        "src/business/services/environment.cpp",
        "src/business/services/laser_fire.cpp",
        "src/business/services/facade.cpp",
        "src/business/services/motion.cpp",
        "src/business/services/ore_presence.cpp",
        "src/business/services/spectrum_stream.cpp",
        "src/business/app/runtime.cpp")
}

function Invoke-Checked {
    param([string]$What, [scriptblock]$Action)
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

Write-Host "==> compiling static libraries"
$archives = @()
foreach ($name in $layers.Keys) {
    $archive = Join-Path $outDir "$name.a"
    $objects = @()
    foreach ($source in $layers[$name]) {
        $object = Join-Path $outDir ("{0}.{1}.o" -f $name, [IO.Path]::GetFileNameWithoutExtension($source))
        Invoke-Checked "compiling $source" { & $Compiler @flags -c $source -o $object }
        $objects += $object
    }
    if (Test-Path $archive) { Remove-Item -Force $archive }
    Invoke-Checked "archiving $name" { & $ar rcs $archive @objects }
    $archives += $archive
    Write-Host "    $name.a"
}
# 静态库按依赖顺序给链接器：business -> devices -> transport -> protocols
$linkOrder = @(
    (Join-Path $outDir "libs_devices.a"),
    (Join-Path $outDir "libs_transport.a"),
    (Join-Path $outDir "libs_protocols.a"))
$businessOrder = @((Join-Path $outDir "libs_business.a")) + $linkOrder

Write-Host "==> compiling the AvaSpec stand-in DLL"
$fakeDll = Join-Path $outDir "fake_avaspec_test.dll"
Invoke-Checked "building the fake AvaSpec SDK" {
    & $Compiler -std=c++17 -Wall -Wextra -shared -o $fakeDll "tests/fake_avaspec_sdk.cpp"
}

# businessd 的两个源文件不进静态库：main 是可执行入口，named_pipe_server 归守护进程所有，
# 只被 businessd.exe 和离线 IPC 测试复用。
Write-Host "==> compiling the businessd daemon"
$daemonSources = @("src/businessd/named_pipe_server.cpp", "src/businessd/main.cpp")
$daemonObjects = @()
foreach ($source in $daemonSources) {
    $object = Join-Path $outDir ("businessd.{0}.o" -f [IO.Path]::GetFileNameWithoutExtension($source))
    Invoke-Checked "compiling $source" { & $Compiler @flags -c $source -o $object }
    $daemonObjects += $object
}
$daemon = Join-Path $outDir "businessd.exe"
Invoke-Checked "linking businessd.exe" { & $Compiler @flags $daemonObjects $businessOrder -o $daemon -ladvapi32 }
Write-Host "    businessd.exe"

$ipcOrder = @((Join-Path $outDir "businessd.named_pipe_server.o")) + $businessOrder

Write-Host "==> compiling tests"
$testSupport = Join-Path $outDir "check.o"
Invoke-Checked "compiling tests/check.cpp" { & $Compiler @flags -c "tests/check.cpp" -o $testSupport }

$tests = [ordered]@{
    "test_protocols"    = @("tests/test_protocols.cpp",    $linkOrder)
    "test_devices"      = @("tests/test_devices.cpp",      $linkOrder)
    "test_air_conditioner_protocol" = @("tests/test_air_conditioner_protocol.cpp", $linkOrder)
    "test_air_conditioner_device"   = @("tests/test_air_conditioner_device.cpp",   $linkOrder)
    "test_su300"          = @("tests/test_su300.cpp",          $linkOrder)
    "test_spectrometer" = @("tests/test_spectrometer.cpp", $linkOrder)
    "test_business_core"     = @("tests/test_business_core.cpp",     $businessOrder)
    "test_business_services" = @("tests/test_business_services.cpp", $businessOrder)
    "test_business_facade"   = @("tests/test_business_facade.cpp",   $businessOrder)
    # IPC 测试覆盖 handle_line()，并在测试进程内启动真实命名管道；
    # 不拖 main.cpp，避免另起一个 businessd 入口。
    "test_business_ipc"      = @("tests/test_business_ipc.cpp", $ipcOrder)
    "test_laser_fire"        = @("tests/test_laser_fire.cpp",       $businessOrder)
    "test_spectrum_stream"   = @("tests/test_spectrum_stream.cpp",  $businessOrder)
}
foreach ($name in $tests.Keys) {
    $source = $tests[$name][0]
    $exe = Join-Path $outDir "$name.exe"
    $libs = @($tests[$name][1])
    $extra = @()
    if ($name -eq "test_business_ipc") { $extra = @("-ladvapi32") }
    Invoke-Checked "linking $name" { & $Compiler @flags $source $testSupport @libs @extra -o $exe }
    Write-Host "    $name.exe"
}

if ($SkipTests) { Write-Host "==> skipping tests (-SkipTests)"; return }

# 光谱仪测试自己会把替身 DLL 复制到一个纯 ASCII 的临时目录再交给驱动：
# 驱动要求绝对 UTF-8 路径，而 Windows 的 argv 是当前 ANSI 代码页的字节，
# 非 ASCII 路径直接传进去会被拒。这里只负责把原始产物路径传过去。
Write-Host "==> running tests"
$failed = @()
foreach ($name in $tests.Keys) {
    $exe = Join-Path $outDir "$name.exe"
    if ($name -eq "test_spectrometer") { & $exe $fakeDll } else { & $exe }
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}

if ($failed.Count) {
    Write-Error ("failed: " + ($failed -join ", "))
    exit 1
}
Write-Host "==> all tests passed"




