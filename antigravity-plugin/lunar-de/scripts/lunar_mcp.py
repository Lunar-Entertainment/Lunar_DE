#!/usr/bin/env python3
"""Lunar DE MCP Server (JSON-RPC 2.0 over stdio) for Antigravity."""

from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "lunar-de", "version": "1.0.1"}

TOOLS: list[dict[str, Any]] = [
    {
        "name": "get_system_info",
        "description": "Get connected RandR monitors, memory/disk usage, audio volume, and active X11 display.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "display": {"type": "string", "description": "Optional X11 display (e.g. :0 or :2). Defaults to active DISPLAY."}
            },
        },
    },
    {
        "name": "list_windows",
        "description": "List visible X11 windows (window ID and title).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "display": {"type": "string", "description": "Optional X11 display (default :0 or current DISPLAY)."}
            },
        },
    },
    {
        "name": "launch_app",
        "description": "Search installed .desktop applications by name and launch the matching app on X11.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "App name or keyword (e.g. brave, files, terminal, discord, settings)."},
                "display": {"type": "string", "description": "Optional X11 display (default current DISPLAY or :0)."},
            },
            "required": ["name"],
        },
    },
    {
        "name": "window_action",
        "description": "Focus, close, maximize, or snap (left/right) a window by title substring or X11 window ID.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "Window title substring or numeric X11 window ID."},
                "action": {
                    "type": "string",
                    "enum": ["focus", "close", "maximize", "snap_left", "snap_right"],
                    "description": "Action to perform on the window.",
                },
                "display": {"type": "string", "description": "Optional X11 display."},
            },
            "required": ["target", "action"],
        },
    },
    {
        "name": "take_screenshot",
        "description": "Capture the active X11 screen to a PNG file and return its path.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "display": {"type": "string", "description": "Optional X11 display (default current DISPLAY or :0)."},
                "output_path": {"type": "string", "description": "Optional output PNG path (defaults to /tmp/lunar-mcp-shot-<ts>.png)."},
            },
        },
    },
    {
        "name": "set_volume",
        "description": "Set system audio volume percentage (0-100) or toggle mute via pactl.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "percent": {"type": "integer", "minimum": 0, "maximum": 100, "description": "Volume percentage 0-100."},
                "mute": {"type": "boolean", "description": "Optional: true to mute, false to unmute."},
            },
        },
    },
    {
        "name": "set_wallpaper",
        "description": "List available wallpapers, switch to a wallpaper by keyword, or enable 'CYCLE_ALL' auto-rotation.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Wallpaper keyword, full path, 'CYCLE_ALL', or 'list' to list available wallpapers."}
            },
            "required": ["query"],
        },
    },
    {
        "name": "set_bar_layout",
        "description": "Switch Lunar DE bar layout between 'top' (compact Top Bar), 'taskbar' (centered bottom taskbar), and 'taskbar-top' (centered top taskbar).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "layout": {"type": "string", "enum": ["top", "taskbar", "taskbar-top"]}
            },
            "required": ["layout"],
        },
    },
    {
        "name": "send_input",
        "description": "Send keystrokes (e.g. 'Super_L+Return', 'Alt+Tab') or a mouse click at (x, y) via xdotool.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Optional xdotool key chord (e.g. 'Super_L+g')."},
                "text": {"type": "string", "description": "Optional text to type."},
                "click_x": {"type": "integer", "description": "Optional X coordinate to click."},
                "click_y": {"type": "integer", "description": "Optional Y coordinate to click."},
                "display": {"type": "string", "description": "Optional X11 display."},
            },
        },
    },
    {
        "name": "gemini_live",
        "description": "Inspect or control the Lunar DE Gemini Live voice assistant (status, start, stop, toggle, toggle_vision, or send_prompt).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["status", "start", "stop", "toggle", "toggle_vision", "send_prompt"],
                    "description": "Action to perform on Gemini Live.",
                },
                "prompt": {
                    "type": "string",
                    "description": "Text prompt to send when action='send_prompt'.",
                },
            },
            "required": ["action"],
        },
    },
]


