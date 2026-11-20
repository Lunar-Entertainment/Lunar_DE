#!/usr/bin/env python3
"""Lunar DE Gemini Live Voice Assistant Daemon (Low-Latency + Vision + AGY)."""

from __future__ import annotations

import array
import asyncio
import base64
import ctypes
import importlib.util
import json
import math
import os
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import websockets

# Ensure Xlib thread-safety before any GTK/GDK imports on worker threads
try:
    _libx11_init = ctypes.cdll.LoadLibrary("libX11.so.6")
    _libx11_init.XInitThreads()
except Exception:
    pass

STATE_PATH = Path("/tmp/lunar-live-state.json")
CMD_PATH = Path("/tmp/lunar-live-cmd")
AGY_LAST_OUTPUT_PATH = Path("/tmp/lunar-agy-last.txt")

SAMPLE_RATE_IN = 16000
SAMPLE_RATE_OUT = 24000
CHUNK_SAMPLES = 320  # 20ms at 16kHz for minimal audio framing delay
CHUNK_BYTES = CHUNK_SAMPLES * 2

LIVE_MODELS: list[tuple[str, dict[str, Any]]] = [
    ("models/gemini-3.1-flash-live-preview", {"thinkingBudget": 0}),
    ("models/gemini-2.5-flash-native-audio-latest", {"thinkingBudget": 0}),
    ("models/gemini-2.5-flash-native-audio-preview-12-2025", {}),
]

SYSTEM_PROMPT = (
    "You are Lunar, the fast voice assistant built into Lunar DE on Linux (X11). "
    "Keep spoken replies ultra-concise (1 to 2 short natural sentences). "
    "You have full desktop control and vision: you can view the screen (view_screen, "
    "set_screen_vision), list/focus/minimize/maximize/snap/move/close any app window "
    "(list_windows, open_app, close_app, manage_window), interact inside apps by "
    "clicking, typing, scrolling, or pressing keys (interact_in_app), open URLs, "
    "control media playback/volume/wallpapers/workspaces/clipboard or run shell commands "
    "(media_control, desktop_control), and delegate coding or complex multi-step tasks "
    "to Antigravity (ask_antigravity, open_antigravity, get_antigravity_status). "
    "Act immediately without asking permission for safe everyday requests (opening, closing, "
    "viewing, clicking, typing, workspace/media changes). Only ask before destructive acts "
    "like force-killing apps with unsaved work or deleting files."
)

TOOLS_SPEC: list[dict[str, Any]] = [
    {
        "functionDeclarations": [
            {
                "name": "open_app",
                "description": "Launch a desktop application by name (e.g., brave, discord, files, terminal, spotify, settings).",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "name": {"type": "STRING", "description": "Application name or keyword to search and launch."}
                    },
                    "required": ["name"],
                },
            },
            {
                "name": "close_app",
                "description": "Close an open application window cleanly (or force-kill if requested).",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "target": {"type": "STRING", "description": "Window title, app name (e.g. discord, brave), X11 ID, or 'active'."},
                        "force": {"type": "BOOLEAN", "description": "True to force-kill unresponsive window/process."},
                    },
                    "required": ["target"],
                },
            },
            {
                "name": "list_windows",
                "description": "List all open windows with X11 IDs, titles, app class, workspace, geometry, and active/minimized state.",
                "parameters": {"type": "OBJECT", "properties": {}},
            },
            {
                "name": "manage_window",
                "description": "Control an open window: focus, close, minimize, maximize, unmaximize, fullscreen, snap_left, snap_right, move_to_workspace_1, move_to_workspace_2.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "target": {"type": "STRING", "description": "Window title substring, app name, X11 ID, or 'active'."},
                        "action": {
                            "type": "STRING",
                            "enum": [
                                "focus",
                                "close",
                                "minimize",
                                "maximize",
                                "unmaximize",
                                "fullscreen",
                                "snap_left",
                                "snap_right",
                                "move_to_workspace_1",
                                "move_to_workspace_2",
                            ],
                        },
                    },
                    "required": ["target", "action"],
                },
            },
            {
                "name": "view_screen",
                "description": "Capture and view the user's current screen, active monitor, or a specific window. Always call this before answering questions about what is on screen or before clicking UI elements by norm_x/norm_y.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "target": {
                            "type": "STRING",
                            "enum": ["active_monitor", "all", "window"],
                            "description": "Capture the active monitor (default), all monitors, or a specific window.",
                        },
                        "window_name": {"type": "STRING", "description": "Window title substring when target='window'."},
                    },
                },
            },
            {
                "name": "set_screen_vision",
                "description": "Turn continuous periodic screen sharing on or off.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "enabled": {"type": "BOOLEAN", "description": "True to stream periodic screenshots, false to stop."}
                    },
                    "required": ["enabled"],
                },
            },
            {
                "name": "interact_in_app",
                "description": "Perform UI actions inside an app: click, double_click, right_click, type text, press keys/shortcuts, or scroll. Use norm_x/norm_y (0-1000) relative to the last viewed screen.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "action": {
                            "type": "STRING",
                            "enum": ["click", "double_click", "right_click", "type", "key", "scroll_up", "scroll_down"],
                        },
                        "target_window": {"type": "STRING", "description": "Optional window title or ID to focus first."},
                        "text": {"type": "STRING", "description": "Text to type when action='type'."},
                        "key": {"type": "STRING", "description": "Key or chord when action='key' (e.g. 'ctrl+l', 'Return', 'Escape', 'Alt+F4')."},
                        "norm_x": {"type": "NUMBER", "description": "Normalized horizontal position 0-1000 on last captured screen."},
                        "norm_y": {"type": "NUMBER", "description": "Normalized vertical position 0-1000 on last captured screen."},
                        "x": {"type": "INTEGER", "description": "Absolute X11 X pixel coordinate."},
                        "y": {"type": "INTEGER", "description": "Absolute X11 Y pixel coordinate."},
                        "button": {"type": "STRING", "enum": ["left", "middle", "right"]},
                        "scroll_amount": {"type": "INTEGER", "description": "Wheel notches for scroll_up/scroll_down (default 3)."},
                    },
                    "required": ["action"],
                },
            },
            {
                "name": "open_url",
                "description": "Open a URL or search query in the default web browser.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "url": {"type": "STRING", "description": "URL or web search query."}
                    },
                    "required": ["url"],
                },
            },
            {
                "name": "media_control",
                "description": "Control media playback or system audio volume.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "action": {
                            "type": "STRING",
                            "enum": ["play_pause", "next", "previous", "stop", "volume_up", "volume_down", "mute", "unmute", "set_volume"],
                        },
                        "percent": {"type": "INTEGER", "description": "Volume percentage 0-100 when action='set_volume'."},
                    },
                    "required": ["action"],
                },
            },
            {
                "name": "desktop_control",
                "description": "Control Lunar DE settings and utilities: switch_workspace (1 or 2), set_wallpaper, set_bar_layout (top/taskbar/taskbar-top), open_terminal, open_display_settings, open_settings, get_system_info, get_clipboard, set_clipboard, or run_command.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "action": {
                            "type": "STRING",
                            "enum": [
                                "switch_workspace",
                                "set_wallpaper",
                                "set_bar_layout",
                                "open_terminal",
                                "open_display_settings",
                                "open_settings",
                                "get_system_info",
                                "get_clipboard",
                                "set_clipboard",
                                "run_command",
                            ],
                        },
                        "value": {"type": "STRING", "description": "Argument (e.g. '2', 'nordic aurora', 'taskbar-top', clipboard text, or shell command)."},
                    },
                    "required": ["action"],
                },
            },
            {
                "name": "ask_antigravity",
                "description": "Delegate coding, debugging, repository edits, or complex multi-step agent tasks to Antigravity (agy) asynchronously in the background.",
                "behavior": "NON_BLOCKING",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "prompt": {"type": "STRING", "description": "Detailed task description for Antigravity."},
                        "workspace": {"type": "STRING", "description": "Optional workspace folder path."},
                        "mode": {"type": "STRING", "enum": ["auto", "accept-edits", "plan", "default"]},
                        "continue_session": {"type": "BOOLEAN", "description": "Continue recent Antigravity thread in workspace."},
                    },
                    "required": ["prompt"],
                },
            },
            {
                "name": "open_antigravity",
                "description": "Focus or launch the Antigravity desktop IDE window.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "workspace": {"type": "STRING", "description": "Optional project folder path to open."}
                    },
                },
            },
            {
                "name": "get_antigravity_status",
                "description": "Check Antigravity CLI availability, active background tasks, and latest AGY output summary.",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "task_id": {"type": "STRING", "description": "Optional task ID from ask_antigravity."}
                    },
                },
            },
        ]
    }
]


_CAPTURE_LOCK = threading.Lock()
_X11_INIT_THREADS_DONE = False
_KEY_ALIASES = {
    "enter": "Return",
    "return": "Return",
    "esc": "Escape",
    "escape": "Escape",
    "tab": "Tab",
    "space": "space",
    "backspace": "BackSpace",
    "bs": "BackSpace",
    "del": "Delete",
    "delete": "Delete",
    "up": "Up",
    "down": "Down",
    "left": "Left",
    "right": "Right",
    "home": "Home",
    "end": "End",
    "pageup": "Prior",
    "pgup": "Prior",
    "pagedown": "Next",
    "pgdn": "Next",
    "super": "Super_L",
    "win": "Super_L",
    "cmd": "Super_L",
    "meta": "Super_L",
    "ctrl": "ctrl",
    "control": "ctrl",
    "alt": "alt",
    "shift": "shift",
}


