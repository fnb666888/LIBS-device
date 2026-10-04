<#
.SYNOPSIS
    编译实时光谱 UI（Qt6 Widgets + Qt6 Charts）。不需要 CMake，一条命令搞定。

.DESCRIPTION
    与 src/build.ps1 同一个风格：直接调 g++，不挑生成器、不挑工程路径。
    工程在 Windows 上、目标机是现场触摸屏，配置环境的成本要压到最低。

    依赖：
      - g++（MinGW）。默认取 PATH 里的 g++；也可以 -Compiler 指定绝对路径。
      - Qt6 的 mingw_64 发行版（Widgets + Charts）。默认猜 D:\Qt6\6.11.2\mingw_64，
        可用 -QtPrefix 覆盖。**必须 MinGW 的 Qt**，不能混 MSVC。

    产物：
      build\libs-spectrum-ui.exe
      build\qt\  运行时需要的 Qt6 DLL + 平台插件（windeployqt 拷的）

    这个 target 刻意**不链接** libs_devices / libs_business：UI 只通过命名管道
    上的 JSON 与守护进程说话。理由见 CMakeLists.txt 顶部注释。

.EXAMPLE
    pwsh -File build.ps1
    pwsh -File build.ps1 -QtPrefix D:\Qt6\6.11.2\mingw_64
#>
[CmdletBinding()]
param(
    [string]$Compiler = "g++",
    [string]$QtPrefix = "",
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# ── 找 Qt ────────────────────────────────────────────────────────────────
# 猜一个：环境变量 → D:\Qt6 下最新的 mingw_64 → 编译报错让人自己指。
if (-not $QtPrefix) {
    $envQt = $env:LIBS_QT_PREFIX
    if ($envQt) {
        $QtPrefix = $envQt
    } else {
        # @() 是必须的：管道只剩一个结果时 $candidates 会退化成 string，
        # 于是 $candidates[0] 取到的是首字符 "D" 而不是路径。踩过一次，记在这里。
        $candidates = @(Get-ChildItem "D:\Qt6" -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "^\d+\.\d+\.\d+$" } |
            ForEach-Object { Join-Path $_.FullName "mingw_64" } |
            Where-Object { Test-Path (Join-Path (Join-Path $_ "include") "QtCharts/QChart") } |
            Sort-Object -Descending)
        if ($candidates.Count -gt 0) { $QtPrefix = $candidates[0] }
    }
}
if (-not $QtPrefix -or -not (Test-Path (Join-Path $QtPrefix "include\QtCharts\QChart"))) {
    throw "找不到 Qt6（mingw_64，含 Widgets + Charts）。用 -QtPrefix <路径> 指定，" +
          "例如 -QtPrefix D:\Qt6\6.11.2\mingw_64"
}
Write-Host "Qt6 前缀: $QtPrefix"

$outDir = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ── 编译 ────────────────────────────────────────────────────────────────
$compilerPath = (Get-Command $Compiler -ErrorAction SilentlyContinue)
if (-not $compilerPath) { throw "找不到编译器 '$Compiler'。请把 MinGW 的 g++ 目录加进 PATH，或用 -Compiler 指定绝对路径。" }
$compilerDir = Split-Path -Parent $compilerPath.Source

$sources = @(
    "src/main.cpp",
    "src/main_window.cpp",
  "src/spectrum_plot.cpp",
    "src/theme.cpp",
    "src/widgets.cpp",
    "src/machine_state.cpp",
    "src/status_poller.cpp",
    "src/stream_poller.cpp",
    "src/frame_model.cpp",
    "src/pipe_client.cpp"
)

# moc 由 CMake 的 AUTOMOC 自动跑；这里手搓等价物——直接调 moc.exe 预生成 moc_*.cpp，
# 否则 Q_OBJECT / signals-slots 不会工作（表现是运行期 "No meta object"）。
$moc = Join-Path $QtPrefix "bin\moc.exe"
if (-not (Test-Path $moc)) { throw "找不到 moc.exe（$moc）" }

$generated = Join-Path $outDir "moc"
New-Item -ItemType Directory -Force -Path $generated | Out-Null
# Qt 6.8 的 moc 在 Windows 上对包含中文字符的绝对输出路径不稳定（会
# 报“Cannot create ... No such file or directory”，即使目录实际存在）。
# 脚本已经 Set-Location 到 ui 根目录，因此给 moc 相对路径，生成的文件
# 后面再由编译循环解析成绝对路径；这样现场把仓库放在中文目录也能构建。
$generatedRelative = "build\moc"

