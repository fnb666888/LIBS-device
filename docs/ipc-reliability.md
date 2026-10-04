# IPC 可靠性回归

前端和业务守护进程在同一台 Windows 工控机上通过
`\\.\pipe\libs-businessd` 传输 JSON Lines。前端的每一次状态、光谱或按钮请求都
可能是一个短连接，因此只测 `handle_line()` 不能证明线路在持续运行时稳定。

## 本次不稳定的根因

现场看到的“前后端断开”由几类问题叠加造成：

1. 服务端虽然为每条连接创建线程，但正常断开路径没有同时执行
   `DisconnectNamedPipe` 和 `CloseHandle`，完成的线程也一直留在容器里。短连接持续
   轮询时，管道句柄和线程对象会逐步累积，最终表现为创建管道失败或连接超时。
2. 服务端的 `stop()` 以前只修改停止标志；accept 线程可能卡在 `ConnectNamedPipe`，
   客户端线程可能卡在 `ReadFile`，所以重启或异常恢复时会留下旧进程和半开连接。
3. UI 客户端以前使用同步 `WriteFile`/`ReadFile`。传入的请求超时参数并不能限制这两个
   Win32 调用，后端或设备链路无响应时，状态轮询线程会永久占用，新的请求随后排队。
4. 后端实际返回的是顶层字符串 `error` 加 `error_code`/`error_kind`，UI 却只按
   `error` 对象解析，设备错误会被显示成 `unknown/(无消息)`，误导排障并触发重复重试。

现在服务端使用可被停止事件唤醒的 overlapped 管道，客户端使用有总时限的
overlapped 读写；所有连接的句柄由 `ClientState` 统一回收，UI 同时兼容两种错误报文。

## 真实线路测试

`tests/test_business_ipc.cpp` 除了离线协议用例，还会在当前进程内启动一个
`NamedPipeServer`，用 Win32 `CreateFileA`、`WriteFile`、`ReadFile` 做真实请求：

- 等待并唤醒 `ConnectNamedPipe`，验证 `stop()` 不会把测试线程永远卡住；
- 完成真实的 `ping` 请求和一行一响应的分帧检查；
- 8 个客户端线程各建立 8 次短连接，检查 accept 循环没有窗口导致的
  `ERROR_FILE_NOT_FOUND`/`ERROR_PIPE_BUSY` 误报；
- 停止服务后读取当前进程句柄数，允许少量 CRT/线程固定开销，但拒绝每条连接
  遗留一个管道句柄。

该测试使用默认全禁用设备配置，不会打开串口、激光器或光谱仪，也不会发出设备
控制指令。它要求 Windows（命名管道和 `GetProcessHandleCount`），已经挂在现有
`build.ps1` 的 `test_business_ipc.exe` 中：

```powershell
cd D:\桌面\固废设备\LIBS-device\src
$env:PATH = "C:\QtBuild\Qt\Tools\mingw1310_64\bin;$env:PATH"
pwsh -NoProfile -File .\build.ps1 `
  -Compiler 'C:\QtBuild\Qt\Tools\mingw1310_64\bin\g++.exe'
```

若只需重跑 IPC 用例：

```powershell
.\build\test_business_ipc.exe
```

## 现场长稳测试

离线回归只覆盖当前进程中的服务端和 API，不替代工控机验收。现场部署后，应在不
连接真实设备控制动作的前提下，持续运行状态查询和光谱预览，记录：

1. `businessd.exe` 的进程句柄数、工作集和线程数；
2. UI 请求延迟、失败类型及重试次数；
3. `ERROR_PIPE_BUSY`、`ERROR_FILE_NOT_FOUND` 和后端进程退出次数；
4. 每次短连接完成后句柄数是否回到稳定区间。

出现一次业务失败不能直接判定后端离线：应区分管道暂时繁忙、设备锁等待、设备
返回错误和进程确实退出。现场长稳测试不得自动调用 `laser_fire`、`laser_run` 或
电机运动命令。
