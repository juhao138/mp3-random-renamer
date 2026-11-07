#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mp3_renamer_gui.py —— mp3 随机数字改名器的图形界面

界面用 tkinter（Python 自带，不需要 pip 安装任何东西）。
与命令行版共用同一套核心逻辑（mp3_random_renamer.py），行为完全一致：

    周杰伦 - 稻香.mp3  ->  676006_周杰伦 - 稻香.mp3

启动方式
--------
    python mp3_renamer_gui.py       # 会带一个黑色控制台窗口
    pythonw mp3_renamer_gui.py      # 只显示界面，推荐

带参数启动（打包成 exe 后同样可用）：
    mp3_renamer_gui.py "D:\\Music" --start                 打开就自动开始监测这个文件夹
    mp3_renamer_gui.py "D:\\Music" --once                  打开只扫一遍，不常驻
    mp3_renamer_gui.py "D:\\Music" --once --reprocess      把已改过名的文件重新编号

界面上的按钮
------------
    开始监测   持续盯着文件夹，有新 mp3 就立刻改名（Ctrl+C 对应的就是“停止”）
    只执行一次 扫描一遍就结束
    停止       结束监测（当前这一遍扫完才停，通常不到 1 秒）
    清空日志   清屏

设置（路径、模式、位数、间隔等）会记住，下次打开自动填好。
"""

from __future__ import annotations

import json
import os
import queue
import sys
import tempfile
import threading
import tkinter as tk
import tkinter.font as tkfont
import traceback
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

def app_dir() -> Path:
    """程序所在目录。打包成 exe 后 __file__ 指向临时解包目录，必须用 exe 自己的位置。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


# 保证无论从哪里启动都能 import 到同目录的核心模块
sys.path.insert(0, str(app_dir()))

import mp3_random_renamer as core            # noqa: E402

APP_TITLE = "MP3 随机数字改名器"
UI_FONT = "Microsoft YaHei UI"
LOG_FONT = "Consolas"

COLORS = {
    "info": "#2b2b2b",
    "rename": "#177245",
    "preview": "#6b3fa0",
    "restore": "#0b6bcb",
    "warn": "#a05a00",
    "error": "#b00020",
    "head": "#0b5cad",
}


# --------------------------------------------------------------------------- #
# 高 DPI 支持（否则在 150% 缩放下字会糊）
# --------------------------------------------------------------------------- #
def enable_dpi_awareness() -> None:
    if sys.platform != "win32":
        return
    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            from ctypes import windll
            windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def config_candidates() -> list[Path]:
    """配置文件候选位置：优先用户 AppData，写不进去就退回程序同目录（便携模式）。"""
    paths: list[Path] = []
    base = os.environ.get("APPDATA")
    if base:
        paths.append(Path(base) / "mp3_random_renamer" / "gui.json")
    paths.append(app_dir() / "gui_config.json")
    return paths