$generatedSources = @()
foreach ($src in $sources) {
    # src/xxx.cpp → src/xxx.hpp。不能对 "src/xxx.cpp" 用
    # [IO.Path]::ChangeExtension($src, $null)：传 $null 会得到 "src/xxx."（带个尾巴的点），
    # 接上 .hpp 就是 "src/xxx..hpp"，永远 Test-Path 不到，于是 moc 一个都不生成，
    # 链接时才炸出一堆 staticMetaObject / vtable 未定义。踩过一次，写在这里。
    $stem = [IO.Path]::GetFileNameWithoutExtension($src)
    $headerRelative = Join-Path "src" ($stem + ".hpp")
    $header = Join-Path $PSScriptRoot $headerRelative
    if (-not (Test-Path $header)) { continue }   # 例如 main.cpp 没有对应头文件
    $text = [IO.File]::ReadAllText($header)
    if ($text -notmatch "Q_OBJECT") { continue }
    $out = Join-Path $generatedRelative ($stem + ".moc.cpp")
    Write-Host "MOC  $stem"
    & $moc $headerRelative -o $out
    if ($LASTEXITCODE -ne 0) { throw "moc 失败: $header" }
    $generatedSources += $out
}
if ($generatedSources.Count -eq 0) {
    throw "一个 moc 文件都没生成——Q_OBJECT 的类不会有关于 meta object 的代码，链接必失败。"
}

# windeployqt 也要在 PATH（它调用同目录的其它 exe）。
$env:PATH = (Join-Path $QtPrefix "bin") + ";" + $compilerDir + ";" + $env:PATH

$includeFlags = @("-I$QtPrefix\include", "-I$QtPrefix\include\QtCore", "-I$QtPrefix\include\QtGui",
                  "-I$QtPrefix\include\QtWidgets", "-I$QtPrefix\include\QtCharts", "-Isrc")

$libFlags = @("-L$QtPrefix\bin", "-lQt6Charts", "-lQt6Widgets", "-lQt6Gui", "-lQt6Core",
              "-lmingw32", "-luserenv", "-lAdvapi32", "-lShell32", "-limm32", "-lwinmm", "-lopengl32")

$objDir = Join-Path $outDir "obj"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$all = $sources + $generatedSources
$objects = @()
$failed = $false
foreach ($src in $all) {
    $abs = if ([IO.Path]::IsPathRooted($src)) { $src } else { Join-Path $PSScriptRoot $src }
    if (-not (Test-Path $abs)) { $abs = Join-Path $PSScriptRoot ($src -replace '/', '\') }
    $obj = Join-Path $objDir (([IO.Path]::GetFileNameWithoutExtension($src)) + ".o")
    Write-Host "CXX $src"
    & $Compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic -municode @includeFlags -c $abs -o $obj
    if ($LASTEXITCODE -ne 0) { $failed = $true; break }
    $objects += $obj
}
if ($failed) { throw "编译失败" }

$exe = Join-Path $outDir "libs-spectrum-ui.exe"
Write-Host "LINK libs-spectrum-ui.exe"
# -mwindows：链接成 **GUI 子系统**程序。
# 不加的话它是控制台子系统程序，Windows 启动时会先分配一个控制台窗口 ——
# 现场双击 exe 会多弹一个黑框（像终端），既吓人也显得不专业。
# 其它应用不会这样，就是因为它们的链接行里有这一条。
& $Compiler @objects @libFlags -mwindows -o $exe
if ($LASTEXITCODE -ne 0) { throw "链接失败" }
Write-Host "OK $exe"

# ── 部署运行时（拷贝部署，见 docs §10.5）────────────────────────────────
if (-not $SkipDeploy) {
    $deployDir = Join-Path $outDir "qt"
    New-Item -ItemType Directory -Force -Path $deployDir | Out-Null
    $windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"
    if (Test-Path $windeployqt) {
        Write-Host "windeployqt → $deployDir"
        & $windeployqt --release --no-translations --no-system-d3d-compiler `
            --dir $deployDir $exe
        if ($LASTEXITCODE -ne 0) { Write-Warning "windeployqt 失败，Qt DLL 可能不全（现场运行会缺库）" }
    } else {
        Write-Warning "找不到 windeployqt.exe，跳过 Qt DLL 拷贝。现场运行需自备 Qt 运行时。"
    }
}

