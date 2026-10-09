<#
.SYNOPSIS
    编译并运行 libs_core 的全部离线测试。不需要 CMake，一条命令搞定。

.DESCRIPTION
    刻意不依赖 CMake：光谱仪替身 DLL 的加载路径必须是绝对 UTF-8，而 Windows 的
    argv 是 ANSI 代码页字节，任何生成器都救不了，只能在调用侧绕开。这里直接按文件编译，
    只用 g++ 和一个 PowerShell 循环，因此不挑生成器、不挑工程路径。

    产物都落在 build/ 下：
      build\libs_transport.a / libs_devices.a          两个静态库
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

# 列表顺序即依赖顺序：transport <- devices <- business
$layers = [ordered]@{
    "libs_transport" = @("src/error.cpp", "src/hex.cpp", "src/transport/serial.cpp", "src/transport/serial_timing.cpp")
    "libs_devices"   = @(
        "src/protocols/modbus_rtu.cpp", "src/protocols/laser.cpp",
        "src/protocols/rangefinder.cpp", "src/protocols/indicator.cpp",
        "src/protocols/air_conditioner.cpp",
        "src/devices/laser.cpp", "src/devices/rangefinder.cpp",
        "src/devices/indicator.cpp", "src/devices/spectrometer.cpp",
    "src/devices/air_conditioner.cpp")
    # 业务层。只在 devices 之上组装业务语义，不反向改驱动。
    "libs_business"  = @(
        # shared/：共同底座。它不认识任何业务（下面的依赖方向检查守住这条）。
        "src/business/shared/time_util.cpp",
        "src/business/shared/json/json.cpp",
        "src/business/shared/config/site_config.cpp",
        "src/business/shared/config/parameter_store.cpp",
        "src/business/shared/session/device_session.cpp",
        "src/business/shared/logging/run_log.cpp",
        "src/business/shared/data_delivery.cpp",
        # readiness/：业务一，就绪判定。
        "src/business/readiness/facts.cpp",
        "src/business/readiness/verdict.cpp",
        "src/business/readiness/lamp.cpp",
        "src/business/readiness/monitor.cpp",
        "src/business/readiness/precheck.cpp",
        # measurement/：业务二，激光触发光谱检测。
        "src/business/measurement/frame.cpp",
        "src/business/measurement/frame_bus.cpp",
        "src/business/measurement/writer.cpp",
        "src/business/measurement/capture_plan.cpp",
        "src/business/measurement/job.cpp",
        # app/：进程边界与装配。ParameterService 放这儿而不是 shared/，
        # 因为它要引用 measurement::FireParameters / SpectrumMeasurement ——
        # 放 shared 会直接违反下面的依赖方向检查。
        "src/business/app/parameter_service.cpp",
        "src/business/app/runtime.cpp",
        "src/business/app/facade.cpp")
}

