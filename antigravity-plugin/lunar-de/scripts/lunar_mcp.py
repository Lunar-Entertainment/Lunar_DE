#!/usr/bin/env python3
"""Lunar DE MCP Server (JSON-RPC 2.0 over stdio) for Antigravity."""

from __future__ import annotations

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
SERVER_INFO = {"name": "lunar-de", "version": "1.0.0"}

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
            Path.home() / ".local/share/flatpak/exports/share/applications",
            Path("/var/lib/flatpak/exports/share/applications"),
            Path("/usr/share/applications"),
        ]
        for d in app_dirs:
            if not d.exists():
                continue
            for p in sorted(d.glob("*.desktop")):
                txt = p.read_text(encoding="utf-8", errors="ignore")
                app_name = ""
                exec_cmd = ""
                for line in txt.splitlines():
                    if line.startswith("Name=") and not app_name:
                        app_name = line.split("=", 1)[1].strip()
                    elif line.startswith("Exec=") and not exec_cmd:
                        exec_cmd = line.split("=", 1)[1].strip()
                if query in app_name.lower() and exec_cmd:
                    for tok in ("--file-forwarding", "@@u", "@@", "%U", "%u", "%F", "%f"):
                        exec_cmd = exec_cmd.replace(tok, "")
                    env = os.environ.copy()
                    env["DISPLAY"] = disp
                    env.pop("WAYLAND_DISPLAY", None)
                    env["GDK_BACKEND"] = "x11"
                    env["XDG_CURRENT_DESKTOP"] = "zorin:GNOME"
                    subprocess.Popen(
                        ["/bin/sh", "-c", exec_cmd.strip()],
                        env=env,
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
                    all_wps.append(p)
        if query.lower() == "list":
            return {"count": len(all_wps), "wallpapers": [p.name for p in all_wps[:50]]}
        if query.upper() == "CYCLE_ALL":
            (cfg_dir() / "wallpaper").write_text("CYCLE_ALL\n", encoding="utf-8")
            return {"ok": True, "wallpaper": "CYCLE_ALL"}
        matched = Path(query) if Path(query).exists() else None
        if not matched:
            q_low = query.lower().replace("_", " ")
            for p in all_wps:
                if q_low in p.stem.lower().replace("_", " "):
                    matched = p
                    break
        if matched:
            (cfg_dir() / "wallpaper").write_text(str(matched) + "\n", encoding="utf-8")
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