# --------------------------------------------------------------------------- #
# 主窗口
# --------------------------------------------------------------------------- #
class RenamerApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()

        self.title(APP_TITLE)
        self.geometry("900x660")
        self.minsize(840, 560)

        self._queue: "queue.Queue[tuple[str, str]]" = queue.Queue()
        self._worker: threading.Thread | None = None
        self._stop = threading.Event()
        self._running = False
        self._count = 0
        self._once = False
        self._restoring = False
        self._run_id = 0
        self._editable: list[tk.Widget] = []

        self._build_ui()
        self.var_sep.trace_add("write", lambda *_: self._refresh_hint())
        self._load_config()
        self._sync_widgets()

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(80, self._drain_queue)

    # ------------------------------------------------------------------ 界面
    def _build_ui(self) -> None:
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont"):
            try:
                tkfont.nametofont(name).configure(family=UI_FONT, size=9)
            except tk.TclError:
                pass
        style = ttk.Style(self)
        for theme in ("vista", "winnative", "clam"):
            if theme in style.theme_names():
                style.theme_use(theme)
                break

        self.columnconfigure(0, weight=1)
        self.rowconfigure(3, weight=1)

        # ---- 路径 ----
        top = ttk.Frame(self, padding=(12, 12, 12, 2))
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)

        ttk.Label(top, text="监测文件夹").grid(row=0, column=0, padx=(0, 8))
        self.var_path = tk.StringVar()
        self.entry_path = ttk.Entry(top, textvariable=self.var_path)
        self.entry_path.grid(row=0, column=1, sticky="ew")

        self.btn_browse = ttk.Button(top, text="浏览…", width=8, command=self._browse)
        self.btn_browse.grid(row=0, column=2, padx=(8, 0))
        self.btn_open = ttk.Button(top, text="打开", width=6, command=self._open_dir)
        self.btn_open.grid(row=0, column=3, padx=(6, 0))

        # ---- 选项 ----
        opt = ttk.LabelFrame(self, text="选项", padding=(12, 8, 12, 10))
        opt.grid(row=1, column=0, sticky="ew", padx=12, pady=(6, 4))
        opt.columnconfigure(0, weight=1)

        self.var_mode = tk.StringVar(value="prefix")
        self.var_sep = tk.StringVar(value=core.DEFAULT_SEP)
        self.var_digits = tk.StringVar(value=str(core.DEFAULT_DIGITS))
        self.var_interval = tk.StringVar(value="1.0")
        self.var_recursive = tk.BooleanVar(value=False)
        self.var_dry = tk.BooleanVar(value=False)
        self.var_reprocess = tk.BooleanVar(value=False)
        self.var_hint = tk.StringVar()

        # 第一行：左边改名方式，右边分隔符与示例
        row1 = ttk.Frame(opt)
        row1.grid(row=0, column=0, sticky="ew")
        row1.columnconfigure(1, weight=1)

        left1 = ttk.Frame(row1)
        left1.grid(row=0, column=0, sticky="w")
        ttk.Label(left1, text="改名方式").pack(side="left")
        self.rb_prefix = ttk.Radiobutton(
            left1, text="加在原名前（保留原名）", value="prefix",
            variable=self.var_mode, command=self._sync_widgets,
        )
        self.rb_prefix.pack(side="left", padx=(8, 0))
        self.rb_replace = ttk.Radiobutton(
            left1, text="只保留数字（丢掉原名）", value="replace",
            variable=self.var_mode, command=self._sync_widgets,
        )
        self.rb_replace.pack(side="left", padx=(12, 0))

        right1 = ttk.Frame(row1)
        right1.grid(row=0, column=2, sticky="e")
        ttk.Label(right1, text="分隔符").pack(side="left")
        self.entry_sep = ttk.Entry(right1, textvariable=self.var_sep, width=4, justify="center")
        self.entry_sep.pack(side="left", padx=(6, 0))
        self.lbl_hint = ttk.Label(right1, textvariable=self.var_hint, foreground="#666666")
        self.lbl_hint.pack(side="left", padx=(14, 0))

        # 第二行：左边位数与间隔，右边两个开关
        row2 = ttk.Frame(opt)
        row2.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        row2.columnconfigure(1, weight=1)

        left2 = ttk.Frame(row2)
        left2.grid(row=0, column=0, sticky="w")
        ttk.Label(left2, text="随机数字位数").pack(side="left")
        self.spn_digits = ttk.Spinbox(
            left2, from_=1, to=18, width=5,
            textvariable=self.var_digits, justify="center",
        )
        self.spn_digits.pack(side="left", padx=(8, 0))
        ttk.Label(left2, text="扫描间隔").pack(side="left", padx=(24, 0))
        self.spn_interval = ttk.Spinbox(
            left2, from_=0.2, to=3600, increment=0.5, width=7,
            textvariable=self.var_interval, justify="center",
        )
        self.spn_interval.pack(side="left", padx=(8, 0))
        ttk.Label(left2, text="秒").pack(side="left", padx=(6, 0))

        right2 = ttk.Frame(row2)
        right2.grid(row=0, column=2, sticky="e")
        self.chk_recursive = ttk.Checkbutton(
            right2, text="包含子文件夹", variable=self.var_recursive,
        )
        self.chk_recursive.pack(side="left")
        self.chk_dry = ttk.Checkbutton(
            right2, text="试运行（只显示，不改名）", variable=self.var_dry,
        )
        self.chk_dry.pack(side="left", padx=(16, 0))

        # 第三行：重新处理已改名的文件
        row3 = ttk.Frame(opt)
        row3.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        self.chk_reprocess = ttk.Checkbutton(
            row3, text="重新处理已改名的文件（摘掉旧数字，换新编号）",
            variable=self.var_reprocess,
        )
        self.chk_reprocess.pack(side="left")
        ttk.Label(
            row3, foreground="#777777",
            text="不勾选时，改过名的文件一律跳过（状态文件里记着它们）",
        ).pack(side="left", padx=(12, 0))

        self._editable = [
            self.entry_path, self.btn_browse, self.btn_open,
            self.rb_prefix, self.rb_replace, self.entry_sep,
            self.spn_digits, self.spn_interval,
            self.chk_recursive, self.chk_dry, self.chk_reprocess,
        ]

        # ---- 按钮 ----
        bar = ttk.Frame(self, padding=(12, 6, 12, 0))
        bar.grid(row=2, column=0, sticky="ew")
        self.btn_start = ttk.Button(bar, text="开始监测", command=lambda: self._start(False))
        self.btn_start.pack(side="left")
        self.btn_once = ttk.Button(bar, text="只执行一次", command=lambda: self._start(True))
        self.btn_once.pack(side="left", padx=(8, 0))
        self.btn_stop = ttk.Button(bar, text="停止", command=self._stop_clicked)
        self.btn_stop.pack(side="left", padx=(8, 0))
        self.btn_restore = ttk.Button(bar, text="恢复原名", command=self._restore)
        self.btn_restore.pack(side="left", padx=(20, 0))
        self.btn_clear = ttk.Button(bar, text="清空日志", command=self._clear_log)
        self.btn_clear.pack(side="right")
        self._editable.append(self.btn_clear)
        self._editable.append(self.btn_restore)

        # ---- 日志 ----
        box = ttk.Frame(self, padding=(12, 8, 12, 6))
        box.grid(row=3, column=0, sticky="nsew")
        box.rowconfigure(0, weight=1)
        box.columnconfigure(0, weight=1)

        self.text = tk.Text(
            box, height=16, wrap="none", state="disabled",
            font=(LOG_FONT, 10), background="#fbfbfb",
            relief="solid", borderwidth=1, padx=8, pady=6,
        )
        self.text.grid(row=0, column=0, sticky="nsew")
        yscroll = ttk.Scrollbar(box, orient="vertical", command=self.text.yview)
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll = ttk.Scrollbar(box, orient="horizontal", command=self.text.xview)
        xscroll.grid(row=1, column=0, sticky="ew")
        self.text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)

        for level, color in COLORS.items():
            font = (LOG_FONT, 10, "bold") if level == "head" else (LOG_FONT, 10)
            self.text.tag_configure(level, foreground=color, font=font)
        self.text.tag_configure("muted", foreground="#777777")

        # ---- 状态栏 ----
        self.var_status = tk.StringVar(value="就绪：选择文件夹后点“开始监测”或“只执行一次”。")
        status = ttk.Label(
            self, textvariable=self.var_status, anchor="w", relief="sunken", padding=(8, 4),
        )
        status.grid(row=4, column=0, sticky="ew")

    # ------------------------------------------------------------- 状态同步
    def _sync_widgets(self) -> None:
        mode = self.var_mode.get()
        for widget in self._editable:
            try:
                widget.configure(state="disabled" if self._running else "normal")
            except tk.TclError:
                pass

        if not self._running:
            self.entry_sep.configure(state="disabled" if mode == "replace" else "normal")
            self.btn_start.configure(state="normal")
            self.btn_once.configure(state="normal")
            self.btn_stop.configure(state="disabled")
        else:
            self.btn_start.configure(state="disabled")
            self.btn_once.configure(state="disabled")
            self.btn_stop.configure(state="normal")

        if mode == "replace":
            self.var_hint.set("例：123456.mp3")
        else:
            self.var_hint.set(f"例：123456{self.var_sep.get()}歌名.mp3")

    def _refresh_hint(self) -> None:
        if self.var_mode.get() == "replace":
            self.var_hint.set("例：123456.mp3")
        else:
            self.var_hint.set(f"例：123456{self.var_sep.get()}歌名.mp3")

    def _set_running(self, running: bool) -> None:
        self._running = running
        self._sync_widgets()

    # ---------------------------------------------------------------- 日志
    def _append(self, level: str, msg: str) -> None:
        tag = level if level in COLORS else "info"
        stamp = datetime.now().strftime("%H:%M:%S")
        self.text.configure(state="normal")
        self.text.insert("end", f"[{stamp}] {msg}\n", tag)
        self.text.see("end")
        self.text.configure(state="disabled")

        if level == "rename":
            self._count += 1
            self.var_status.set(f"监测中…… 本次已处理 {self._count} 个文件")
        elif level == "preview":
            self._count += 1
            self.var_status.set(f"试运行预览中…… 本次共 {self._count} 个文件")
        elif level == "restore":
            self._count += 1
            self.var_status.set(f"正在恢复原名…… 已恢复 {self._count} 个")

    def _drain_queue(self) -> None:
        done = False
        try:
            while True:
                level, msg, rid = self._queue.get_nowait()
                if rid != self._run_id:      # 上一次运行残留的消息，丢掉
                    continue
                if level == "__done__":
                    done = True
                    self._append("head", msg)
                elif level == "__error__":
                    done = True
                    self._append("error", msg)
                else:
                    self._append(level, msg)
        except queue.Empty:
            pass

        if done:
            self._set_running(False)
            if self._restoring:
                self.var_status.set(f"恢复完成，共 {self._count} 个文件。")
                self._restoring = False
            elif self._once:
                self.var_status.set(f"完成，共处理 {self._count} 个文件。")
            else:
                self.var_status.set(f"已停止。本次共处理 {self._count} 个文件。")
        self.after(80, self._drain_queue)

    def _clear_log(self) -> None:
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")
        self._count = 0

    # ------------------------------------------------------------- 开始/停止
    def _current_dir(self) -> Path:
        raw = self.var_path.get().strip().strip('"')
        if not raw:
            raise ValueError("请先选择要监测的文件夹。")
        root = Path(os.path.expandvars(raw)).expanduser()
        if not root.exists():
            raise ValueError(f"文件夹不存在：{root}")
        if not root.is_dir():
            raise ValueError(f"这不是文件夹：{root}")
        return root.resolve()

    def _collect(self) -> tuple[Path, int, float]:
        root = self._current_dir()

        try:
            digits = int(self.var_digits.get().strip())
        except ValueError:
            raise ValueError("随机数字位数必须是整数。")
        if not 1 <= digits <= 18:
            raise ValueError("随机数字位数需要在 1 ~ 18 之间。")

        try:
            interval = float(self.var_interval.get().strip())
        except ValueError:
            raise ValueError("扫描间隔必须是数字（秒）。")
        if interval <= 0:
            raise ValueError("扫描间隔必须大于 0。")

        return root, digits, interval

    def _start(self, once: bool) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        try:
            root, digits, interval = self._collect()
        except ValueError as exc:
            messagebox.showwarning("提示", str(exc), parent=self)
            return

        # 先确认这个文件夹真的能改名，避免刷一屏“拒绝访问”
        if not self.var_dry.get():
            problem = core.check_writable(root)
            if problem:
                messagebox.showerror(
                    "这个文件夹无法写入",
                    f"{root}\n\n原因：{problem}\n\n"
                    "改名一定会失败。常见原因：\n"
                    "· 程序运行在受限环境 / 沙箱里 —— 请直接在资源管理器里双击 exe 运行\n"
                    "· 杀毒软件的“受控文件夹访问”拦住了未签名程序\n"
                    "· 文件夹或磁盘是只读的，当前账号没有写权限\n"
                    "· 文件被云盘/同步工具锁住",
                    parent=self,
                )
                return

        mode = self.var_mode.get()
        sep = self.var_sep.get()
        recursive = self.var_recursive.get()
        dry_run = self.var_dry.get()
        reprocess = self.var_reprocess.get()

        state_path = root / core.STATE_NAME
        try:
            state = core.State(state_path)
        except Exception as exc:                # 理论上不会发生，兜个底
            messagebox.showerror("错误", f"状态文件读取失败：{exc}", parent=self)
            return

        self._save_config()
        self._count = 0
        self._once = once
        self._run_id += 1
        self._stop.clear()
        self._set_running(True)

        action = "单次执行" if once else "开始监测"
        self._append("head", f"===== {action} · {datetime.now():%Y-%m-%d %H:%M:%S} =====")
        self._append("info", f"文件夹：{root}")
        self._append("info", f"改名方式：{'数字加在原名前' if mode == 'prefix' else '只保留数字'}"
                             f"；位数 {digits}；"
                             + ("递归子文件夹" if recursive else "不递归")
                             + ("；试运行" if dry_run else "")
                             + ("；重新处理已改名文件" if reprocess else ""))
        self._append("info", f"状态文件：{state_path.name}（已用编号 {len(state.used)} 个）")
        if once:
            self.var_status.set("正在扫描……")
        else:
            self.var_status.set("监测中…… 等待 mp3 文件")

        self._worker = threading.Thread(
            target=self._work,
            args=(root, digits, interval, once, state, self._run_id),
            kwargs=dict(
                mode=mode, sep=sep, recursive=recursive,
                dry_run=dry_run, reprocess=reprocess,
            ),
            daemon=True,
        )
        self._worker.start()

    def _work(
        self,
        root: Path,
        digits: int,
        interval: float,
        once: bool,
        state: core.State,
        rid: int,
        *,
        mode: str,
        sep: str,
        recursive: bool,
        dry_run: bool,
        reprocess: bool,
    ) -> None:
        core.set_log_sink(lambda level, msg: self._queue.put((level, msg, rid)))
        try:
            total = core.run_loop(
                root,
                recursive=recursive,
                digits=digits,
                mode=mode,
                sep=sep,
                state=state,
                dry_run=dry_run,
                interval=interval,
                once=once,
                stop_event=self._stop,
                reprocess=reprocess,
            )
            self._queue.put(("__done__", f"结束，共处理 {total} 个文件。", rid))
        except Exception as exc:
            self._queue.put(("__error__", f"运行出错：{exc!r}", rid))
        finally:
            core.set_log_sink(None)

    def _stop_clicked(self) -> None:
        if self._running:
            self._stop.set()
            self.var_status.set("正在停止……（当前这一遍扫描结束后退出）")
            self.btn_stop.configure(state="disabled")

    # ------------------------------------------------------------- 恢复原名
    def _restore(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        try:
            root = self._current_dir()
        except ValueError as exc:
            messagebox.showwarning("提示", str(exc), parent=self)
            return

        dry_run = self.var_dry.get()
        if not dry_run:
            problem = core.check_writable(root)
            if problem:
                messagebox.showerror(
                    "这个文件夹无法写入",
                    f"{root}\n\n原因：{problem}\n\n"
                    "恢复一定会失败。若程序是从受限环境/沙箱里启动的，"
                    "请在资源管理器里直接双击 exe 再试。",
                    parent=self,
                )
                return

        if not messagebox.askyesno(
            "恢复原名",
            f"把 {root} 里本程序改过名的 mp3 还原成原始文件名？\n\n"
            "· 有记录的（本版本改的）精确还原；文件被手动改名过，也能靠内容指纹认出来\n"
            "· 老文件只能摘掉 ≥6 位的数字前缀\n"
            "· 原名已被占用时自动加 (2)(3) 后缀\n\n"
            "这是真实改名，确定继续吗？",
            parent=self,
        ):
            return

        state = core.State(root / core.STATE_NAME)
        self._save_config()
        self._count = 0
        self._restoring = True
        self._run_id += 1
        self._stop.clear()
        self._set_running(True)

        self._append("head", f"===== 恢复原名 · {datetime.now():%Y-%m-%d %H:%M:%S} =====")
        self._append("info", f"文件夹：{root}（原名记录 {len(state.orig)} 条）")
        self._append("info", "范围：" + ("包含子文件夹" if self.var_recursive.get() else "不包含子文件夹")
                             + ("；试运行" if dry_run else ""))
        self.var_status.set("正在恢复原名……")

        self._worker = threading.Thread(
            target=self._work_restore,
            args=(root, state, self.var_sep.get(), self.var_recursive.get(), dry_run, self._run_id),
            daemon=True,
        )
        self._worker.start()

    def _work_restore(
        self,
        root: Path,
        state: core.State,
        sep: str,
        recursive: bool,
        dry_run: bool,
        rid: int,
    ) -> None:
        core.set_log_sink(lambda level, msg: self._queue.put((level, msg, rid)))
        try:
            total = core.restore_once(
                root, recursive=recursive, state=state, sep=sep, dry_run=dry_run,
            )
            self._queue.put(("__done__", f"恢复完成，共 {total} 个文件。", rid))
        except Exception as exc:
            self._queue.put(("__error__", f"恢复出错：{exc!r}", rid))
        finally:
            core.set_log_sink(None)

    # ------------------------------------------------------------ 杂项按钮
    def _browse(self) -> None:
        current = self.var_path.get().strip().strip('"')
        initial = current if current and Path(current).is_dir() else str(Path.home())
        chosen = filedialog.askdirectory(parent=self, title="选择要监测的文件夹", initialdir=initial)
        if chosen:
            self.var_path.set(os.path.normpath(chosen))
            self._sync_widgets()

    def _open_dir(self) -> None:
        raw = self.var_path.get().strip().strip('"')
        path = Path(os.path.expandvars(raw)).expanduser() if raw else None
        if path is None or not path.is_dir():
            messagebox.showwarning("提示", "请先选择一个有效的文件夹。", parent=self)
            return
        try:
            os.startfile(str(path))          # noqa: S606
        except Exception as exc:
            messagebox.showerror("错误", f"无法打开文件夹：{exc}", parent=self)

    # ------------------------------------------------------------- 配置读写
    def _load_config(self) -> None:
        data: dict | None = None
        for path in config_candidates():
            try:
                loaded = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if isinstance(loaded, dict):
                data = loaded
                break
        if data is None:
            return

        self.var_path.set(str(data.get("path", "")))
        if data.get("mode") in ("prefix", "replace"):
            self.var_mode.set(data["mode"])
        if isinstance(data.get("sep"), str):
            self.var_sep.set(data["sep"])
        if str(data.get("digits", "")).isdigit():
            self.var_digits.set(str(data["digits"]))
        try:
            self.var_interval.set(f"{float(data.get('interval', 1.0)):g}")
        except (TypeError, ValueError):
            pass
        self.var_recursive.set(bool(data.get("recursive", False)))
        self.var_dry.set(bool(data.get("dry_run", False)))
        self.var_reprocess.set(bool(data.get("reprocess", False)))
        self._refresh_hint()

    def _save_config(self) -> None:
        data = {
            "path": self.var_path.get(),
            "mode": self.var_mode.get(),
            "sep": self.var_sep.get(),
            "digits": self.var_digits.get(),
            "interval": self.var_interval.get(),
            "recursive": self.var_recursive.get(),
            "dry_run": self.var_dry.get(),
            "reprocess": self.var_reprocess.get(),
        }
        payload = json.dumps(data, ensure_ascii=False, indent=2)
        for path in config_candidates():
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(payload, encoding="utf-8")
                return
            except OSError:
                continue

    # ------------------------------------------------------------------ 退出
    def _on_close(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            if not messagebox.askyesno("退出", "正在监测中，确定停止并退出吗？", parent=self):
                return
            self._stop.set()
            self._worker.join(timeout=3.0)
        self._save_config()
        self.destroy()


def report_error(exc_text: str) -> None:
    """打包成 exe 后没有控制台，出错时写到临时目录并尽量弹窗。"""
    log_path = Path(tempfile.gettempdir()) / "mp3_renamer_error.log"
    try:
        log_path.write_text(exc_text, encoding="utf-8")
        where = str(log_path)
    except OSError:
        where = "（日志也写不出去）"
    try:
        box = tk.Tk()
        box.withdraw()
        messagebox.showerror(
            APP_TITLE,
            f"程序出错了：\n\n{exc_text.strip().splitlines()[-1] if exc_text.strip() else ''}\n\n"
            f"详细信息已写入：\n{where}",
        )
        box.destroy()
    except Exception:
        pass


def parse_cli(argv: list[str]) -> tuple[str | None, bool, bool, bool]:
    """支持 exe/py 带参数启动：

        mp3_renamer_gui.py "D:\\Music" --start          直接开监测（可做快捷方式/开机自启）
        mp3_renamer_gui.py "D:\\Music" --once           启动后只扫一遍
        mp3_renamer_gui.py "D:\\Music" --once --reprocess   把已改过名的重新编号
    """
    path: str | None = None
    start = False
    once = False
    reprocess = False
    for arg in argv[1:]:
        if arg in ("-h", "--help", "/?"):
            print(__doc__)
            raise SystemExit(0)
        if arg == "--start":
            start = True
        elif arg == "--once":
            once = True
            start = True
        elif arg == "--reprocess":
            reprocess = True
        elif not arg.startswith("-") and path is None:
            path = arg
    return path, start, once, reprocess


def main() -> int:
    enable_dpi_awareness()

    def hook(exc_type, exc, tb):
        report_error("".join(traceback.format_exception(exc_type, exc, tb)))

    sys.excepthook = hook

    path, autostart, once, reprocess = parse_cli(sys.argv)

    app = RenamerApp()
    # 界面回调里的异常默认只打到 stderr（exe 里看不到），这里接管
    app.report_callback_exception = lambda et, ev, etb: report_error(
        "".join(traceback.format_exception(et, ev, etb))
    )
    if path:
        app.var_path.set(os.path.normpath(path))
    if reprocess:
        app.var_reprocess.set(True)
    if autostart:
        # 等窗口画出来再自动开始，避免日志滚动时窗口还没渲染
        app.after(300, lambda: app._start(once))
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