def resolve_display(arg: str | None) -> str:
    if arg and arg.strip():
        return arg.strip()
    return os.environ.get("DISPLAY") or ":0"


def run_cmd(cmd: list[str], display: str | None = None, timeout: int = 8) -> tuple[int, str, str]:
    env = os.environ.copy()
    if display:
        env["DISPLAY"] = display
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except Exception as exc:
        return 1, "", str(exc)


def cfg_dir() -> Path:
    xdg = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg) if xdg else Path.home() / ".config"
    d = base / "lunar-de"
    d.mkdir(parents=True, exist_ok=True)
    return d


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


def _send_x11_client_message(
    display: str,
    target_win: int,
    atom_name: str,
    d0: int = 0,
    d1: int = 0,
    d2: int = 0,
    propagate_to_root: bool = False,
) -> bool:
    try:
        x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XSendEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_ulong,
            ctypes.c_int,
            ctypes.c_long,
            ctypes.c_void_p,
        ]
        x11.XSendEvent.restype = ctypes.c_int
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XFlush.restype = ctypes.c_int
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.restype = ctypes.c_int
        dpy = x11.XOpenDisplay(display.encode("utf-8"))
        if not dpy:
            return False
        try:
            root = x11.XDefaultRootWindow(dpy)
            win = root if target_win == 0 else target_win
            atom = x11.XInternAtom(dpy, atom_name.encode("utf-8"), 0)
            ev = _XClientMessageEvent()
            ev.type = 33
            ev.display = dpy
            ev.window = win
            ev.message_type = atom
            ev.format = 32
            ev.data.l[0] = d0
            ev.data.l[1] = d1
            ev.data.l[2] = d2
            mask = ((1 << 20) | (1 << 19)) if propagate_to_root else 0
            dest = root if propagate_to_root else win
            x11.XSendEvent(dpy, dest, 0, mask, ctypes.byref(ev))
            x11.XFlush(dpy)
            return True
        finally:
            x11.XCloseDisplay(dpy)
    except Exception:
        return False


def _get_panel_window(display: str) -> int:
    rc, out, _ = run_cmd(["xprop", "-display", display, "-root", "_LUNAR_PANEL_WINDOW"], display=display)
    if rc != 0 or "#" not in out:
        return 0
    m = re.search(r"0x[0-9a-fA-F]+", out.split("#", 1)[1])
    return int(m.group(0), 16) if m else 0


