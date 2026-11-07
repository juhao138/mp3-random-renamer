#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mp3_random_renamer.py —— 监测指定文件夹里的 mp3，在原名前面加上不重复的随机数字

示例：周杰伦 - 稻香.mp3  ->  708368_周杰伦 - 稻香.mp3   （后缀原样保留）

用法示例
--------
    python mp3_random_renamer.py "D:\\Music"
    python mp3_random_renamer.py "D:\\Music" --interval 2
    python mp3_random_renamer.py "D:\\Music" --once
    python mp3_random_renamer.py "D:\\Music" --sep "" --digits 6
    python mp3_random_renamer.py "D:\\Music" --mode replace      # 只留数字、丢掉原名
    python mp3_random_renamer.py "D:\\Music" --recursive --dry-run

特点
----
* 默认“加前缀”：保留原文件名，只在前头加随机数字，中间默认用下划线分隔（--sep 可改）
* 只动主文件名，后缀（含 .mp3 / .MP3 的大小写）原样保留
* 随机数字互不重复：进程内用集合记录，同时写入状态文件，重启脚本也不会撞号
* 已经加过前缀的文件不会再被反复加前缀（状态文件记录了生成过的文件名）
* 纯标准库实现（轮询监测），无需安装 watchdog
* 正在被占用 / 被其它程序锁住的文件会重试，失败只记日志不中断
* --once 只跑一遍；--dry-run 只打印不落盘

状态文件
--------
默认在监测目录下生成隐藏文件 .mp3_random_renamer_state.json，记录
“用过的随机数字”和“生成过的文件名”。删掉它不影响音频文件本身，
只是重启后无法再认出哪些文件名是本脚本生成的（那些文件会被再加一次前缀）。
可用 --no-state 完全禁用，或用 --state 指定别的位置。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import stat
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Callable, Iterator

MP3_SUFFIX = ".mp3"
STATE_NAME = ".mp3_random_renamer_state.json"
DEFAULT_DIGITS = 9          # 随机数字的位数，默认 9 位（1 ~ 999999999）
DEFAULT_INTERVAL = 1.0      # 轮询间隔（秒）
DEFAULT_SEP = "_"           # 数字和原名之间的分隔符
RENAME_RETRIES = 6          # 文件被占用时的重试次数
MAX_NAME_LEN = 255          # 单个文件名长度上限（NTFS）
MIN_LEGACY_DIGITS = 6       # 恢复原名时，没记录的老前缀至少要这么长才敢摘
FINGERPRINT_WINDOW = 512 * 1024   # 内容指纹只读首尾各 512KB