def resolve_display(display: str | None = None) -> str:
    if display and display.strip():
        return display.strip()
    return os.environ.get("DISPLAY") or ":0"


def get_api_key_path() -> Path:
    cfg_home = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(cfg_home) / "lunar-de" / "gemini_api_key"


def get_config_dir() -> Path:
    return get_api_key_path().parent


def load_api_key() -> str:
    for env_name in ("GEMINI_API_KEY", "GOOGLE_API_KEY"):
        val = os.environ.get(env_name, "").strip()
        if val:
            return val
    key_file = get_api_key_path()
    if key_file.exists():
        try:
            return key_file.read_text(encoding="utf-8").strip()
        except OSError:
            return ""
    return ""


def build_setup_message(model: str, thinking_cfg: dict[str, Any] | None = None) -> dict[str, Any]:
    gen_cfg: dict[str, Any] = {
        "responseModalities": ["AUDIO"],
        "speechConfig": {
            "voiceConfig": {"prebuiltVoiceConfig": {"voiceName": "Aoede"}}
        },
    }
    if thinking_cfg:
        gen_cfg["thinkingConfig"] = dict(thinking_cfg)

    return {
        "setup": {
            "model": model,
            "generationConfig": gen_cfg,
            "systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]},
            "tools": TOOLS_SPEC,
            "inputAudioTranscription": {},
            "outputAudioTranscription": {},
            "realtimeInputConfig": {
                "automaticActivityDetection": {
                    "disabled": False,
                    "startOfSpeechSensitivity": "START_SENSITIVITY_HIGH",
                    "endOfSpeechSensitivity": "END_SENSITIVITY_HIGH",
                    "prefixPaddingMs": 100,
                    "silenceDurationMs": 350,
                },
                "activityHandling": "START_OF_ACTIVITY_INTERRUPTS",
            },
        }
    }


def pcm_rms(pcm_bytes: bytes) -> float:
    if len(pcm_bytes) < 2:
        return 0.0
    samples = array.array("h")
    samples.frombytes(pcm_bytes[: len(pcm_bytes) - (len(pcm_bytes) % 2)])
    if not samples:
        return 0.0
    sq = sum(s * s for s in samples)
    return math.sqrt(sq / len(samples))