def handle_tool(name: str, args: dict[str, Any]) -> dict[str, Any]:
    disp = resolve_display(args.get("display"))

    if name == "get_system_info":
        _, mons, _ = run_cmd(["xrandr", "--listmonitors"], display=disp)
        _, mem, _ = run_cmd(["free", "-h"])
        _, vol, _ = run_cmd(["pactl", "get-sink-volume", "@DEFAULT_SINK@"])
        _, mute, _ = run_cmd(["pactl", "get-sink-mute", "@DEFAULT_SINK@"])
        disk = shutil.disk_usage("/")
        return {
            "display": disp,
            "monitors": mons,
            "memory": mem,
            "disk_free_gb": round(disk.free / (1024**3), 1),
            "disk_total_gb": round(disk.total / (1024**3), 1),
            "audio_volume": vol,
            "audio_mute": mute,
        }

    if name == "list_windows":
        rc, out, err = run_cmd(["xdotool", "search", "--onlyvisible", "--name", "."], display=disp)
        if rc != 0 and not out:
            return {"display": disp, "windows": [], "note": err or "No visible windows found"}
        wins = []
        for wid in out.splitlines():
            wid = wid.strip()
            if not wid:
                continue
            _, title, _ = run_cmd(["xdotool", "getwindowname", wid], display=disp)
            if title and title not in ("lunar-panel", "lunar-popup", "lunar-wm"):
                wins.append({"id": wid, "title": title})
        return {"display": disp, "windows": wins}

    if name == "launch_app":
        query = str(args.get("name", "")).strip().lower()
        if not query:
            return {"ok": False, "error": "Empty app name"}
        app_dirs = [
            Path.home() / ".local/share/applications",
            Path("/usr/share/applications"),
            Path.home() / ".local/share/flatpak/exports/share/applications",
            Path("/var/lib/flatpak/exports/share/applications"),
            Path("/var/lib/snapd/desktop/applications"),
        ]
        for d in app_dirs:
            if not d.exists():
                continue
            for p in sorted(d.glob("*.desktop")):
                txt = p.read_text(encoding="utf-8", errors="ignore")
                app_name = ""
                exec_cmd = ""
                no_display = False
                hidden = False
                in_main = False
                for raw in txt.splitlines():
                    line = raw.strip()
                    if line.startswith("[") and line.endswith("]"):
                        if line == "[Desktop Entry]":
                            in_main = True
                        elif in_main:
                            break
                        continue
                    if not in_main or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    k, v = k.strip(), v.strip()
                    if k == "Name" and not app_name:
                        app_name = v
                    elif k == "Exec" and not exec_cmd:
                        exec_cmd = v
                    elif k == "NoDisplay":
                        no_display = v.lower() == "true"
                    elif k == "Hidden":
                        hidden = v.lower() == "true"
                if no_display or hidden or not app_name or not exec_cmd:
                    continue
                if query in app_name.lower() or query in p.stem.lower():
                    for tok in ("--file-forwarding", "@@u", "@@", "%U", "%u", "%F", "%f"):
                        exec_cmd = exec_cmd.replace(tok, "")
                    exec_cmd = exec_cmd.strip()
                    if "flatpak run" in exec_cmd:
                        extra = (
                            f" --env=DISPLAY={disp}"
                            " --env=GDK_BACKEND=x11 --env=QT_QPA_PLATFORM=xcb"
                            " --env=ELECTRON_OZONE_PLATFORM_HINT=x11"
                            " --env=NO_AT_BRIDGE=1 --env=GTK_A11Y=none"
                            " --env=XDG_CURRENT_DESKTOP=Lunar"
                            " --unset-env=WAYLAND_DISPLAY --unset-env=GTK_MODULES"
                            " --unset-env=GTK3_MODULES --unset-env=QT_ACCESSIBILITY"
                            " --unset-env=GIO_USE_VFS"
                        )
                        exec_cmd = exec_cmd.replace("flatpak run", f"flatpak run{extra}", 1)
                    env = os.environ.copy()
                    env["DISPLAY"] = disp
                    for var in (
                        "WAYLAND_DISPLAY",
                        "GTK_MODULES",
                        "GTK3_MODULES",
                        "QT_ACCESSIBILITY",
                        "GIO_USE_VFS",
                    ):
                        env.pop(var, None)
                    env["GDK_BACKEND"] = "x11"
                    env["QT_QPA_PLATFORM"] = "xcb"
                    env["ELECTRON_OZONE_PLATFORM_HINT"] = "x11"
                    env["GTK_USE_PORTAL"] = "0"
                    env["NO_AT_BRIDGE"] = "1"
                    env["GTK_A11Y"] = "none"
                    env["XDG_CURRENT_DESKTOP"] = "Lunar"
                    subprocess.Popen(
                        ["/bin/sh", "-c", exec_cmd],
                        env=env,
                        cwd=str(Path.home()),
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        start_new_session=True,
                    )
                    return {"ok": True, "launched": app_name, "display": disp}
        return {"ok": False, "error": f"No application matching '{query}' found."}

    if name == "window_action":
        target = str(args.get("target", "")).strip()
        action = str(args.get("action", "")).strip()
        wid = target if target.isdigit() else ""
        if not wid:
            _, out, _ = run_cmd(["xdotool", "search", "--onlyvisible", "--name", target], display=disp)
            lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
            if not lines:
                return {"ok": False, "error": f"No window matching '{target}'"}
            wid = lines[0]
        if action == "focus":
            run_cmd(["xdotool", "windowactivate", wid], display=disp)
        elif action == "close":
            run_cmd(["xdotool", "windowclose", wid], display=disp)
        elif action == "maximize":
            run_cmd(["xdotool", "windowactivate", "--sync", wid, "key", "Super_L+Up"], display=disp)
        elif action == "snap_left":
            run_cmd(["xdotool", "windowactivate", "--sync", wid, "key", "Super_L+Left"], display=disp)
        elif action == "snap_right":
            run_cmd(["xdotool", "windowactivate", "--sync", wid, "key", "Super_L+Right"], display=disp)
        return {"ok": True, "window_id": wid, "action": action, "display": disp}

    if name == "take_screenshot":
        out_path = str(args.get("output_path") or f"/tmp/lunar-mcp-shot-{int(time.time())}.png")
        _, dim_out, _ = run_cmd(["xdpyinfo", "-display", disp])
        m = re.search(r"dimensions:\s+(\d+x\d+)", dim_out)
        dim = m.group(1) if m else "1920x1080"
        rc, _, err = run_cmd(
            ["ffmpeg", "-y", "-video_size", dim, "-f", "x11grab", "-i", f"{disp}.0", "-vframes", "1", out_path],
            display=disp,
            timeout=10,
        )
        if rc == 0 and Path(out_path).exists():
            return {"ok": True, "path": out_path, "dimensions": dim, "display": disp}
        return {"ok": False, "error": err or "ffmpeg screenshot failed"}

    if name == "set_volume":
        if "percent" in args and args["percent"] is not None:
            pct = max(0, min(100, int(args["percent"])))
            run_cmd(["pactl", "set-sink-volume", "@DEFAULT_SINK@", f"{pct}%"])
        if "mute" in args and args["mute"] is not None:
            run_cmd(["pactl", "set-sink-mute", "@DEFAULT_SINK@", "1" if args["mute"] else "0"])
        _, vol, _ = run_cmd(["pactl", "get-sink-volume", "@DEFAULT_SINK@"])
        return {"ok": True, "volume": vol}

    if name == "set_wallpaper":
        query = str(args.get("query", "")).strip()
        wp_dirs = [
            Path("/usr/share/lunar-de/assets/wallpapers"),
            Path("/usr/local/share/lunar-de/assets/wallpapers"),
            Path("/home/lunar/Lunar_DE/assets/wallpapers"),
            Path("/usr/share/backgrounds"),
            Path.home() / "Pictures",
        ]
        all_wps: list[Path] = []
        seen: set[str] = set()
        for d in wp_dirs:
            if not d.exists():
                continue
            for p in sorted(d.iterdir()):
                if p.is_file() and p.suffix.lower() in (".jpg", ".jpeg", ".png") and p.name not in seen:
                    seen.add(p.name)
                    all_wps.append(p.resolve())
        if query.lower() == "list":
            return {"ok": True, "count": len(all_wps), "wallpapers": [p.name for p in all_wps[:50]]}
        if query.upper() == "CYCLE_ALL":
            (cfg_dir() / "wallpaper").write_text("CYCLE_ALL\n", encoding="utf-8")
            _send_x11_client_message(disp, 0, "_LUNAR_WM_ACTION", 2, propagate_to_root=True)
            panel_win = _get_panel_window(disp)
            if panel_win:
                _send_x11_client_message(disp, panel_win, "_LUNAR_PANEL_ACTION", 6)
            return {"ok": True, "wallpaper": "CYCLE_ALL"}
        matched = Path(query).resolve() if Path(query).exists() else None
        if not matched:
            q_low = query.lower().replace("_", " ")
            for p in all_wps:
                if q_low in p.stem.lower().replace("_", " "):
                    matched = p
                    break
        if matched:
            (cfg_dir() / "wallpaper").write_text(str(matched) + "\n", encoding="utf-8")
            _send_x11_client_message(disp, 0, "_LUNAR_WM_ACTION", 2, propagate_to_root=True)
            panel_win = _get_panel_window(disp)
            if panel_win:
                _send_x11_client_message(disp, panel_win, "_LUNAR_PANEL_ACTION", 6)
            return {"ok": True, "wallpaper": str(matched)}
        return {"ok": False, "error": f"No wallpaper matching '{query}'"}

    if name == "set_bar_layout":
        layout = str(args.get("layout", "top")).strip().lower()
        if layout not in ("top", "taskbar", "taskbar-top"):
            return {"ok": False, "error": "layout must be 'top', 'taskbar', or 'taskbar-top'"}
        (cfg_dir() / "panel_layout").write_text(layout + "\n", encoding="utf-8")
        return {"ok": True, "layout": layout, "note": "Saved to panel_layout."}

    if name == "send_input":
        if args.get("click_x") is not None and args.get("click_y") is not None:
            run_cmd(
                ["xdotool", "mousemove", str(int(args["click_x"])), str(int(args["click_y"])), "click", "1"],
                display=disp,
            )
        if args.get("key"):
            run_cmd(["xdotool", "key", str(args["key"])], display=disp)
        if args.get("text"):
            run_cmd(["xdotool", "type", "--delay", "20", str(args["text"])], display=disp)
        return {"ok": True, "display": disp}

    if name == "gemini_live":
        action = str(args.get("action", "status")).strip()
        state_path = Path("/tmp/lunar-live-state.json")
        cmd_path = Path("/tmp/lunar-live-cmd")

        def _read_state() -> dict[str, Any]:
            if state_path.exists():
                try:
                    return json.loads(state_path.read_text(encoding="utf-8"))
                except Exception:
                    pass
            return {"status": "not_running"}

        if action == "status":
            return {"ok": True, "state": _read_state()}

        cmd_map = {
            "start": "start_live",
            "stop": "stop_live",
            "toggle": "toggle_live",
            "toggle_vision": "toggle_vision",
        }
        if action in cmd_map:
            with cmd_path.open("a", encoding="utf-8") as fh:
                fh.write(cmd_map[action] + "\n")
            return {"ok": True, "sent": cmd_map[action], "state": _read_state()}

        if action == "send_prompt":
            prompt_text = str(args.get("prompt", "")).strip()
            if not prompt_text:
                return {"ok": False, "error": "Missing 'prompt' for action='send_prompt'"}
            single_line = " ".join(prompt_text.splitlines()).strip()
            with cmd_path.open("a", encoding="utf-8") as fh:
                fh.write(f"prompt:{single_line}\n")
            return {"ok": True, "sent": "prompt", "prompt": single_line, "state": _read_state()}

        return {"ok": False, "error": f"Unknown gemini_live action: {action}"}

    return {"ok": False, "error": f"Unknown tool: {name}"}


def send_response(msg: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def main() -> int:
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue

        req_id = req.get("id")
        method = req.get("method", "")
        params = req.get("params") or {}

        if method == "initialize":
            send_response(
                {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {
                        "protocolVersion": PROTOCOL_VERSION,
                        "capabilities": {"tools": {}},
                        "serverInfo": SERVER_INFO,
                    },
                }
            )
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            send_response({"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            tname = params.get("name", "")
            targs = params.get("arguments") or {}
            res = handle_tool(tname, targs)
            send_response(
                {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {
                        "content": [{"type": "text", "text": json.dumps(res, indent=2)}],
                        "isError": not res.get("ok", True),
                    },
                }
            )
        elif req_id is not None:
            send_response(
                {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "error": {"code": -32601, "message": f"Method not found: {method}"},
                }
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