# --------------------------------------------------------------------------- #
# 输出
# --------------------------------------------------------------------------- #
def _tolerate_console_encoding() -> None:
    """输出被重定向时，避免生僻字符（如 emoji 文件名）把脚本搞崩。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, OSError, ValueError):
            pass


def log(msg: str, level: str = "info") -> None:
    _emit(level, msg)


def warn(msg: str) -> None:
    _emit("warn", msg)


# 日志出口：默认打印到控制台；GUI 可以通过 set_log_sink() 把日志接走。
LogSink = Callable[[str, str], None]
_LOG_SINK: "LogSink | None" = None


def set_log_sink(sink: "LogSink | None") -> None:
    """把日志交给外部处理（level, text）；传 None 恢复打印到控制台。"""
    global _LOG_SINK
    _LOG_SINK = sink


def _emit(level: str, msg: str) -> None:
    sink = _LOG_SINK
    if sink is not None:
        try:
            sink(level, msg)
            return
        except Exception:       # GUI 已经关掉了之类，降级为打印
            pass
    stamp = f"[{datetime.now():%H:%M:%S}]"
    if level == "warn":
        print(f"{stamp} [警告] {msg}", file=sys.stderr, flush=True)
    else:
        print(f"{stamp} {msg}", flush=True)


# 记录已经警告过的文件（按“大小 + 修改时间”判断是否是新情况），
# 避免同一个被占用的文件每轮轮询都刷一条警告。
_WARNED: dict[str, tuple[int, int]] = {}

# 试运行模式下已经预告过的文件，避免每轮轮询重复打印同一条
_PREVIEWED: set[str] = set()

# 本次运行里刚刚改出来的文件名，防止同一轮/下一轮又被当成“已改过名的”重新处理
_PROCESSED: set[str] = set()


def _warn_once(path: Path, msg: str) -> None:
    try:
        st = path.stat()
        sig = (st.st_size, st.st_mtime_ns)
    except OSError:
        sig = (-1, -1)
    key = str(path)
    if _WARNED.get(key) == sig:
        return
    _WARNED[key] = sig
    warn(f"{msg}：{path.name}")


# --------------------------------------------------------------------------- #
# 状态文件：记录用过的随机数字 + 生成过的文件名
# --------------------------------------------------------------------------- #
class State:
    """持久化状态：用过的随机数字、生成过的文件名、“新名 → 原名”映射、内容指纹。"""

    def __init__(self, path: Path | None) -> None:
        self.path = path
        self.used: set[int] = set()             # 已经用过的随机数字
        self.names: set[str] = set()            # 本脚本生成过的完整文件名
        self.orig: dict[str, str] = {}          # 生成的名字 -> 原始文件名（按名字恢复）
        self.fps: dict[str, str] = {}           # 内容指纹 -> 原始文件名（按内容恢复）
        self._load()

    def _load(self) -> None:
        if not self.path or not self.path.is_file():
            return
        try:
            # utf-8-sig：记事本等编辑器会把 UTF-8 文件加上 BOM，别因此把状态整个丢掉
            data = json.loads(self.path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as exc:
            warn(f"状态文件无法读取，将当作空状态继续：{exc}")
            return
        if not isinstance(data, dict):
            return
        try:
            self.used = {int(x) for x in data.get("used", [])}
            self.names = {str(x) for x in data.get("names", [])}
            raw_orig = data.get("orig", {})
            if isinstance(raw_orig, dict):
                self.orig = {str(k): str(v) for k, v in raw_orig.items()}
            raw_fps = data.get("fps", {})
            if isinstance(raw_fps, dict):
                self.fps = {str(k): str(v) for k, v in raw_fps.items()}
        except (TypeError, ValueError) as exc:
            warn(f"状态文件内容异常，已忽略：{exc}")

    def save(self) -> None:
        """原子写入，避免断电/中断留下半个文件。"""
        if not self.path:
            return
        tmp = self.path.parent / (self.path.name + ".tmp")
        payload = {
            "used": sorted(self.used),
            "names": sorted(self.names),
            "orig": self.orig,
            "fps": self.fps,
        }
        try:
            tmp.write_text(
                json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
                encoding="utf-8",
            )
            os.replace(tmp, self.path)
        except OSError as exc:
            warn(f"状态文件写入失败：{exc}")

    def snapshot(self) -> tuple:
        return (
            frozenset(self.used),
            frozenset(self.names),
            frozenset(self.orig.items()),
            frozenset(self.fps.items()),
        )


# --------------------------------------------------------------------------- #
# 找文件
# --------------------------------------------------------------------------- #
def iter_mp3_files(root: Path, recursive: bool) -> Iterator[Path]:
    """遍历目录下后缀为 .mp3 的普通文件（大小写不敏感）。"""
    try:
        entries = root.rglob("*") if recursive else root.iterdir()
        for p in entries:
            try:
                if p.suffix.lower() != MP3_SUFFIX:
                    continue
                if p.is_file():
                    yield p
            except OSError:
                continue
    except OSError as exc:
        warn(f"无法遍历目录 {root}：{exc}")


# --------------------------------------------------------------------------- #
# 随机数字
# --------------------------------------------------------------------------- #
def draw_number(used: set[int], digits: int) -> int:
    """抽一个不在 used 里的 digits 位随机数字。"""
    low = 0 if digits <= 1 else 10 ** (digits - 1)
    high = 10 ** digits - 1
    span = high - low + 1
    if len(used) >= span:
        raise RuntimeError(f"{digits} 位随机数字已经用完，请加大 --digits")

    for _ in range(min(5000, span)):
        n = random.randint(low, high)
        if n not in used:
            return n

    # 数字空间快用尽时，顺序兜底，保证一定能取到空号
    for n in range(low, high + 1):
        if n not in used:
            return n
    raise RuntimeError(f"{digits} 位随机数字已经用完，请加大 --digits")


def new_name(base: str, n: int, suffix: str, mode: str, sep: str) -> str:
    """拼出新文件名：加前缀模式保留原名，替换模式只留数字。"""
    if mode == "replace" or not base:
        return f"{n}{suffix}"
    return f"{n}{sep}{base}{suffix}"


def split_own_prefix(stem: str, used: set[int], sep: str) -> tuple[str, int] | None:
    """stem 是本程序生成的“数字名”时，返回（原始主文件名, 用到的数字），否则 None。

    只有“数字确实是本程序发过的号”才认，避免误伤 `2024 演唱会` 这种自带数字的名字。
    """
    m = re.match(r"^(\d+)(.*)$", stem, re.ASCII)
    if not m:
        return None
    num, rest = m.group(1), m.group(2)
    n = int(num)
    if n not in used:
        return None
    if rest == "":                   # 只保留数字模式留下的纯数字名，原名已无从得知
        return "", n
    if sep and rest.startswith(sep):
        return rest[len(sep):], n
    if not sep:
        return rest, n
    return None


def build_target(
    src: Path, base: str, used: set[int], digits: int, mode: str, sep: str
) -> tuple[Path, int]:
    """为 src 挑一个未被使用、且磁盘上不存在的目标路径，返回（目标路径, 用到的数字）。"""
    for _ in range(500):
        n = draw_number(used, digits)
        target = src.with_name(new_name(base, n, src.suffix, mode, sep))
        if target == src:            # 抽到了自己原本的名字
            continue
        if target.exists():          # 磁盘上已有同名文件，换一个号
            continue
        used.add(n)                  # 先占坑，防止同一批里撞号
        return target, n
    raise RuntimeError(f"连续 500 次都抽不到可用编号：{src.name}")


# --------------------------------------------------------------------------- #
# 改名
# --------------------------------------------------------------------------- #
def check_writable(root: Path) -> str | None:
    """真刀真枪试一次“建文件 → 改名 → 删除”。

    返回 None 表示这个目录可以改名；否则返回失败原因。
    比只看权限位靠谱：沙箱、受控文件夹访问、只读盘都能提前发现。
    """
    name = f".mp3_renamer_write_test_{os.getpid()}"
    probe = root / f"{name}.tmp"
    moved = root / f"{name}.moved"
    try:
        probe.write_bytes(b"x")
        os.rename(probe, moved)
        os.remove(moved)
        return None
    except OSError as exc:
        for p in (probe, moved):
            try:
                p.unlink()
            except OSError:
                pass
        return exc.strerror or str(exc)


def is_readonly(path: Path) -> bool:
    try:
        return not (path.stat().st_mode & stat.S_IWRITE)
    except OSError:
        return False


def file_fingerprint(path: Path) -> str | None:
    """内容指纹：文件大小 + 首尾各 512KB 的 sha256，读失败返回 None。

    没用全量 SHA-256 是因为太慢：8MB 的歌全量算一次要 70ms 左右，
    上千首就是一分多钟；而只读首尾各 512KB 只要几毫秒，
    对“认出同一首歌”这个用途已经完全够（同大小且首尾都相同，基本只能是同一份音频）。
    """
    try:
        size = path.stat().st_size
    except OSError:
        return None

    h = hashlib.sha256()
    h.update(b"size:%d|" % size)
    try:
        with open(path, "rb") as fh:
            h.update(fh.read(FINGERPRINT_WINDOW))
            if size > FINGERPRINT_WINDOW:
                fh.seek(max(FINGERPRINT_WINDOW, size - FINGERPRINT_WINDOW))
                h.update(fh.read())
    except OSError:
        return None
    return h.hexdigest()


def safe_rename(src: Path, dst: Path) -> None:
    """改名；被占用/只读时短暂重试。"""
    last: OSError | None = None
    for i in range(RENAME_RETRIES):
        try:
            os.rename(src, dst)      # 目标已存在时 Windows 会抛 FileExistsError
            return
        except FileExistsError:
            raise
        except OSError as exc:       # Windows: 被占用 / 只读 / 拒绝访问
            last = exc
            if i == 0 and is_readonly(src):
                # 只读属性会挡住改名，第一次失败时试着去掉
                try:
                    os.chmod(src, stat.S_IWRITE | stat.S_IREAD)
                    continue
                except OSError:
                    pass
            if i < RENAME_RETRIES - 1:
                time.sleep(0.2 * (i + 1))
    assert last is not None
    raise last


def scan_once(
    root: Path,
    *,
    recursive: bool,
    digits: int,
    mode: str,
    sep: str,
    state: State,
    dry_run: bool,
    reprocess: bool = False,
) -> int:
    """扫一遍目录，改名，返回本次改名数量。

    reprocess=False（默认）：本程序改过的文件跳过，不会重复加前缀。
    reprocess=True ：把已改过名的文件摘掉旧数字、按当前设置重新编号。
    """
    changed = 0
    for src in iter_mp3_files(root, recursive):
        found = split_own_prefix(src.stem, state.used, sep)
        base = found[0] if found else src.stem
        if src.name in _PROCESSED:
            # 本次运行里刚改过的，别在同一批/下一轮里再改一遍
            continue
        if not reprocess and (src.name in state.names or found is not None):
            # 已经改过名的，别再动它（加上 found 是为了状态文件丢了也不叠加前缀）
            continue
        if dry_run and str(src) in _PREVIEWED:
            continue

        try:
            target, n = build_target(src, base, state.used, digits, mode, sep)
        except RuntimeError as exc:
            warn(str(exc))
            continue

        if len(target.name) > MAX_NAME_LEN:
            state.used.discard(n)
            _warn_once(src, f"新文件名超过 {MAX_NAME_LEN} 字符，跳过")
            continue

        if dry_run:
            _PREVIEWED.add(str(src))
            log(f"[试运行] {src.name}  ->  {target.name}", "preview")
            changed += 1
            continue

        # 改名前先算内容指纹：名字可能是别人改过的，但内容认得出来
        digest = file_fingerprint(src)
        known = state.fps.get(digest) if digest else None

        try:
            safe_rename(src, target)
        except FileNotFoundError:
            state.used.discard(n)
            _warn_once(src, "文件已消失，跳过")
            continue
        except FileExistsError:
            state.used.discard(n)
            _warn_once(src, f"目标名已被占用，跳过 {target.name}")
            continue
        except OSError as exc:
            state.used.discard(n)
            code = getattr(exc, "winerror", None)
            _warn_once(
                src,
                f"改名失败（{exc.strerror or exc}"
                + (f"，错误码 {code}" if code else "")
                + "），稍后重试",
            )
            continue

        _WARNED.pop(str(src), None)
        _PROCESSED.add(target.name)
        # 记下“新名 -> 原名”，恢复时才有据可依：
        #   hash 里已经记过原名（以前被我们改过）→ 用那个，哪怕中途被手动改过名
        if known:
            original = known
        elif found is not None:
            original = f"{base}{src.suffix}"
        else:
            original = state.orig.get(src.name, src.name)
        state.orig.pop(src.name, None)
        state.orig[target.name] = original
        if digest:
            state.fps[digest] = original
        state.names.discard(src.name)      # 旧名字已经不存在了
        state.names.add(target.name)
        changed += 1
        log(f"{src.name}  ->  {target.name}", "rename")

    return changed


def free_name(folder: Path, name: str) -> str:
    """name 已被占用时，加 (2)(3)… 找一个不冲突的名字。"""
    if not (folder / name).exists():
        return name
    stem, suffix = os.path.splitext(name)
    for i in range(2, 1000):
        candidate = f"{stem} ({i}){suffix}"
        if not (folder / candidate).exists():
            return candidate
    raise RuntimeError(f"找不到可用的文件名：{name}")


def restore_once(
    root: Path,
    *,
    recursive: bool,
    state: State,
    sep: str,
    dry_run: bool = False,
    force: bool = False,
) -> int:
    """把本程序改过名的 mp3 还原成原始文件名，返回恢复数量。

    认名字的优先级：
      1. 状态文件里记的“新名 → 原名”（最准）
      2. 内容 sha256 指纹（文件名被改过、或换成了纯数字名也能认出来）
      3. 前缀推断：数字必须是本程序发过的号，且位数不少于 MIN_LEGACY_DIGITS
         （避免把 `2024_演唱会` 这类自带的数字当前缀摘掉）
    """
    seps = [sep] if sep else []
    for s in ("_", "-", ""):
        if s not in seps:
            seps.append(s)

    changed = 0
    skipped: list[tuple[str, str]] = []
    for src in iter_mp3_files(root, recursive):
        original = state.orig.get(src.name, "")
        how = "记录"
        n: int | None = None
        if original:
            m = re.match(r"^(\d+)", src.stem, re.ASCII)
            n = int(m.group(1)) if m else None
        else:
            reason: str | None = None
            for s in seps:
                found = split_own_prefix(src.stem, state.used, s)
                if found is None:
                    continue
                base, num = found
                if not base:
                    reason = "纯数字名，且没有原名记录"
                elif not force and len(str(num)) < MIN_LEGACY_DIGITS:
                    reason = f"前缀 {num} 位数偏短，可能是歌名自带的数字，未恢复"
                else:
                    original = f"{base}{src.suffix}"
                    n = num
                break

            if not original:
                # 名字对不上，就按文件内容找回来的名字
                digest = file_fingerprint(src)
                if digest and digest in state.fps:
                    original = state.fps[digest]
                    how = "内容指纹"
                    m = re.match(r"^(\d+)", src.stem, re.ASCII)
                    n = int(m.group(1)) if m else None
                    reason = None

            if not original:
                if reason:
                    skipped.append((src.name, reason))
                continue

        if dry_run:
            log(f"[试运行] {src.name}  ->  {original}（按{how}）", "preview")
            changed += 1
            continue

        try:
            target = src.with_name(free_name(src.parent, original))
        except RuntimeError as exc:
            warn(str(exc))
            continue
        if target == src:
            continue

        try:
            safe_rename(src, target)
        except OSError as exc:
            warn(f"恢复失败 {src.name}：{exc.strerror or exc}")
            continue

        state.orig.pop(src.name, None)
        state.names.discard(src.name)
        if n is not None:
            state.used.discard(n)          # 号收回来，以后还能用
        changed += 1
        log(f"{src.name}  ->  {target.name}"
            + ("（按内容指纹匹配）" if how == "内容指纹" else ""), "restore")

    if skipped:
        log(f"有 {len(skipped)} 个文件没能恢复：", "warn")
        for name, why in skipped[:5]:
            log(f"    {name} —— {why}", "warn")
        if len(skipped) > 5:
            log(f"    ……还有 {len(skipped) - 5} 个同类情况", "warn")

    if not dry_run and changed:
        state.save()
    return changed


def run_loop(
    root: Path,
    *,
    recursive: bool,
    digits: int,
    mode: str,
    sep: str,
    state: State,
    dry_run: bool,
    interval: float = DEFAULT_INTERVAL,
    once: bool = False,
    stop_event: "threading.Event | None" = None,
    reprocess: bool = False,
) -> int:
    """扫描主循环，返回累计处理数量。

    once=True 只扫一遍；stop_event 被 set 后会在本遍扫描结束时退出，
    因此 GUI 点“停止”最多等一次扫描的时间。
    """
    total = 0
    _WARNED.clear()
    _PREVIEWED.clear()
    _PROCESSED.clear()
    problem = check_writable(root)
    if problem and not dry_run:
        warn(f"这个文件夹当前无法写入（{problem}）。改名一定会失败，可能原因："
             f"程序运行在受限环境/沙箱里、杀软“受控文件夹访问”、或该目录是只读的。"
             f"建议直接在资源管理器里双击 exe 运行再试。")
    if state.names and not reprocess:
        log(f"状态文件里记录了 {len(state.names)} 个已改过名的文件，默认跳过；"
            f"要换新编号请打开“重新处理”（命令行 --reprocess）")
    try:
        while True:
            before = state.snapshot()
            total += scan_once(
                root,
                recursive=recursive,
                digits=digits,
                mode=mode,
                sep=sep,
                state=state,
                dry_run=dry_run,
                reprocess=reprocess,
            )
            if not dry_run and state.snapshot() != before:
                state.save()

            if once:
                return total
            if stop_event is not None:
                if stop_event.wait(interval):
                    return total
            else:
                time.sleep(interval)
    except KeyboardInterrupt:
        return total


# --------------------------------------------------------------------------- #
# 命令行
# --------------------------------------------------------------------------- #
def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="监测指定文件夹下的 mp3 文件，在文件名前面加上不重复的随机数字（原名与后缀都保留）。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("path", nargs="?", help="要监测的文件夹路径；不填则运行时提示输入")
    parser.add_argument(
        "--mode", choices=("prefix", "replace"), default="prefix",
        help="prefix=数字加在原名前面；replace=只保留数字、丢掉原名",
    )
    parser.add_argument("--sep", default=DEFAULT_SEP, help="前缀数字与原名之间的分隔符，可设为空字符串")
    parser.add_argument(
        "--interval", type=float, default=DEFAULT_INTERVAL,
        help="轮询间隔（秒）",
    )
    parser.add_argument("--digits", type=int, default=DEFAULT_DIGITS, help="随机数字的位数")
    parser.add_argument("-r", "--recursive", action="store_true", help="递归处理子文件夹")
    parser.add_argument("--once", action="store_true", help="只扫描一次就退出，不持续监测")
    parser.add_argument("--dry-run", action="store_true", help="只打印，不真正改名")
    parser.add_argument("--state", default=None, help="状态文件路径")
    parser.add_argument("--no-state", action="store_true", help="不使用状态文件（不记录已用编号）")
    parser.add_argument(
        "--reprocess", action="store_true",
        help="重新处理已经改过名的文件：摘掉旧数字，按当前设置换一个新编号",
    )
    parser.add_argument(
        "--restore", action="store_true",
        help="恢复原名：把本程序改过名的文件还原成原始文件名（配 --dry-run 可预览）",
    )
    parser.add_argument(
        "--restore-any", action="store_true",
        help="配合 --restore：不校验前缀位数，只要是本程序发过的号就摘（老库慎用）",
    )
    args = parser.parse_args(argv)

    if not 1 <= args.digits <= 18:
        parser.error("--digits 必须是 1 ~ 18 之间的整数")
    if args.interval <= 0:
        parser.error("--interval 必须大于 0")
    return args


def ask_path() -> str:
    try:
        raw = input("请输入要监测的文件夹路径：")
    except (EOFError, KeyboardInterrupt):
        print()
        raise SystemExit(0)
    return raw.strip().strip("'\"")


def resolve_dir(raw: str) -> Path:
    path = Path(os.path.expandvars(raw)).expanduser()
    if not path.exists():
        raise SystemExit(f"路径不存在：{path}")
    if not path.is_dir():
        raise SystemExit(f"这不是文件夹：{path}")
    return path.resolve()


def main(argv: list[str] | None = None) -> int:
    _tolerate_console_encoding()
    args = parse_args(argv)

    raw = args.path if args.path else ask_path()
    if not raw:
        raise SystemExit("没有提供路径，已退出。")
    root = resolve_dir(raw)

    if args.no_state:
        state = State(None)
    else:
        state_path = Path(args.state).expanduser() if args.state else root / STATE_NAME
        state = State(state_path)

    rule = f"数字加在原名前（例如 123{args.sep}歌名.mp3）" if args.mode == "prefix" \
        else "只保留数字（例如 123.mp3）"
    log(f"监测目录：{root}")
    log(f"匹配规则：*{MP3_SUFFIX}（大小写不敏感）" + ("，含子文件夹" if args.recursive else "，不含子文件夹"))

    if args.restore:
        log("模式：恢复原名")
        if args.dry_run:
            log("试运行模式：只显示会改回什么，不动文件")
        if state.path:
            log(f"状态文件：{state.path}（原名记录 {len(state.orig)} 条）")
        total = restore_once(
            root,
            recursive=args.recursive,
            state=state,
            sep=args.sep,
            dry_run=args.dry_run,
            force=args.restore_any,
        )
        log(f"完成，共恢复 {total} 个文件。")
        return 0

    log(f"改名方式：{rule}；随机数字 {args.digits} 位")
    log("运行方式：" + ("只扫描一次" if args.once else f"每 {args.interval:g} 秒扫描一次"))
    if args.dry_run:
        log("试运行模式：不会真的改名")
    if args.reprocess:
        log("重新处理：已改过名的文件会摘掉旧数字、换新编号")
    if state.path:
        log(f"状态文件：{state.path}（已用编号 {len(state.used)} 个）")

    kwargs = dict(
        recursive=args.recursive,
        digits=args.digits,
        mode=args.mode,
        sep=args.sep,
        state=state,
        dry_run=args.dry_run,
        reprocess=args.reprocess,
    )

    if args.once:
        total = run_loop(root, once=True, **kwargs)
        log(f"完成，共处理 {total} 个文件。")
        return 0

    log("开始监测，按 Ctrl+C 停止。")
    total = run_loop(root, interval=args.interval, **kwargs)
    print()
    log(f"已停止，本次共处理 {total} 个文件。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as exc:  # 兜底，避免闪退看不到信息
        warn(f"未预期的错误：{exc!r}")
        sys.exit(1)