def play_chime_async(rising: bool = True) -> None:
    """Play a brief 2-tone chime asynchronously without stalling audio capture."""
    freqs = (523.25, 783.99) if rising else (659.25, 440.0)
    samples = array.array("h")
    sr = SAMPLE_RATE_OUT
    for freq in freqs:
        n = int(sr * 0.07)
        for i in range(n):
            env = math.sin(math.pi * i / n)
            samples.append(int(3600 * env * math.sin(2.0 * math.pi * freq * i / sr)))
    raw = samples.tobytes()

    def _runner() -> None:
        try:
            proc = subprocess.Popen(
                ["paplay", "--raw", "--format=s16le", f"--rate={sr}", "--channels=1"],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if proc.stdin:
                proc.stdin.write(raw)
                proc.stdin.close()
            proc.wait(timeout=2)
        except Exception:
            pass

    threading.Thread(target=_runner, daemon=True).start()


def sanitize_app_env(display: str | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env["DISPLAY"] = resolve_display(display)
    for var in (
        "WAYLAND_DISPLAY",
        "SESSION_MANAGER",
        "GNOME_DESKTOP_SESSION_ID",
        "GNOME_SHELL_SESSION_MODE",
        "GTK_MODULES",
        "GTK3_MODULES",
        "QT_ACCESSIBILITY",
        "GIO_USE_VFS",
    ):
        env.pop(var, None)
    env["GDK_BACKEND"] = "x11"
    env["QT_QPA_PLATFORM"] = "xcb"
    env["SDL_VIDEODRIVER"] = "x11"
    env["CLUTTER_BACKEND"] = "x11"
    env["ELECTRON_OZONE_PLATFORM_HINT"] = "x11"
    env["GTK_USE_PORTAL"] = "0"
    env["NO_AT_BRIDGE"] = "1"
    env["GTK_A11Y"] = "none"
    env["XDG_CURRENT_DESKTOP"] = "Lunar"
    return env


def inject_flatpak_x11_flags(exec_cmd: str, display: str) -> str:
    if "flatpak run" not in exec_cmd:
        return exec_cmd
    extra = (
        f" --env=DISPLAY={display}"
        " --env=GDK_BACKEND=x11 --env=QT_QPA_PLATFORM=xcb"
        " --env=ELECTRON_OZONE_PLATFORM_HINT=x11"
        " --env=NO_AT_BRIDGE=1 --env=GTK_A11Y=none"
        " --env=XDG_CURRENT_DESKTOP=Lunar"
        " --unset-env=WAYLAND_DISPLAY --unset-env=GTK_MODULES"
        " --unset-env=GTK3_MODULES --unset-env=QT_ACCESSIBILITY"
        " --unset-env=GIO_USE_VFS"
    )
    return exec_cmd.replace("flatpak run", f"flatpak run{extra}", 1)


def spawn_detached(cmd: str | list[str], cwd: Path | None = None, display: str | None = None) -> None:
    env = sanitize_app_env(display)
    argv = ["/bin/sh", "-c", cmd] if isinstance(cmd, str) else cmd
    work_dir = str(cwd) if (cwd and cwd.is_dir()) else str(Path.home())
    subprocess.Popen(
        argv,
        env=env,
        cwd=work_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def run_cmd_sync(
    cmd: list[str],
    display: str | None = None,
    timeout: float = 5.0,
    stdin_text: str | None = None,
) -> tuple[int, str, str]:
    env = os.environ.copy()
    if display:
        env["DISPLAY"] = display
    else:
        env.setdefault("DISPLAY", resolve_display())
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            input=stdin_text,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except Exception as exc:
        return 1, "", str(exc)


@dataclass
class DesktopAppEntry:
    desktop_id: str
    name: str
    comment: str
    exec_cmd: str
    working_dir: str
    terminal: bool


class AppIndex:
    """Cached index of installed .desktop applications."""

    def __init__(self, ttl_sec: float = 60.0) -> None:
        self._ttl = ttl_sec
        self._entries: list[DesktopAppEntry] = []
        self._loaded_at = 0.0
        self._lock = threading.Lock()

    def _scan(self) -> list[DesktopAppEntry]:
        dirs = [
            Path.home() / ".local/share/applications",
            Path("/usr/share/applications"),
            Path.home() / ".local/share/flatpak/exports/share/applications",
            Path("/var/lib/flatpak/exports/share/applications"),
            Path("/var/lib/snapd/desktop/applications"),
        ]
        seen: set[str] = set()
        entries: list[DesktopAppEntry] = []
        for d in dirs:
            if not d.exists():
                continue
            try:
                files = sorted(d.glob("*.desktop"))
            except OSError:
                continue
            for p in files:
                if p.name in seen:
                    continue
                try:
                    txt = p.read_text(encoding="utf-8", errors="ignore")
                except OSError:
                    continue
                in_main = False
                no_display = False
                hidden = False
                app_type = ""
                name = ""
                comment = ""
                exec_cmd = ""
                work_dir = ""
                terminal = False
                for raw in txt.splitlines():
                    line = raw.strip()
                    if not line or line.startswith("#"):
                        continue
                    if line.startswith("[") and line.endswith("]"):
                        if line == "[Desktop Entry]":
                            in_main = True
                        elif in_main:
                            break
                        continue
                    if not in_main or "=" not in line:
                        continue
                    key, val = line.split("=", 1)
                    key = key.strip()
                    val = val.strip()
                    if key == "Type":
                        app_type = val
                    elif key == "Name" and not name:
                        name = val
                    elif key == "Comment" and not comment:
                        comment = val
                    elif key == "Exec" and not exec_cmd:
                        exec_cmd = val
                    elif key == "Path" and not work_dir:
                        work_dir = val
                    elif key == "NoDisplay":
                        no_display = val.lower() == "true"
                    elif key == "Hidden":
                        hidden = val.lower() == "true"
                    elif key == "Terminal":
                        terminal = val.lower() == "true"
                if app_type != "Application" or no_display or hidden or not name or not exec_cmd:
                    continue
                for tok in ("--file-forwarding", "@@u", "@@", "%U", "%u", "%F", "%f", "%i", "%c", "%k", "%d", "%D", "%n", "%N", "%v", "%m"):
                    exec_cmd = exec_cmd.replace(tok, "")
                exec_cmd = exec_cmd.strip()
                if not exec_cmd:
                    continue
                seen.add(p.name)
                entries.append(
                    DesktopAppEntry(
                        desktop_id=p.stem.lower(),
                        name=name,
                        comment=comment,
                        exec_cmd=exec_cmd,
                        working_dir=work_dir,
                        terminal=terminal,
                    )
                )
        return entries

    def get_entries(self, force: bool = False) -> list[DesktopAppEntry]:
        with self._lock:
            now = time.monotonic()
            if force or not self._entries or (now - self._loaded_at) > self._ttl:
                self._entries = self._scan()
                self._loaded_at = now
            return list(self._entries)

    def find_best(self, query: str) -> DesktopAppEntry | None:
        q = query.lower().strip()
        if not q:
            return None
        entries = self.get_entries()
        best: tuple[int, DesktopAppEntry] | None = None
        for entry in entries:
            n_low = entry.name.lower()
            id_low = entry.desktop_id
            score = 0
            if n_low == q or id_low == q:
                score = 100
            elif id_low.endswith(f".{q}"):
                score = 92
            elif n_low.startswith(q):
                score = 80
            elif f" {q}" in f" {n_low}":
                score = 70
            elif q in n_low:
                score = 60
            elif q in id_low:
                score = 50
            elif q in entry.comment.lower():
                score = 25
            if score > 0 and (best is None or score > best[0]):
                best = (score, entry)
        return best[1] if best else None


APP_INDEX = AppIndex()


# ---------------------------------------------------------------------------
# Fast X11 / EWMH IPC & Window Queries
# ---------------------------------------------------------------------------

class _XClientMessageData(ctypes.Union):
    _fields_ = [("b", ctypes.c_char * 20), ("s", ctypes.c_short * 10), ("l", ctypes.c_long * 5)]


class _XClientMessageEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("message_type", ctypes.c_ulong),
        ("format", ctypes.c_int),
        ("data", _XClientMessageData),
    ]


_X11_LIB: Any = None


def _load_x11() -> Any:
    global _X11_LIB
    if _X11_LIB is None:
        lib = ctypes.cdll.LoadLibrary("libX11.so.6")
        lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
        lib.XOpenDisplay.restype = ctypes.c_void_p
        lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        lib.XDefaultRootWindow.restype = ctypes.c_ulong
        lib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.XInternAtom.restype = ctypes.c_ulong
        lib.XSendEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_ulong,
            ctypes.c_int,
            ctypes.c_long,
            ctypes.c_void_p,
        ]
        lib.XSendEvent.restype = ctypes.c_int
        lib.XFlush.argtypes = [ctypes.c_void_p]
        lib.XFlush.restype = ctypes.c_int
        lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
        lib.XCloseDisplay.restype = ctypes.c_int
        _X11_LIB = lib
    return _X11_LIB


def send_x11_client_message(
    display: str,
    target_win: int,
    atom_name: str,
    d0: int = 0,
    d1: int = 0,
    d2: int = 0,
    d3: int = 0,
    propagate_to_root: bool = False,
    atom_names_in_data: tuple[str | None, str | None] | None = None,
) -> bool:
    try:
        x11 = _load_x11()
        dpy = x11.XOpenDisplay(display.encode("utf-8"))
        if not dpy:
            return False
        try:
            root = x11.XDefaultRootWindow(dpy)
            win = root if target_win == 0 else target_win
            atom = x11.XInternAtom(dpy, atom_name.encode("utf-8"), 0)
            if atom_names_in_data:
                if atom_names_in_data[0]:
                    d1 = int(x11.XInternAtom(dpy, atom_names_in_data[0].encode("utf-8"), 0))
                if atom_names_in_data[1]:
                    d2 = int(x11.XInternAtom(dpy, atom_names_in_data[1].encode("utf-8"), 0))
            ev = _XClientMessageEvent()
            ev.type = 33  # ClientMessage
            ev.display = dpy
            ev.window = win
            ev.message_type = atom
            ev.format = 32
            ev.data.l[0] = d0
            ev.data.l[1] = d1
            ev.data.l[2] = d2
            ev.data.l[3] = d3
            mask = ((1 << 20) | (1 << 19)) if propagate_to_root else 0
            dest = root if propagate_to_root else win
            x11.XSendEvent(dpy, dest, 0, mask, ctypes.byref(ev))
            x11.XFlush(dpy)
            return True
        finally:
            x11.XCloseDisplay(dpy)
    except Exception:
        return False


def get_root_window_prop_win(display: str, prop: str) -> int:
    rc, out, _ = run_cmd_sync(["xprop", "-display", display, "-root", prop], display=display, timeout=2.0)
    if rc != 0 or "#" not in out:
        return 0
    m = re.search(r"0x[0-9a-fA-F]+", out.split("#", 1)[1])
    return int(m.group(0), 16) if m else 0


def parse_monitors(display: str) -> list[dict[str, Any]]:
    rc, out, _ = run_cmd_sync(["xrandr", "--listmonitors"], display=display, timeout=2.0)
    mons: list[dict[str, Any]] = []
    if rc == 0:
        for line in out.splitlines():
            m = re.match(r"^\s*(\d+):\s+(\+\*?|\*?)\s*(\S+)\s+(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+)", line)
            if m:
                mons.append(
                    {
                        "index": int(m.group(1)),
                        "primary": "*" in m.group(2),
                        "name": m.group(3),
                        "x": int(m.group(6)),
                        "y": int(m.group(7)),
                        "width": int(m.group(4)),
                        "height": int(m.group(5)),
                    }
                )
    if not mons:
        rc_d, dim_out, _ = run_cmd_sync(["xdpyinfo", "-display", display], display=display, timeout=2.0)
        dm = re.search(r"dimensions:\s+(\d+)x(\d+)", dim_out) if rc_d == 0 else None
        w, h = (int(dm.group(1)), int(dm.group(2))) if dm else (1920, 1080)
        mons.append({"index": 0, "primary": True, "name": "default", "x": 0, "y": 0, "width": w, "height": h})
    return mons


def query_windows(display: str) -> dict[str, Any]:
    active_id = get_root_window_prop_win(display, "_NET_ACTIVE_WINDOW")
    cur_desk = 0
    rc_d, desk_out, _ = run_cmd_sync(["xprop", "-display", display, "-root", "_NET_CURRENT_DESKTOP"], display=display, timeout=2.0)
    if rc_d == 0 and "=" in desk_out:
        dm = re.search(r"\d+", desk_out.split("=", 1)[1])
        if dm:
            cur_desk = int(dm.group(0))

    windows: list[dict[str, Any]] = []
    rc, out, _ = run_cmd_sync(["wmctrl", "-lpxG"], display=display, timeout=2.5)
    if rc == 0 and out:
        for line in out.splitlines():
            parts = line.split(None, 9)
            if len(parts) < 9:
                continue
            wid_hex = parts[0]
            try:
                wid_int = int(wid_hex, 16)
                desk = int(parts[1])
                pid = int(parts[2])
                wx, wy, ww, wh = int(parts[3]), int(parts[4]), int(parts[5]), int(parts[6])
            except ValueError:
                continue
            wm_class = parts[7]
            title = parts[9].strip() if len(parts) > 9 else ""
            if title in ("lunar-panel", "lunar-popup", "lunar-wm"):
                continue
            windows.append(
                {
                    "id": hex(wid_int),
                    "id_int": wid_int,
                    "title": title,
                    "app_class": wm_class,
                    "pid": pid,
                    "workspace": desk + 1,
                    "x": wx,
                    "y": wy,
                    "width": ww,
                    "height": wh,
                    "active": wid_int == active_id,
                    "minimized": False,
                }
            )
    else:
        rc_c, cl_out, _ = run_cmd_sync(["xprop", "-display", display, "-root", "_NET_CLIENT_LIST"], display=display, timeout=2.0)
        if rc_c == 0 and "#" in cl_out:
            for hx in re.findall(r"0x[0-9a-fA-F]+", cl_out.split("#", 1)[1]):
                wid_int = int(hx, 16)
                _, title, _ = run_cmd_sync(["xdotool", "getwindowname", str(wid_int)], display=display, timeout=1.5)
                if title in ("lunar-panel", "lunar-popup", "lunar-wm"):
                    continue
                windows.append(
                    {
                        "id": hex(wid_int),
                        "id_int": wid_int,
                        "title": title or "Window",
                        "app_class": "",
                        "pid": 0,
                        "workspace": cur_desk + 1,
                        "x": 0,
                        "y": 0,
                        "width": 0,
                        "height": 0,
                        "active": wid_int == active_id,
                        "minimized": False,
                    }
                )

    if windows:
        ids_str = " ".join(w["id"] for w in windows[:30])
        rc_s, st_out, _ = run_cmd_sync(
            [
                "/bin/sh",
                "-c",
                f'for id in {ids_str}; do echo "WID=$id"; xprop -id "$id" _NET_WM_STATE 2>/dev/null; done',
            ],
            display=display,
            timeout=2.5,
        )
        if rc_s == 0 and st_out:
            by_id = {w["id"]: w for w in windows}
            cur_wid = ""
            for ln in st_out.splitlines():
                if ln.startswith("WID="):
                    cur_wid = ln.split("=", 1)[1].strip()
                elif cur_wid in by_id and "_NET_WM_STATE_HIDDEN" in ln:
                    by_id[cur_wid]["minimized"] = True

    return {
        "display": display,
        "current_workspace": cur_desk + 1,
        "active_window_id": hex(active_id) if active_id else None,
        "windows": windows,
    }


def resolve_window(target: str, display: str) -> dict[str, Any] | None:
    info = query_windows(display)
    wins = info["windows"]
    if not wins:
        return None
    t = target.strip()
    if not t or t.lower() == "active":
        for w in wins:
            if w["active"]:
                return w
        return wins[0]

    # Exact numeric or hex XID match
    t_num: int | None = None
    try:
        t_num = int(t, 16) if t.lower().startswith("0x") else int(t)
    except ValueError:
        t_num = None
    if t_num is not None:
        for w in wins:
            if w["id_int"] == t_num:
                return w

    tl = t.lower()
    for w in wins:
        if tl == w["title"].lower() or tl == w["app_class"].lower():
            return w
    # Prefer active window if multiple match substring
    ordered = sorted(wins, key=lambda item: (not item["active"],))
    for w in ordered:
        if tl in w["title"].lower() or tl in w["app_class"].lower():
            return w
    return None


# ---------------------------------------------------------------------------
# Fast In-Process Screen Vision Capture
# ---------------------------------------------------------------------------

def capture_screen_jpeg(
    display: str,
    target: str = "active_monitor",
    window_name: str = "",
    max_width: int = 1280,
    quality: int = 72,
) -> dict[str, Any]:
    win_info = query_windows(display)
    monitors = parse_monitors(display)
    wins = win_info["windows"]
    active_win = next((w for w in wins if w["active"]), None)

    rx, ry, rw, rh = 0, 0, 0, 0
    if target == "window" and (window_name or active_win):
        matched = resolve_window(window_name or "active", display)
        if matched and matched["width"] > 0 and matched["height"] > 0:
            rx, ry, rw, rh = matched["x"], matched["y"], matched["width"], matched["height"]

    if rw == 0 or rh == 0:
        if target == "all":
            min_x = min(m["x"] for m in monitors)
            min_y = min(m["y"] for m in monitors)
            max_x = max(m["x"] + m["width"] for m in monitors)
            max_y = max(m["y"] + m["height"] for m in monitors)
            rx, ry, rw, rh = min_x, min_y, max(1, max_x - min_x), max(1, max_y - min_y)
        else:
            chosen_mon = monitors[0]
            if active_win and active_win["width"] > 0:
                cx = active_win["x"] + active_win["width"] // 2
                cy = active_win["y"] + active_win["height"] // 2
                for m in monitors:
                    if m["x"] <= cx < m["x"] + m["width"] and m["y"] <= cy < m["y"] + m["height"]:
                        chosen_mon = m
                        break
            else:
                prim = next((m for m in monitors if m["primary"]), None)
                if prim:
                    chosen_mon = prim
            rx, ry, rw, rh = chosen_mon["x"], chosen_mon["y"], chosen_mon["width"], chosen_mon["height"]

    jpg_bytes: bytes | None = None
    # Fast path: in-process Gdk capture
    with _CAPTURE_LOCK:
        try:
            old_disp = os.environ.get("DISPLAY")
            os.environ["DISPLAY"] = display
            import gi

            gi.require_version("Gdk", "3.0")
            from gi.repository import Gdk

            gdk_disp = Gdk.Display.open(display) or Gdk.Display.get_default()
            if gdk_disp:
                screen = gdk_disp.get_default_screen()
                root = screen.get_root_window()
                scr_w, scr_h = root.get_width(), root.get_height()
                cx = max(0, min(rx, max(0, scr_w - 1)))
                cy = max(0, min(ry, max(0, scr_h - 1)))
                cw = max(1, min(rw, scr_w - cx))
                ch = max(1, min(rh, scr_h - cy))
                pb = Gdk.pixbuf_get_from_window(root, cx, cy, cw, ch)
                if pb is not None:
                    rx, ry, rw, rh = cx, cy, cw, ch
                    if cw > max_width:
                        new_h = max(1, int(round(ch * (max_width / cw))))
                        pb = pb.scale_simple(max_width, new_h, 2)
                    ok, buf = pb.save_to_bufferv("jpeg", ["quality"], [str(quality)])
                    if ok and buf:
                        jpg_bytes = bytes(buf)
        except Exception:
            jpg_bytes = None
        finally:
            if old_disp is not None:
                os.environ["DISPLAY"] = old_disp

    # Reliable fallback: ffmpeg x11grab
    if jpg_bytes is None:
        tmp_path = None
        try:
            with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tf:
                tmp_path = tf.name
            scale_arg = f"scale='min({max_width},iw)':-2"
            rc, _, _ = run_cmd_sync(
                [
                    "ffmpeg",
                    "-y",
                    "-loglevel",
                    "error",
                    "-video_size",
                    f"{rw}x{rh}",
                    "-f",
                    "x11grab",
                    "-i",
                    f"{display}+0,{ry}" if rx == 0 else f"{display}+{rx},{ry}",
                    "-vf",
                    scale_arg,
                    "-vframes",
                    "1",
                    "-q:v",
                    "5",
                    tmp_path,
                ],
                display=display,
                timeout=4.0,
            )
            if rc == 0 and tmp_path and os.path.exists(tmp_path):
                jpg_bytes = Path(tmp_path).read_bytes()
        except Exception:
            jpg_bytes = None
        finally:
            if tmp_path:
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass

    return {
        "ok": jpg_bytes is not None,
        "jpeg_bytes": jpg_bytes or b"",
        "capture_rect": (rx, ry, rw, rh),
        "monitors": monitors,
        "active_window": active_win,
        "windows": [
            {"id": w["id"], "title": w["title"], "workspace": w["workspace"], "active": w["active"]}
            for w in wins
        ],
    }


# ---------------------------------------------------------------------------
# Antigravity (agy) Discovery & Helpers
# ---------------------------------------------------------------------------

def find_agy_cli() -> str | None:
    for cand in (
        shutil.which("agy"),
        str(Path.home() / ".local/bin/agy"),
        "/usr/local/bin/agy",
        "/usr/bin/agy",
    ):
        if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def find_antigravity_desktop_bin() -> str | None:
    for cand in (
        shutil.which("antigravity"),
        "/usr/local/bin/antigravity",
        "/usr/bin/antigravity",
        str(Path.home() / "antigravity/Antigravity/Antigravity-x64/antigravity"),
    ):
        if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def detect_agy_backend() -> str:
    if find_agy_cli():
        return "agy_cli"
    if importlib.util.find_spec("google.antigravity") is not None:
        return "antigravity_sdk"
    if find_antigravity_desktop_bin():
        return "desktop_app"
    return "none"


def resolve_agy_workspace(workspace: str = "") -> Path:
    if workspace and Path(workspace).expanduser().is_dir():
        return Path(workspace).expanduser().resolve()
    lunar_de_dir = Path.home() / "Lunar_DE"
    if lunar_de_dir.is_dir():
        return lunar_de_dir
    return Path.home()


# ---------------------------------------------------------------------------
# Wallpaper & Desktop Control Helpers
# ---------------------------------------------------------------------------

def find_wallpapers() -> list[Path]:
    dirs = [
        Path("/usr/share/lunar-de/assets/wallpapers"),
        Path("/usr/local/share/lunar-de/assets/wallpapers"),
        Path(__file__).resolve().parent.parent / "assets" / "wallpapers",
        Path("/usr/share/backgrounds"),
        Path.home() / "Pictures",
        Path.home() / "Pictures" / "Wallpapers",
        get_config_dir() / "wallpapers",
    ]
    seen_names: set[str] = set()
    all_wps: list[Path] = []
    for d in dirs:
        if not d.is_dir():
            continue
        try:
            items = sorted(d.iterdir())
        except OSError:
            continue
        for p in items:
            if p.is_file() and p.suffix.lower() in (".jpg", ".jpeg", ".png") and p.name not in seen_names:
                seen_names.add(p.name)
                all_wps.append(p.resolve())
    return all_wps


def _set_wallpaper(query: str, display: str) -> dict[str, Any]:
    q = query.strip()
    cfg_dir = get_config_dir()
    cfg_dir.mkdir(parents=True, exist_ok=True)
    if q.upper() == "CYCLE_ALL" or q.lower() == "cycle":
        (cfg_dir / "wallpaper").write_text("CYCLE_ALL\n", encoding="utf-8")
        target_str = "CYCLE_ALL"
    else:
        all_wps = find_wallpapers()
        matched: Path | None = Path(q).resolve() if q and Path(q).is_file() else None
        if not matched and q:
            q_low = q.lower().replace("_", " ").replace("-", " ")
            for p in all_wps:
                stem = p.stem.lower().replace("_", " ").replace("-", " ")
                if q_low in stem:
                    matched = p
                    break
        if not matched:
            return {"ok": False, "error": f"No wallpaper matching '{q}' found."}
        target_str = str(matched)
        (cfg_dir / "wallpaper").write_text(target_str + "\n", encoding="utf-8")

    # Notify lunar-wm (kWmReloadWallpaper = 2) and lunar-panel (kPanelReloadWallpaper = 6)
    send_x11_client_message(display, 0, "_LUNAR_WM_ACTION", 2, propagate_to_root=True)
    panel_win = get_root_window_prop_win(display, "_LUNAR_PANEL_WINDOW")
    if panel_win:
        send_x11_client_message(display, panel_win, "_LUNAR_PANEL_ACTION", 6)
    return {"ok": True, "wallpaper": target_str}


def _normalize_key_chord(key_spec: str) -> str:
    parts = [p.strip() for p in re.split(r"\+", key_spec) if p.strip()]
    mapped = [_KEY_ALIASES.get(p.lower(), p) for p in parts]
    return "+".join(mapped) if mapped else key_spec


# ---------------------------------------------------------------------------
# Daemon Class
# ---------------------------------------------------------------------------

class LunarLiveDaemon:
    def __init__(self) -> None:
        self.running = True
        self.wake_enabled = False
        self.vision_enabled = False
        self.live_requested = False
        self.stop_live_flag = False
        self.reload_key_flag = False

        self.user_text = ""
        self.model_text = ""
        self.agy_status = ""
        self.active_tool = ""
        self.status = "idle"
        self.last_error = ""

        self.pending_initial_prompt = ""
        self.last_capture_rect: tuple[int, int, int, int] = (0, 0, 1920, 1080)

        self._ws: Any = None
        self._ws_lock = asyncio.Lock()
        self._agy_tasks: dict[str, dict[str, Any]] = {}
        self._agy_seq = 0
        self._last_written_key: tuple[Any, ...] | None = None
        self._last_write_time = 0.0

    def sync_state(self, force: bool = False) -> None:
        u_txt = self.user_text[-180:]
        m_txt = self.model_text[-220:]
        agy_txt = self.agy_status[-80:]
        tool_txt = self.active_tool[-80:]
        err_txt = self.last_error[-160:]

        key = (
            self.status,
            self.wake_enabled,
            self.vision_enabled,
            u_txt,
            m_txt,
            agy_txt,
            tool_txt,
            err_txt,
        )
        if not force and self._last_written_key == key:
            return

        now = time.monotonic()
        non_transcript_changed = (
            self._last_written_key is None
            or (
                key[0],
                key[1],
                key[2],
                key[5],
                key[6],
                key[7],
            )
            != (
                self._last_written_key[0],
                self._last_written_key[1],
                self._last_written_key[2],
                self._last_written_key[5],
                self._last_written_key[6],
                self._last_written_key[7],
            )
        )
        if not force and not non_transcript_changed and (now - self._last_write_time) < 0.08:
            return

        payload = {
            "status": self.status,
            "wake_enabled": self.wake_enabled,
            "vision_enabled": self.vision_enabled,
            "user_text": u_txt,
            "model_text": m_txt,
            "agy_status": agy_txt,
            "active_tool": tool_txt,
            "error": err_txt,
            "ts": time.time(),
        }
        tmp = STATE_PATH.with_name(f".{STATE_PATH.name}.{os.getpid()}.tmp")
        try:
            tmp.write_text(json.dumps(payload), encoding="utf-8")
            tmp.replace(STATE_PATH)
            self._last_written_key = key
            self._last_write_time = now
        except OSError:
            pass

    async def ws_send_json(self, obj: dict[str, Any]) -> bool:
        ws = self._ws
        if ws is None:
            return False
        data = json.dumps(obj)
        async with self._ws_lock:
            if self._ws is None:
                return False
            await self._ws.send(data)
            return True

    def poll_commands(self) -> None:
        if not CMD_PATH.exists():
            return
        try:
            cmds = CMD_PATH.read_text(encoding="utf-8").splitlines()
            CMD_PATH.unlink(missing_ok=True)
        except Exception:
            return
        for raw in cmds:
            cmd = raw.strip()
            if not cmd:
                continue
            if cmd == "toggle_live":
                if self.status in ("connecting", "live", "speaking"):
                    self.stop_live_flag = True
                else:
                    self.live_requested = True
            elif cmd == "start_live":
                if self.status not in ("connecting", "live", "speaking"):
                    self.live_requested = True
            elif cmd == "stop_live":
                self.stop_live_flag = True
            elif cmd == "toggle_vision":
                self.vision_enabled = not self.vision_enabled
                self.sync_state(force=True)
            elif cmd == "vision_on":
                self.vision_enabled = True
                self.sync_state(force=True)
            elif cmd == "vision_off":
                self.vision_enabled = False
                self.sync_state(force=True)
            elif cmd == "toggle_wake":
                self.wake_enabled = not self.wake_enabled
                self.sync_state(force=True)
            elif cmd == "reload_key":
                self.reload_key_flag = True
            elif cmd.startswith("prompt:"):
                text = cmd.split(":", 1)[1].strip()
                if text:
                    if self.status in ("live", "speaking") and self._ws is not None:
                        asyncio.create_task(self.send_user_text_turn(text))
                    else:
                        self.pending_initial_prompt = text
                        self.live_requested = True
            elif cmd == "quit":
                self.running = False
                self.stop_live_flag = True

    async def send_user_text_turn(self, text: str) -> None:
        self.user_text = text
        self.sync_state(force=True)
        try:
            await self.ws_send_json(
                {
                    "clientContent": {
                        "turns": [{"role": "user", "parts": [{"text": text}]}],
                        "turnComplete": True,
                    }
                }
            )
        except Exception:
            pass

    async def push_video_frame(self, jpg_bytes: bytes) -> bool:
        if not jpg_bytes or self._ws is None:
            return False
        b64 = base64.b64encode(jpg_bytes).decode("ascii")
        try:
            return await self.ws_send_json(
                {
                    "realtimeInput": {
                        "video": {"mimeType": "image/jpeg", "data": b64}
                    }
                }
            )
        except Exception:
            return False

    # ------------------------------------------------------------------
    # Antigravity (agy) Background Execution
    # ------------------------------------------------------------------

    async def _run_agy_background(
        self,
        task_id: str,
        prompt: str,
        workspace: Path,
        backend: str,
        mode: str,
        continue_session: bool,
    ) -> None:
        info = self._agy_tasks[task_id]
        try:
            output_text = ""
            ok = False
            if backend == "agy_cli":
                agy_bin = find_agy_cli()
                if not agy_bin:
                    raise RuntimeError("agy CLI disappeared from PATH")
                cmd = [
                    agy_bin,
                    "-p",
                    prompt,
                    "--output-format",
                    "text",
                    "--mode",
                    mode or "auto",
                ]
                if continue_session:
                    cmd.append("-c")
                proc = await asyncio.create_subprocess_exec(
                    *cmd,
                    cwd=str(workspace),
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                )
                try:
                    stdout_b, stderr_b = await asyncio.wait_for(proc.communicate(), timeout=180.0)
                except asyncio.TimeoutError:
                    proc.kill()
                    await proc.wait()
                    raise RuntimeError("Antigravity timed out after 180s")
                out_s = stdout_b.decode("utf-8", errors="replace").strip()
                err_s = stderr_b.decode("utf-8", errors="replace").strip()
                ok = proc.returncode == 0 and bool(out_s or not err_s)
                output_text = out_s or err_s or f"Exit code {proc.returncode}"
            elif backend == "antigravity_sdk":
                from google.antigravity import Agent, CapabilitiesConfig, LocalAgentConfig  # type: ignore

                cfg = LocalAgentConfig(capabilities=CapabilitiesConfig())
                chunks: list[str] = []
                async with Agent(cfg) as agent:
                    resp = await agent.chat(prompt)
                    async for tok in resp:
                        chunks.append(str(tok))
                output_text = "".join(chunks).strip()
                ok = bool(output_text)

            AGY_LAST_OUTPUT_PATH.write_text(output_text + "\n", encoding="utf-8")
            info["status"] = "done" if ok else "error"
            info["output"] = output_text
            info["finished_at"] = time.time()
            self.agy_status = "AGY: done" if ok else "AGY: error"
            self.sync_state(force=True)

            if self._ws is not None and self.status in ("live", "speaking"):
                snippet = output_text[:900]
                notice = (
                    f"[Antigravity task {task_id} completed ({info['status']})]\n"
                    f"{snippet}\n"
                    "Briefly tell the user the result in 1-2 short sentences."
                )
                await self.ws_send_json(
                    {
                        "clientContent": {
                            "turns": [{"role": "user", "parts": [{"text": notice}]}],
                            "turnComplete": True,
                        }
                    }
                )
        except Exception as exc:
            info["status"] = "error"
            info["output"] = str(exc)
            info["finished_at"] = time.time()
            self.agy_status = "AGY: error"
            self.sync_state(force=True)
            if self._ws is not None and self.status in ("live", "speaking"):
                try:
                    await self.ws_send_json(
                        {
                            "clientContent": {
                                "turns": [
                                    {
                                        "role": "user",
                                        "parts": [
                                            {
                                                "text": (
                                                    f"[Antigravity task {task_id} failed: {exc}] "
                                                    "Briefly let the user know."
                                                )
                                            }
                                        ],
                                    }
                                ],
                                "turnComplete": True,
                            }
                        }
                    )
                except Exception:
                    pass

    # ------------------------------------------------------------------
    # Tool Dispatcher
    # ------------------------------------------------------------------

    async def execute_tool_call_async(self, name: str, args: dict[str, Any]) -> dict[str, Any]:
        disp = resolve_display()
        self.active_tool = name
        self.sync_state(force=True)
        try:
            return await self._dispatch_tool(name, args or {}, disp)
        finally:
            self.active_tool = ""
            self.sync_state(force=True)

    async def _dispatch_tool(self, name: str, args: dict[str, Any], disp: str) -> dict[str, Any]:
        if name == "open_app":
            query = str(args.get("name", "")).strip()
            entry = await asyncio.to_thread(APP_INDEX.find_best, query)
            if not entry:
                # Try forcing cache refresh once in case app was newly installed
                await asyncio.to_thread(APP_INDEX.get_entries, True)
                entry = await asyncio.to_thread(APP_INDEX.find_best, query)
            if not entry:
                return {"ok": False, "error": f"No installed application matching '{query}'."}
            cmd = inject_flatpak_x11_flags(entry.exec_cmd, disp)
            if entry.terminal:
                cmd = f"x-terminal-emulator -e sh -c {shlex.quote(cmd)}"
            work = Path(entry.working_dir) if entry.working_dir else None
            await asyncio.to_thread(spawn_detached, cmd, work, disp)
            return {"ok": True, "launched": entry.name}

        if name == "close_app":
            target = str(args.get("target", "")).strip()
            force = bool(args.get("force", False))
            win = await asyncio.to_thread(resolve_window, target, disp)
            if not win:
                return {"ok": False, "error": f"No open window matching '{target}'."}
            wid = str(win["id_int"])
            if force:
                await asyncio.to_thread(run_cmd_sync, ["xdotool", "windowkill", wid], disp, 3.0)
                if win.get("pid", 0) > 1:
                    try:
                        os.kill(int(win["pid"]), signal.SIGTERM)
                    except OSError:
                        pass
                return {"ok": True, "force_closed": win["title"], "window_id": win["id"]}
            send_x11_client_message(disp, win["id_int"], "_NET_CLOSE_WINDOW", 0, 2, propagate_to_root=True)
            return {"ok": True, "closed": win["title"], "window_id": win["id"]}

        if name == "list_windows":
            return await asyncio.to_thread(query_windows, disp)

        if name == "manage_window":
            target = str(args.get("target", "active")).strip()
            action = str(args.get("action", "")).strip()
            win = await asyncio.to_thread(resolve_window, target, disp)
            if not win:
                return {"ok": False, "error": f"No open window matching '{target}'."}
            wid_int = int(win["id_int"])
            wid_s = str(wid_int)
            if action == "focus":
                send_x11_client_message(disp, wid_int, "_NET_ACTIVE_WINDOW", 2, 0, propagate_to_root=True)
            elif action == "close":
                send_x11_client_message(disp, wid_int, "_NET_CLOSE_WINDOW", 0, 2, propagate_to_root=True)
            elif action == "minimize":
                await asyncio.to_thread(run_cmd_sync, ["xdotool", "windowminimize", wid_s], disp, 3.0)
            elif action in ("maximize", "unmaximize", "fullscreen"):
                d0 = 0 if action == "unmaximize" else 1
                if action == "fullscreen":
                    atom_pair = ("_NET_WM_STATE_FULLSCREEN", None)
                else:
                    atom_pair = ("_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT")
                await asyncio.to_thread(
                    send_x11_client_message,
                    disp,
                    wid_int,
                    "_NET_WM_STATE",
                    d0,
                    0,
                    0,
                    2,
                    True,
                    atom_pair,
                )
            elif action in ("snap_left", "snap_right", "move_to_workspace_1", "move_to_workspace_2"):
                send_x11_client_message(disp, wid_int, "_NET_ACTIVE_WINDOW", 2, 0, propagate_to_root=True)
                await asyncio.sleep(0.08)
                key_map = {
                    "snap_left": "Super_L+Left",
                    "snap_right": "Super_L+Right",
                    "move_to_workspace_1": "Super_L+Shift+1",
                    "move_to_workspace_2": "Super_L+Shift+2",
                }
                await asyncio.to_thread(run_cmd_sync, ["xdotool", "key", key_map[action]], disp, 3.0)
            else:
                return {"ok": False, "error": f"Unknown window action '{action}'."}
            return {"ok": True, "window": win["title"], "window_id": win["id"], "action": action}

        if name == "view_screen":
            target = str(args.get("target", "active_monitor")).strip() or "active_monitor"
            window_name = str(args.get("window_name", "")).strip()
            cap = await asyncio.to_thread(capture_screen_jpeg, disp, target, window_name, 1280, 72)
            self.last_capture_rect = tuple(cap["capture_rect"])  # type: ignore[arg-type]
            if not cap["ok"]:
                return {"ok": False, "error": "Failed to capture screen."}
            await self.push_video_frame(cap["jpeg_bytes"])
            return {
                "ok": True,
                "capture_rect": list(cap["capture_rect"]),
                "monitors": cap["monitors"],
                "active_window": cap["active_window"],
                "windows": cap["windows"],
                "note": "Current screen frame sent to vision. Clicks with norm_x/norm_y (0-1000) map onto capture_rect.",
            }

        if name == "set_screen_vision":
            enabled = bool(args.get("enabled", False))
            self.vision_enabled = enabled
            self.sync_state(force=True)
            if enabled:
                cap = await asyncio.to_thread(capture_screen_jpeg, disp, "active_monitor", "", 960, 65)
                self.last_capture_rect = tuple(cap["capture_rect"])  # type: ignore[arg-type]
                if cap["ok"]:
                    await self.push_video_frame(cap["jpeg_bytes"])
            return {"ok": True, "vision_enabled": self.vision_enabled}

        if name == "interact_in_app":
            action = str(args.get("action", "")).strip()
            target_win = str(args.get("target_window", "")).strip()
            if target_win:
                win = await asyncio.to_thread(resolve_window, target_win, disp)
                if not win:
                    return {"ok": False, "error": f"Target window '{target_win}' not found."}
                send_x11_client_message(disp, int(win["id_int"]), "_NET_ACTIVE_WINDOW", 2, 0, propagate_to_root=True)
                await asyncio.sleep(0.08)

            px: int | None = None
            py: int | None = None
            if args.get("norm_x") is not None and args.get("norm_y") is not None:
                rx, ry, rw, rh = self.last_capture_rect
                nx = max(0.0, min(1000.0, float(args["norm_x"])))
                ny = max(0.0, min(1000.0, float(args["norm_y"])))
                px = int(round(rx + (nx / 1000.0) * rw))
                py = int(round(ry + (ny / 1000.0) * rh))
            elif args.get("x") is not None and args.get("y") is not None:
                px = int(args["x"])
                py = int(args["y"])

            btn_map = {"left": "1", "middle": "2", "right": "3"}
            btn_code = btn_map.get(str(args.get("button", "left")).lower(), "1")

            if action == "click":
                if px is not None and py is not None:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "mousemove", str(px), str(py), "click", btn_code], disp, 3.0)
                else:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "click", btn_code], disp, 3.0)
                return {"ok": True, "action": action, "x": px, "y": py}

            if action == "double_click":
                if px is not None and py is not None:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "mousemove", str(px), str(py)], disp, 2.0)
                await asyncio.to_thread(run_cmd_sync, ["xdotool", "click", "--repeat", "2", "--delay", "60", "1"], disp, 3.0)
                return {"ok": True, "action": action, "x": px, "y": py}

            if action == "right_click":
                if px is not None and py is not None:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "mousemove", str(px), str(py), "click", "3"], disp, 3.0)
                else:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "click", "3"], disp, 3.0)
                return {"ok": True, "action": action, "x": px, "y": py}

            if action == "type":
                text = str(args.get("text", ""))
                if not text:
                    return {"ok": False, "error": "Missing text for action='type'."}
                if ("\n" in text or not text.isascii()) and shutil.which("xclip"):
                    await asyncio.to_thread(
                        run_cmd_sync,
                        ["xclip", "-selection", "clipboard"],
                        disp,
                        3.0,
                        text,
                    )
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "key", "--clearmodifiers", "ctrl+v"], disp, 3.0)
                else:
                    await asyncio.to_thread(
                        run_cmd_sync,
                        ["xdotool", "type", "--clearmodifiers", "--delay", "12", "--", text],
                        disp,
                        6.0,
                    )
                return {"ok": True, "action": "type", "chars": len(text)}

            if action == "key":
                key_raw = str(args.get("key", "")).strip()
                if not key_raw:
                    return {"ok": False, "error": "Missing key for action='key'."}
                chord = _normalize_key_chord(key_raw)
                await asyncio.to_thread(run_cmd_sync, ["xdotool", "key", "--clearmodifiers", chord], disp, 3.0)
                return {"ok": True, "action": "key", "key": chord}

            if action in ("scroll_up", "scroll_down"):
                amt = max(1, min(20, int(args.get("scroll_amount", 3))))
                if px is not None and py is not None:
                    await asyncio.to_thread(run_cmd_sync, ["xdotool", "mousemove", str(px), str(py)], disp, 2.0)
                scroll_btn = "4" if action == "scroll_up" else "5"
                await asyncio.to_thread(
                    run_cmd_sync,
                    ["xdotool", "click", "--repeat", str(amt), "--delay", "25", scroll_btn],
                    disp,
                    3.0,
                )
                return {"ok": True, "action": action, "amount": amt}

            return {"ok": False, "error": f"Unknown interact_in_app action '{action}'."}

        if name == "open_url":
            raw_url = str(args.get("url", "")).strip()
            if not raw_url:
                return {"ok": False, "error": "Empty URL."}
            if "://" not in raw_url:
                if " " in raw_url or "." not in raw_url:
                    url = "https://www.google.com/search?q=" + urllib.parse.quote_plus(raw_url)
                else:
                    url = "https://" + raw_url
            else:
                url = raw_url
            await asyncio.to_thread(spawn_detached, ["xdg-open", url], None, disp)
            return {"ok": True, "opened": url}

        if name == "media_control":
            action = str(args.get("action", "")).strip()
            if action == "set_volume":
                pct = max(0, min(100, int(args.get("percent", 50))))
                await asyncio.to_thread(run_cmd_sync, ["pactl", "set-sink-volume", "@DEFAULT_SINK@", f"{pct}%"], disp, 3.0)
                return {"ok": True, "action": action, "percent": pct}
            if action in ("volume_up", "volume_down"):
                delta = "+8%" if action == "volume_up" else "-8%"
                await asyncio.to_thread(run_cmd_sync, ["pactl", "set-sink-volume", "@DEFAULT_SINK@", delta], disp, 3.0)
                _, vol, _ = await asyncio.to_thread(run_cmd_sync, ["pactl", "get-sink-volume", "@DEFAULT_SINK@"], disp, 2.0)
                return {"ok": True, "action": action, "volume": vol}
            if action in ("mute", "unmute"):
                await asyncio.to_thread(
                    run_cmd_sync,
                    ["pactl", "set-sink-mute", "@DEFAULT_SINK@", "1" if action == "mute" else "0"],
                    disp,
                    3.0,
                )
                return {"ok": True, "action": action}

            mpris_map = {
                "play_pause": ("PlayPause", "XF86AudioPlay"),
                "next": ("Next", "XF86AudioNext"),
                "previous": ("Previous", "XF86AudioPrev"),
                "stop": ("Stop", "XF86AudioStop"),
            }
            if action in mpris_map:
                method, xkey = mpris_map[action]

                def _mpris_or_key() -> str:
                    rc, names_out, _ = run_cmd_sync(
                        [
                            "dbus-send",
                            "--session",
                            "--dest=org.freedesktop.DBus",
                            "--type=method_call",
                            "--print-reply",
                            "/org/freedesktop/DBus",
                            "org.freedesktop.DBus.ListNames",
                        ],
                        disp,
                        2.0,
                    )
                    players = re.findall(r"org\.mpris\.MediaPlayer2\.[^\s\"']+", names_out) if rc == 0 else []
                    if players:
                        for p in players:
                            run_cmd_sync(
                                [
                                    "dbus-send",
                                    "--session",
                                    f"--dest={p}",
                                    "--type=method_call",
                                    "/org/mpris/MediaPlayer2",
                                    f"org.mpris.MediaPlayer2.Player.{method}",
                                ],
                                disp,
                                2.0,
                            )
                        return f"mpris:{players[0]}"
                    run_cmd_sync(["xdotool", "key", xkey], disp, 2.0)
                    return f"key:{xkey}"

                via = await asyncio.to_thread(_mpris_or_key)
                return {"ok": True, "action": action, "via": via}
            return {"ok": False, "error": f"Unknown media action '{action}'."}

        if name == "desktop_control":
            action = str(args.get("action", "")).strip()
            val = str(args.get("value", "")).strip()
            if action == "switch_workspace":
                idx = 1 if val in ("2", "workspace 2", "second", "next") else 0
                if val in ("1", "0", "workspace 1", "first"):
                    idx = 0
                send_x11_client_message(disp, 0, "_NET_CURRENT_DESKTOP", idx, 0, propagate_to_root=True)
                return {"ok": True, "workspace": idx + 1}
            if action == "set_wallpaper":
                return await asyncio.to_thread(_set_wallpaper, val, disp)
            if action == "set_bar_layout":
                layout = val.lower()
                if layout not in ("top", "taskbar", "taskbar-top"):
                    return {"ok": False, "error": "Layout must be 'top', 'taskbar', or 'taskbar-top'."}
                cfg_dir = get_config_dir()
                cfg_dir.mkdir(parents=True, exist_ok=True)
                (cfg_dir / "panel_layout").write_text(layout + "\n", encoding="utf-8")
                return {"ok": True, "layout": layout, "note": "Saved panel_layout."}
            if action == "open_terminal":
                await asyncio.to_thread(spawn_detached, "x-terminal-emulator || gnome-terminal", None, disp)
                return {"ok": True, "launched": "terminal"}
            if action == "open_display_settings":
                tool = "/usr/bin/lunar-displays" if Path("/usr/bin/lunar-displays").exists() else "lunar-displays"
                await asyncio.to_thread(spawn_detached, [tool], None, disp)
                return {"ok": True, "launched": "lunar-displays"}
            if action == "open_settings":
                await asyncio.to_thread(spawn_detached, "XDG_CURRENT_DESKTOP=GNOME gnome-control-center", None, disp)
                return {"ok": True, "launched": "gnome-control-center"}
            if action == "get_system_info":
                mons = await asyncio.to_thread(parse_monitors, disp)
                _, mem, _ = await asyncio.to_thread(run_cmd_sync, ["free", "-h"], disp, 2.0)
                _, vol, _ = await asyncio.to_thread(run_cmd_sync, ["pactl", "get-sink-volume", "@DEFAULT_SINK@"], disp, 2.0)
                disk = shutil.disk_usage("/")
                return {
                    "ok": True,
                    "display": disp,
                    "monitors": mons,
                    "memory": mem,
                    "disk_free_gb": round(disk.free / (1024**3), 1),
                    "volume": vol,
                }
            if action == "get_clipboard":
                rc, out, err = await asyncio.to_thread(run_cmd_sync, ["xclip", "-o", "-selection", "clipboard"], disp, 2.0)
                if rc != 0:
                    return {"ok": False, "error": err or "Clipboard empty or xclip failed."}
                return {"ok": True, "clipboard": out[:2000]}
            if action == "set_clipboard":
                rc, _, err = await asyncio.to_thread(run_cmd_sync, ["xclip", "-selection", "clipboard"], disp, 2.0, val)
                return {"ok": rc == 0, "error": err} if rc != 0 else {"ok": True, "chars": len(val)}
            if action == "run_command":
                if not val:
                    return {"ok": False, "error": "Empty command."}
                proc = await asyncio.create_subprocess_shell(
                    val,
                    cwd=str(Path.home()),
                    env=sanitize_app_env(disp),
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                )
                try:
                    out_b, err_b = await asyncio.wait_for(proc.communicate(), timeout=10.0)
                except asyncio.TimeoutError:
                    proc.kill()
                    await proc.wait()
                    return {"ok": False, "error": "Command timed out after 10s."}
                return {
                    "ok": proc.returncode == 0,
                    "returncode": proc.returncode,
                    "stdout": out_b.decode("utf-8", errors="replace")[:1500],
                    "stderr": err_b.decode("utf-8", errors="replace")[:800],
                }
            return {"ok": False, "error": f"Unknown desktop action '{action}'."}

        if name == "open_antigravity":
            ws_arg = str(args.get("workspace", "")).strip()
            win = await asyncio.to_thread(resolve_window, "antigravity", disp)
            if win and not ws_arg:
                send_x11_client_message(disp, int(win["id_int"]), "_NET_ACTIVE_WINDOW", 2, 0, propagate_to_root=True)
                return {"ok": True, "focused": win["title"]}
            app_bin = find_antigravity_desktop_bin()
            if not app_bin:
                return {"ok": False, "error": "Antigravity binary not found."}
            cmd = [app_bin]
            if ws_arg:
                cmd.append(str(Path(ws_arg).expanduser()))
            await asyncio.to_thread(spawn_detached, cmd, resolve_agy_workspace(ws_arg), disp)
            return {"ok": True, "launched": app_bin, "workspace": str(resolve_agy_workspace(ws_arg))}

        if name == "ask_antigravity":
            prompt = str(args.get("prompt", "")).strip()
            if not prompt:
                return {"ok": False, "error": "Missing prompt for ask_antigravity."}
            ws_path = resolve_agy_workspace(str(args.get("workspace", "")))
            mode = str(args.get("mode", "auto")).strip() or "auto"
            cont = bool(args.get("continue_session", False))
            backend = detect_agy_backend()

            if backend in ("agy_cli", "antigravity_sdk"):
                self._agy_seq += 1
                tid = f"agy-{self._agy_seq}"
                self._agy_tasks[tid] = {
                    "id": tid,
                    "prompt": prompt,
                    "workspace": str(ws_path),
                    "backend": backend,
                    "status": "running",
                    "output": "",
                    "started_at": time.time(),
                }
                self.agy_status = "AGY: working..."
                self.sync_state(force=True)
                task = asyncio.create_task(
                    self._run_agy_background(tid, prompt, ws_path, backend, mode, cont)
                )
                self._agy_tasks[tid]["_task"] = task
                return {
                    "ok": True,
                    "status": "started",
                    "task_id": tid,
                    "backend": backend,
                    "workspace": str(ws_path),
                    "note": "Antigravity is working in the background. Tell the user briefly.",
                }

            # Fallback: focus/launch Antigravity desktop IDE and copy prompt to clipboard
            if shutil.which("xclip"):
                await asyncio.to_thread(run_cmd_sync, ["xclip", "-selection", "clipboard"], disp, 2.0, prompt)
            win = await asyncio.to_thread(resolve_window, "antigravity", disp)
            if win:
                send_x11_client_message(disp, int(win["id_int"]), "_NET_ACTIVE_WINDOW", 2, 0, propagate_to_root=True)
            else:
                app_bin = find_antigravity_desktop_bin()
                if app_bin:
                    await asyncio.to_thread(spawn_detached, [app_bin, str(ws_path)], ws_path, disp)
            self.agy_status = "AGY: copied to app"
            self.sync_state(force=True)
            return {
                "ok": True,
                "backend": "desktop_app",
                "agy_cli_installed": False,
                "message": (
                    "Focused Antigravity and copied the task prompt to the clipboard so you can paste with Ctrl+V. "
                    "For hands-free background execution, install the agy CLI: "
                    "curl -fsSL https://antigravity.google/cli/install.sh | bash"
                ),
            }

        if name == "get_antigravity_status":
            tid = str(args.get("task_id", "")).strip()
            backend = detect_agy_backend()
            if tid and tid in self._agy_tasks:
                tinfo = {k: v for k, v in self._agy_tasks[tid].items() if k != "_task"}
                return {"ok": True, "backend": backend, "agy_cli": find_agy_cli(), "task": tinfo}
            recent = [
                {k: v for k, v in item.items() if k != "_task"}
                for item in list(self._agy_tasks.values())[-5:]
            ]
            last_file = ""
            if AGY_LAST_OUTPUT_PATH.exists():
                try:
                    last_file = AGY_LAST_OUTPUT_PATH.read_text(encoding="utf-8", errors="replace")[-1000:].strip()
                except OSError:
                    pass
            return {
                "ok": True,
                "backend": backend,
                "agy_cli": find_agy_cli(),
                "recent_tasks": recent,
                "last_output": last_file,
            }

        return {"ok": False, "error": f"Unknown tool '{name}'"}

    # ------------------------------------------------------------------
    # Audio & WebSocket Live Loop
    # ------------------------------------------------------------------

    async def _spawn_audio_pipes(self) -> tuple[asyncio.subprocess.Process, asyncio.subprocess.Process]:
        rec_task = asyncio.create_subprocess_exec(
            "parec",
            "--format=s16le",
            f"--rate={SAMPLE_RATE_IN}",
            "--channels=1",
            "--latency-msec=20",
            stdout=asyncio.subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        play_task = asyncio.create_subprocess_exec(
            "paplay",
            "--raw",
            "--format=s16le",
            f"--rate={SAMPLE_RATE_OUT}",
            "--channels=1",
            "--latency-msec=25",
            stdin=asyncio.subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        return await asyncio.gather(rec_task, play_task)

    async def run_live_session(self, api_key: str, initial_command: str = "") -> None:
        self.stop_live_flag = False
        self.status = "connecting"
        self.last_error = ""
        self.active_tool = ""
        self.sync_state(force=True)

        # Warm PulseAudio/PipeWire pipes concurrently with WS handshake
        audio_pipes_task = asyncio.create_task(self._spawn_audio_pipes())

        ws = None
        last_exc = ""
        for model, thinking_cfg in LIVE_MODELS:
            uri = (
                "wss://generativelanguage.googleapis.com/ws/"
                f"google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={api_key}"
            )
            try:
                ws = await websockets.connect(uri, max_size=8 * 1024 * 1024, open_timeout=6.0)
                await ws.send(json.dumps(build_setup_message(model, thinking_cfg)))
                first = json.loads(await asyncio.wait_for(ws.recv(), timeout=6.0))
                if "setupComplete" in first:
                    break
                await ws.close()
                ws = None
            except Exception as exc:
                last_exc = str(exc)
                if ws:
                    try:
                        await ws.close()
                    except Exception:
                        pass
                    ws = None

        if not ws:
            rec_proc, play_proc = await audio_pipes_task
            for p in (rec_proc, play_proc):
                try:
                    p.terminate()
                except ProcessLookupError:
                    pass
            self.status = "error"
            self.last_error = last_exc or "Failed to connect to Gemini Live"
            self.sync_state(force=True)
            await asyncio.sleep(2.0)
            return

        self._ws = ws
        rec_proc, play_proc = await audio_pipes_task
        self.status = "live"
        self.sync_state(force=True)
        play_chime_async(rising=True)

        if initial_command:
            await self.send_user_text_turn(initial_command)

        audio_out_q: asyncio.Queue[bytes | None] = asyncio.Queue()
        last_activity = time.monotonic()

        async def spawn_fresh_paplay() -> asyncio.subprocess.Process:
            return await asyncio.create_subprocess_exec(
                "paplay",
                "--raw",
                "--format=s16le",
                f"--rate={SAMPLE_RATE_OUT}",
                "--channels=1",
                "--latency-msec=25",
                stdin=asyncio.subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )

        async def send_audio() -> None:
            nonlocal last_activity
            assert rec_proc.stdout is not None
            while not self.stop_live_flag and self.running:
                self.poll_commands()
                if self.stop_live_flag or not self.running:
                    break
                try:
                    chunk = await asyncio.wait_for(rec_proc.stdout.readexactly(CHUNK_BYTES), timeout=0.35)
                except asyncio.TimeoutError:
                    continue
                except asyncio.IncompleteReadError:
                    break

                if pcm_rms(chunk) > 420:
                    last_activity = time.monotonic()
                elif time.monotonic() - last_activity > 28.0:
                    self.stop_live_flag = True
                    break

                b64 = base64.b64encode(chunk).decode("ascii")
                await self.ws_send_json(
                    {
                        "realtimeInput": {
                            "audio": {
                                "mimeType": f"audio/pcm;rate={SAMPLE_RATE_IN}",
                                "data": b64,
                            }
                        }
                    }
                )

        async def play_audio() -> None:
            nonlocal play_proc
            while self.running and not self.stop_live_flag:
                pcm_out = await audio_out_q.get()
                if pcm_out is None:
                    break
                if len(pcm_out) == 0:
                    # Flush sentinel on barge-in: swap to clean paplay pipe
                    try:
                        play_proc.terminate()
                    except ProcessLookupError:
                        pass
                    play_proc = await spawn_fresh_paplay()
                    continue
                if play_proc.stdin is None or play_proc.returncode is not None:
                    play_proc = await spawn_fresh_paplay()
                try:
                    assert play_proc.stdin is not None
                    play_proc.stdin.write(pcm_out)
                    await play_proc.stdin.drain()
                except (BrokenPipeError, ConnectionResetError):
                    play_proc = await spawn_fresh_paplay()

        async def stream_vision() -> None:
            while not self.stop_live_flag and self.running:
                await asyncio.sleep(2.0)
                if self.stop_live_flag or not self.running or not self.vision_enabled:
                    continue
                cap = await asyncio.to_thread(
                    capture_screen_jpeg, resolve_display(), "active_monitor", "", 960, 62
                )
                self.last_capture_rect = tuple(cap["capture_rect"])  # type: ignore[arg-type]
                if cap["ok"]:
                    await self.push_video_frame(cap["jpeg_bytes"])

        async def run_and_reply_tools(fcs: list[dict[str, Any]]) -> None:
            async def _one(fc: dict[str, Any]) -> dict[str, Any]:
                res = await self.execute_tool_call_async(fc.get("name", ""), fc.get("args", {}) or {})
                return {
                    "id": fc.get("id", ""),
                    "name": fc.get("name", ""),
                    "response": res,
                }

            responses = await asyncio.gather(*(_one(fc) for fc in fcs))
            if not self.stop_live_flag and self.running:
                await self.ws_send_json({"toolResponse": {"functionResponses": list(responses)}})

        async def recv_server() -> None:
            nonlocal last_activity
            tool_tasks: set[asyncio.Task[Any]] = set()
            try:
                async for raw in ws:
                    if self.stop_live_flag or not self.running:
                        break
                    msg = json.loads(raw)

                    sc = msg.get("serverContent")
                    if sc:
                        if sc.get("interrupted"):
                            while not audio_out_q.empty():
                                try:
                                    audio_out_q.get_nowait()
                                except asyncio.QueueEmpty:
                                    break
                            audio_out_q.put_nowait(b"")  # flush paplay buffer immediately
                            self.status = "live"
                            self.sync_state(force=True)

                        in_tr = sc.get("inputTranscription", {}).get("text")
                        if in_tr:
                            self.user_text = (self.user_text + " " + in_tr).strip()
                            last_activity = time.monotonic()
                            self.sync_state()

                        out_tr = sc.get("outputTranscription", {}).get("text")
                        if out_tr:
                            self.model_text = (self.model_text + out_tr).strip()
                            last_activity = time.monotonic()
                            self.sync_state()

                        model_turn = sc.get("modelTurn", {})
                        for part in model_turn.get("parts", []):
                            inline = part.get("inlineData")
                            if inline and "data" in inline:
                                if self.status != "speaking":
                                    self.status = "speaking"
                                    self.sync_state(force=True)
                                last_activity = time.monotonic()
                                audio_out_q.put_nowait(base64.b64decode(inline["data"]))

                        if sc.get("turnComplete"):
                            if self.status != "live":
                                self.status = "live"
                                self.sync_state(force=True)
                            last_activity = time.monotonic()

                    tc = msg.get("toolCall")
                    if tc and tc.get("functionCalls"):
                        last_activity = time.monotonic()
                        t = asyncio.create_task(run_and_reply_tools(tc["functionCalls"]))
                        tool_tasks.add(t)
                        t.add_done_callback(tool_tasks.discard)
            finally:
                for t in list(tool_tasks):
                    t.cancel()
                if tool_tasks:
                    await asyncio.gather(*tool_tasks, return_exceptions=True)

        async def watch_stop() -> None:
            while not self.stop_live_flag and self.running:
                self.poll_commands()
                if self.stop_live_flag or not self.running:
                    break
                await asyncio.sleep(0.08)

        tasks = {
            asyncio.create_task(send_audio()),
            asyncio.create_task(play_audio()),
            asyncio.create_task(stream_vision()),
            asyncio.create_task(recv_server()),
            asyncio.create_task(watch_stop()),
        }
        try:
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            self.stop_live_flag = True
            audio_out_q.put_nowait(None)
            for t in pending:
                t.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            for t in done:
                if t.cancelled():
                    continue
                exc = t.exception()
                if exc and not isinstance(exc, asyncio.CancelledError):
                    self.last_error = str(exc)
        except Exception as exc:
            self.last_error = str(exc)
        finally:
            self._ws = None
            for p in (rec_proc, play_proc):
                try:
                    p.terminate()
                except ProcessLookupError:
                    pass
            try:
                await ws.close()
            except Exception:
                pass
            self.stop_live_flag = False
            self.active_tool = ""
            self.status = "idle"
            self.sync_state(force=True)
            play_chime_async(rising=False)

    async def main_loop(self) -> None:
        # Warm desktop app index in background right at startup
        await asyncio.to_thread(APP_INDEX.get_entries, True)
        cached_key = load_api_key()
        while self.running:
            self.poll_commands()
            if self.reload_key_flag or not cached_key:
                self.reload_key_flag = False
                cached_key = load_api_key()

            if not cached_key:
                self.status = "needs_key"
                self.sync_state()
                await asyncio.sleep(0.25)
                continue

            if self.live_requested:
                self.live_requested = False
                init_prompt = self.pending_initial_prompt
                self.pending_initial_prompt = ""
                self.user_text = ""
                self.model_text = ""
                await self.run_live_session(cached_key, initial_command=init_prompt)
                continue

            if self.status != "idle":
                self.status = "idle"
            self.sync_state()
            await asyncio.sleep(0.12)


def main() -> int:
    daemon = LunarLiveDaemon()

    def handle_sig(_sig: int, _frame: Any) -> None:
        daemon.running = False
        daemon.stop_live_flag = True

    def handle_usr1(_sig: int, _frame: Any) -> None:
        if daemon.status in ("connecting", "live", "speaking"):
            daemon.stop_live_flag = True
        else:
            daemon.live_requested = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)
    signal.signal(signal.SIGUSR1, handle_usr1)

    try:
        asyncio.run(daemon.main_loop())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