function Invoke-Checked {
    param([string]$What, [scriptblock]$Action)
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

# ── 依赖方向静态检查（方案 §2 约束 3 / §7 步骤 9）──────────────────────
#
# shared/ 是两个业务的**共同底座**，不属于任何一个。反向依赖一旦发生，
# "ParameterStore 不认识 MeasurementJob" 这句话就失效了，而这种依赖一旦
# 混进来很难在 review 时看出来 —— 所以让它在编译之前就构建失败。
#
# 判据极简：shared/ 下的源文件里不许出现 `business/readiness/` 或
# `business/measurement/`。方向反了只有这一种写法，所以够用。
Write-Host "==> checking dependency direction"
$sharedSources = @($layers["libs_business"] | Where-Object { $_ -like "src/business/shared/*" })
if ($sharedSources.Count -eq 0) {
    throw "依赖方向检查没抓到任何 shared/ 源文件，规则本身失效了"
}
$violations = @()
foreach ($source in $sharedSources) {
    $hits = Select-String -Path $source -Pattern 'business/(readiness|measurement)/'
    if ($hits) { $violations += "$source : $($hits.Line.Trim())" }
}
if ($violations.Count) {
    throw "shared/ must not depend on business logic:`n$($violations -join "`n")"
}
Write-Host "    shared/ 反向依赖检查通过（$($sharedSources.Count) 个文件）"

Write-Host "==> compiling static libraries"
$archives = @()
foreach ($name in $layers.Keys) {
    $archive = Join-Path $outDir "$name.a"
    $objects = @()
    foreach ($source in $layers[$name]) {
        # 协议与控制器常有同名源文件（laser.cpp/air_conditioner.cpp），
        # 对象名必须保留相对路径，否则后编的对象会覆盖前一个。
        $objectSuffix = ($source -replace '[\\/]', '.') -replace '\.cpp$', ''
        $object = Join-Path $outDir ("{0}.{1}.o" -f $name, $objectSuffix)
        Invoke-Checked "compiling $source" { & $Compiler @flags -c $source -o $object }
        $objects += $object
    }
    if (Test-Path $archive) { Remove-Item -Force $archive }
    Invoke-Checked "archiving $name" { & $ar rcs $archive @objects }
    $archives += $archive
    Write-Host "    $name.a"
}
# 静态库按依赖顺序给链接器：business -> devices -> transport
$linkOrder = @(
    (Join-Path $outDir "libs_devices.a"),
    (Join-Path $outDir "libs_transport.a"))
$businessOrder = @((Join-Path $outDir "libs_business.a")) + $linkOrder

# FrameQueue 是零**设备**依赖，但仍然要 json 与时间工具 —— 这两个纯函数文件
# 不含任何设备符号。只链它们，就能让链接器本身守住"没有设备依赖"这条纪律：
# 哪天 FrameQueue 里不小心引了激光器或光谱仪的符号，链接就会失败。
$jsonObject  = Join-Path $outDir "libs_business.src.business.shared.json.json.o"
$timeObject  = Join-Path $outDir "libs_business.src.business.shared.time_util.o"
$frameObject = Join-Path $outDir "libs_business.src.business.measurement.frame_bus.o"
# frame.o 只含 SpectrumFrame/ChannelFrame 的结构与序列化，零设备依赖 ——
# 与 frame_bus.o 一起链进来，同样是为了让链接器守住这条纪律。
$frameStructObject = Join-Path $outDir "libs_business.src.business.measurement.frame.o"
$frameQueueOrder = @($frameObject, $frameStructObject, $jsonObject, $timeObject) + $linkOrder

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
    "test_serial_timing" = @("tests/test_serial_timing.cpp", $linkOrder)
    "test_protocols"    = @("tests/test_protocols.cpp",    $linkOrder)
    "test_devices"      = @("tests/test_devices.cpp",      $linkOrder)
    "test_air_conditioner_protocol" = @("tests/test_air_conditioner_protocol.cpp", $linkOrder)
    "test_air_conditioner_device"   = @("tests/test_air_conditioner_device.cpp",   $linkOrder)
    "test_device_actions"           = @("tests/test_device_actions.cpp",           $linkOrder)
    "test_spectrometer" = @("tests/test_spectrometer.cpp", $linkOrder)
    "test_business_core"     = @("tests/test_business_core.cpp",     $businessOrder)
    "test_business_services" = @("tests/test_business_services.cpp", $businessOrder)
    "test_business_facade"   = @("tests/test_business_facade.cpp",   $businessOrder)
    # IPC 测试直接调用 handle_line，不真的建管道，所以不拖 main.cpp。
    "test_business_ipc"      = @("tests/test_business_ipc.cpp", $ipcOrder)
    # 帧队列零设备依赖：故意**不链** business 库，让 build 强制保证这一点。
    "test_frame_queue"       = @("tests/test_frame_queue.cpp",      $frameQueueOrder)
    "test_measurement_job"   = @("tests/test_measurement_job.cpp",  $businessOrder)
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
    if ($name -eq "test_measurement_job") { $env:LIBS_FAKE_AVASPEC_DLL = $fakeDll }
    if ($name -eq "test_spectrometer") { & $exe $fakeDll } else { & $exe }
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}

if ($failed.Count) {
    Write-Error ("failed: " + ($failed -join ", "))
    exit 1
}
Write-Host "==> all tests passed"




