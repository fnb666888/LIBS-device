#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""libs 业务层薄客户端（`remote/lpc.py`）。

范围报告 §5.2 裁定的进程模型：**C++ 守护进程独占设备，客户端只发意图**。
所以这个文件非常薄——它不碰串口、不碰 SDK、不做判定，只做两件事：

  1. 把一行 JSON 请求写进命名管道，读回一行 JSON 响应；
  2. 少数**纯离线**子命令（例如 `config validate`）根本不需要管道。

薄到能一眼看完是有意的：客户端一旦开始自己判"设备是不是安全"，
判定权就从业务层漏到了 UI 层，任务书 §1 的分层纪律随之失效。

退出码约定（供脚本判断，不靠解析输出）：
  0  业务成功
  1  业务失败（ok=false，看响应里的 error）
  2  用法错误
  3  连不上守护进程 / 传输失败
"""

from __future__ import annotations

import argparse
import json
import os
import sys

# Windows 命名管道默认走 \\.\pipe\<name>。也允许环境变量覆盖，便于多实例联调。
DEFAULT_PIPE = os.environ.get("LIBS_BUSINESSD_PIPE") or r"\\.\pipe\libs-businessd"

# 需要连守护进程的命令。config validate / config template / commands 离线可跑。
OFFLINE_COMMANDS = {"config_validate", "config_template", "commands", "help"}


class TransportError(RuntimeError):
    """连不上管道、读写中断——与业务失败区分开，退出码不同。"""


def open_pipe(pipe_name: str, timeout_ms: int = 5000):
    """打开命名管道并返回 (读句柄, 写句柄)。

    用 ctypes 走 kernel32 而不是 pywin32：CLI 要在没有额外依赖的现场机器上能跑。
    超时是**连接**超时。守护进程那边一个连接一次只处理一条请求再回，
    多条请求按 JSON Lines 连发即可（见 README）。
    """
    if os.name != "nt":
        raise TransportError("命名管道是 Windows 设施；在其他平台上没有可用的守护进程通道")

    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    # FILE_FLAG_OVERLAPPED 不开：同步读写，代码直白且够用。
    FILE_ATTRIBUTE_NORMAL = 0x80

    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    handle = kernel32.CreateFileW(
        pipe_name, GENERIC_READ | GENERIC_WRITE, 0, None,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None,
    )
    if handle == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        # 2 = 文件找不到：最常见的是守护进程没起。这是启动顺序问题，不是业务失败。
        raise TransportError(
            "无法打开命名管道 %s（Win32 错误 %d）。守护进程是否已启动？" % (pipe_name, err)
        )
    return handle, kernel32, ctypes


def call_daemon(pipe_name: str, request: dict, timeout_ms: int = 5000) -> dict:
    """发一条请求，收一条响应。异常都在这一层收口。"""
    handle, kernel32, ctypes = open_pipe(pipe_name)

    from ctypes import wintypes

    kernel32.SetNamedPipeHandleState.restype = wintypes.BOOL
    # PIPE_READMODE_MESSAGE 会切分消息边界；这里显式改回 BYTE 模式，与服务端一致。
    kernel32.SetNamedPipeHandleState(
        handle, ctypes.byref(wintypes.DWORD(0)), None, None
    )

    payload = (json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8")

    # 声明 argtypes 是必须的，不是风格问题：不声明时 ctypes 按 c_int 传参，
    # 64 位下句柄和缓冲区指针会被截断，行为取决于运气。缓冲区用
    # (c_ubyte * n) 数组而不是 bytes/bytearray —— byref() 只接受 ctypes 数组。
    kernel32.WriteFile.restype = wintypes.BOOL
    kernel32.WriteFile.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
    ]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.ReadFile.argtypes = kernel32.WriteFile.argtypes

    out_buf = (ctypes.c_ubyte * len(payload)).from_buffer_copy(payload)
    written = wintypes.DWORD(0)
    ok = kernel32.WriteFile(handle, out_buf, len(payload), ctypes.byref(written), None)
    if not ok:
        kernel32.CloseHandle(handle)
        raise TransportError("向守护进程写入请求失败（Win32 错误 %d）" % ctypes.get_last_error())
    if written.value != len(payload):
        kernel32.CloseHandle(handle)
        raise TransportError("请求只写出了 %d/%d 字节" % (written.value, len(payload)))

    # 守护进程一行一答，所以这里一直读到换行或管道关闭为止。
    buffer = bytearray()
    while b"\n" not in buffer:
        chunk = (ctypes.c_ubyte * 4096)()
        got = wintypes.DWORD(0)
        ok = kernel32.ReadFile(handle, chunk, 4096, ctypes.byref(got), None)
        if not ok:
            err = ctypes.get_last_error()
            kernel32.CloseHandle(handle)
            # 236/109 = 管道在等待中被关闭：服务端崩了或已退出。
            raise TransportError("读取响应失败（Win32 错误 %d）" % err)
        if got.value == 0:
            break
        buffer.extend(bytes(chunk[: got.value]))
        if len(buffer) > 8 * 1024 * 1024:
            kernel32.CloseHandle(handle)
            raise TransportError("响应超过 8MB，判定为异常并断开")

    kernel32.CloseHandle(handle)

    line = bytes(buffer).split(b"\n", 1)[0]
    if not line:
        raise TransportError("守护进程关闭了连接但没有返回任何内容")
    try:
        return json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise TransportError("响应不是一行合法 JSON：%s" % exc)


def emit(obj, pretty: bool) -> None:
    """统一输出。默认压成一行，便于管道串联；--pretty 给人和日志看。"""
    text = json.dumps(obj, ensure_ascii=False, indent=2 if pretty else None,
                      sort_keys=not pretty)
    print(text)


def run_offline(args) -> int:
    """不需要守护进程就能跑的部分。"""
    if args.offline_command == "help":
        print(__doc__.strip())
        return 0

    if args.offline_command == "commands":
        emit({
            "offline": [
                "config validate", "config template", "commands", "help",
            ],
            "note": "在线命令需连接守护进程，去掉 --offline 即可",
        }, args.pretty)
        return 0

    if args.offline_command == "config_template":
        if not args.config:
            print("config template 需要 --config <file>（指定输出路径）", file=sys.stderr)
            return 2
        try:
            with open(args.config, "w", encoding="utf-8") as fh:
                fh.write(DEFAULT_CONFIG_TEMPLATE)
        except OSError as exc:
            print("写模板失败：%s" % exc, file=sys.stderr)
            return 1
        emit({"written": args.config}, args.pretty)
        return 0

    if args.offline_command == "config_validate":
        if not args.config:
            print("config validate 需要 --config <file>", file=sys.stderr)
            return 2
        try:
            with open(args.config, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError as exc:
            emit({"valid": False, "stage": "read", "error": str(exc)}, args.pretty)
            return 1
        try:
            value = json.loads(text)
        except json.JSONDecodeError as exc:
            emit({"valid": False, "stage": "parse", "error": exc.msg,
                  "line": exc.lineno, "column": exc.colno}, args.pretty)
            return 1

        problems = structural_check(value)
        if problems:
            emit({"valid": False, "stage": "structure", "problems": problems}, args.pretty)
            return 1
        emit({
            "valid": True,
            "stage": "structure",
            "note": "只做了结构检查。要做全量校验（含跨字段一致性），"
                    "请在守护进程上跑 config validate。",
        }, args.pretty)
        return 0

    return 2


def structural_check(value) -> list:
    """离线结构检查：只查**看得见的**问题，不猜设备语义。

    刻意保持保守：它不能替代 C++ 的 config::validate，只是让现场在没有守护进程时
    也能抓出最常见的拼写/结构错误。查不出来的，明确说查不出来。
    """
    problems = []
    if not isinstance(value, dict):
        return ["顶层必须是 JSON 对象"]

    for key in ("station_id",):
        if key in value and not isinstance(value[key], str):
            problems.append("%s 必须是字符串" % key)

    for key in ("laser", "rangefinder", "indicator", "air_conditioner"):
        port = value.get(key)
        if port is None:
            continue
        if not isinstance(port, dict):
            problems.append("%s 必须是对象" % key)
            continue
        if "port" in port and not isinstance(port["port"], str):
            problems.append("%s.port 必须是字符串（如 \"COM10\"）" % key)
        for numeric in ("baud", "timeout_ms", "idle_ms", "data_bits"):
            if numeric in port and not isinstance(port[numeric], int):
                problems.append("%s.%s 必须是整数" % (key, numeric))

    spectrometer = value.get("spectrometer")
    if spectrometer is not None:
        if not isinstance(spectrometer, dict):
            problems.append("spectrometer 必须是对象")
        else:
            serials = spectrometer.get("serials")
            if serials is not None and not (
                isinstance(serials, list) and all(isinstance(s, str) for s in serials)
            ):
                problems.append("spectrometer.serials 必须是字符串数组")
            if "synchronized" in spectrometer and not isinstance(
                spectrometer["synchronized"], bool
            ):
                problems.append("spectrometer.synchronized 必须是布尔值")

    interlock = value.get("ac_interlock")
    if isinstance(interlock, dict):
        # 这一条是 C 类联锁的核心纪律：启用了却没标定阈值，是配置自相矛盾。
        if interlock.get("enabled") and not interlock.get("calibrated"):
            problems.append(
                "ac_interlock.enabled 为真但 calibrated 为假：阈值未标定不得启用联锁"
            )
        if "trigger_indoor_temp_c" in interlock and not isinstance(
            interlock["trigger_indoor_temp_c"], (int, float)
        ):
            problems.append("ac_interlock.trigger_indoor_temp_c 必须是数字")

    return problems


# 与 C++ config::default_template() 的输出逐字一致。
# 两边必须同步：Python 侧写出一份守护进程拒绝的配置，比没有模板更糟。
# 全设备 enabled=false —— 模板的职责是给出一个**能通过校验**的起点，
# 不是替用户猜串口号。真实串口与 SDK 路径必须由现场填写。
DEFAULT_CONFIG_TEMPLATE = """{
  "ac_interlock": {
    "calibrated": false,
    "enabled": false,
    "trigger_indoor_temp_c": 0
  },
  "air_conditioner": {
    "baud": 9600,
    "data_bits": 8,
    "enabled": false,
    "idle_ms": 80,
    "parity": "none",
    "port": "",
    "timeout_ms": 650
  },
  "air_conditioner_address": 1,
  "capture_timeout_ms": 3000,
  "conveyor": {
    "running_assumed": true,
    "software_controlled": false
  },
  "gateway": {
    "candidate_adapter": "",
    "candidate_ip": "",
    "enabled": false,
    "observed_ip": "",
    "port": 0
  },
  "indicator": {
    "baud": 9600,
    "data_bits": 8,
    "enabled": false,
    "idle_ms": 80,
    "parity": "none",
    "port": "",
    "timeout_ms": 650
  },
  "indicator_address": 1,
  "indicator_model": "",
  "integration_ms": 10,
  "laser": {
    "baud": 9600,
    "data_bits": 8,
    "enabled": false,
    "idle_ms": 80,
    "parity": "none",
    "port": "",
    "timeout_ms": 650
  },
  "laser_address": 1,
  "rangefinder": {
    "baud": 9600,
    "data_bits": 8,
    "enabled": false,
    "idle_ms": 80,
    "parity": "none",
    "port": "",
    "timeout_ms": 650
  },
  "rangefinder_address": 1,
  "spectrometer": {
    "enabled": false,
    "master_serial": "",
    "sdk_dll": "",
    "serials": [],
    "synchronized": false
  },
  "station_id": "libs-station-01"
}
"""


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="lpc.py",
        description="libs 业务层薄客户端（连 businessd 守护进程）",
    )
    parser.add_argument("--pipe", default=DEFAULT_PIPE, help="命名管道名")
    parser.add_argument("--config", help="配置文件路径（config 子命令用）")
    parser.add_argument("--pretty", action="store_true", help="缩进输出")
    parser.add_argument("--offline", action="store_true",
                        help="不连守护进程，只跑离线子命令")
    parser.add_argument("offline_command", nargs="?", default="help",
                        choices=["help", "commands", "config"])
    parser.add_argument("offline_action", nargs="?", default="",
                        choices=["", "validate", "template"])

    subparsers = {}
    for name in ("ping", "status", "gate", "config_show", "health",
                 "detection_start", "capture", "laser_fire", "composition",
                 "recovery_list", "recovery_write", "param_set", "param_get"):
        p = subparsers.setdefault(name, argparse.ArgumentParser(prog="lpc.py " + name))
        p.add_argument("--args", default="{}", help="JSON 参数字符串")

    # 手工挑子命令，避免 argparse 在 Windows 控制台下的中文 help 编码问题。
    known = set(subparsers) | {"help", "commands", "config"}
    if not argv:
        argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    if argv[0] not in known:
        print("未知命令：%s（用 `lpc.py commands` 看清单）" % argv[0], file=sys.stderr)
        return 2

    if argv[0] == "config":
        if len(argv) < 2 or argv[1] not in ("validate", "template"):
            print("用法：lpc.py config validate|template --config <file>", file=sys.stderr)
            return 2
        args = parser.parse_args(args=argv[2:])
        args.offline_command = "config_" + argv[1]
        return run_offline(args)

    if argv[0] in ("help", "commands"):
        args = parser.parse_args(args=[])
        args.offline_command = argv[0]
        return run_offline(args)

    # ---- 以下都需要守护进程 ----
    rest = argv[1:]
    config_path = None
    pretty = False
    pipe_name = DEFAULT_PIPE
    params = {}
    i = 0
    while i < len(rest):
        token = rest[i]
        if token == "--pipe" and i + 1 < len(rest):
            pipe_name = rest[i + 1]; i += 2
        elif token == "--config" and i + 1 < len(rest):
            config_path = rest[i + 1]; i += 2
        elif token == "--pretty":
            pretty = True; i += 1
        elif token == "--args" and i + 1 < len(rest):
            try:
                params = json.loads(rest[i + 1])
            except json.JSONDecodeError as exc:
                print("--args 不是合法 JSON：%s" % exc, file=sys.stderr)
                return 2
            i += 2
        else:
            # 允许 `--key value` 或 `--key=value` 形式的便捷参数。
            if token.startswith("--") and "=" in token:
                key, _, raw = token[2:].partition("=")
                try:
                    params[key.replace("-", "_")] = json.loads(raw)
                except json.JSONDecodeError:
                    params[key.replace("-", "_")] = raw
                i += 1
            else:
                print("无法识别的参数：%s" % token, file=sys.stderr)
                return 2

    if config_path:
        # 有些命令要读配置副本。守护进程才是权威，这里只是把它带给客户端本地判断用。
        try:
            with open(config_path, "r", encoding="utf-8") as fh:
                params.setdefault("_client_config_path", config_path)
        except OSError as exc:
            print("读配置失败：%s" % exc, file=sys.stderr)
            return 1

    try:
        response = call_daemon(pipe_name, {"command": argv[0], "args": params})
    except TransportError as exc:
        print("传输失败：%s" % exc, file=sys.stderr)
        return 3

    emit(response, pretty)
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
