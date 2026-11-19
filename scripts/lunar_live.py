#!/usr/bin/env python3
"""Lunar DE Gemini Live Voice Assistant Daemon."""

from __future__ import annotations

import array
import asyncio
import base64
import io
import json
import math
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path
from typing import Any

import websockets

STATE_PATH = Path("/tmp/lunar-live-state.json")
CMD_PATH = Path("/tmp/lunar-live-cmd")
SAMPLE_RATE_IN = 16000
SAMPLE_RATE_OUT = 24000
CHUNK_SAMPLES = 1600  # 100ms at 16kHz
CHUNK_BYTES = CHUNK_SAMPLES * 2

LIVE_MODELS = [
    "models/gemini-2.5-flash-native-audio-preview-12-2025",
    "models/gemini-2.0-flash-live-001",
    "models/gemini-2.0-flash-exp",
]

SYSTEM_PROMPT = (
    "You are Lunar, the built-in voice assistant for Lunar DE (a sleek Linux desktop). "
    "Keep responses concise, natural, and helpful. You can also control the desktop "
    "using your tools (open apps, open terminal, adjust volume, switch workspace, or "
    "change wallpapers) whenever the user asks."
)

TOOLS_SPEC = [
    {
        "functionDeclarations": [
            {
                "name": "open_app",
                "description": "Launch a desktop application by name (e.g., brave, files, calculator, settings, bambustudio).",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "name": {"type": "STRING", "description": "Application name to search and launch."}
                    },
                    "required": ["name"],
                },
            },
            {
                "name": "open_terminal",
                "description": "Open a new terminal window.",
                "parameters": {"type": "OBJECT", "properties": {}},
            },
            {
                "name": "set_volume",
                "description": "Set system audio volume percentage (0 to 100).",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "percent": {"type": "INTEGER", "description": "Volume percentage 0-100."}
                    },
                    "required": ["percent"],
                },
            },
            {
                "name": "set_wallpaper",
                "description": "Change desktop wallpaper by name (e.g., lunar eclipse, nordic aurora, cyber synthwave, ocean horizon, dolomites).",
                "parameters": {
                    "type": "OBJECT",
                    "properties": {
                        "name": {"type": "STRING", "description": "Wallpaper name or keyword."}
                    },
                    "required": ["name"],
                },
            },
        ]
    }
]


def get_api_key_path() -> Path:
    cfg_home = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(cfg_home) / "lunar-de" / "gemini_api_key"


def load_api_key() -> str:
    for env_name in ("GEMINI_API_KEY", "GOOGLE_API_KEY"):
        val = os.environ.get(env_name, "").strip()
        if val:
            return val
    key_file = get_api_key_path()
    if key_file.exists():
        return key_file.read_text(encoding="utf-8").strip()
    return ""


def write_state(
    status: str,
    wake_enabled: bool,
    user_text: str = "",
    model_text: str = "",
    error: str = "",
) -> None:
    payload = {
        "status": status,
        "wake_enabled": wake_enabled,
        "user_text": user_text[-180:],
        "model_text": model_text[-220:],
        "error": error[-160:],
        "ts": time.time(),
    }
    tmp = STATE_PATH.with_name(f".{STATE_PATH.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(payload), encoding="utf-8")
    tmp.replace(STATE_PATH)


def pcm_rms(pcm_bytes: bytes) -> float:
    if len(pcm_bytes) < 2:
        return 0.0
    samples = array.array("h")
    samples.frombytes(pcm_bytes[: len(pcm_bytes) - (len(pcm_bytes) % 2)])
    if not samples:
        return 0.0
    sq = sum(s * s for s in samples)
    return math.sqrt(sq / len(samples))


def pcm_to_wav_bytes(pcm_bytes: bytes, sample_rate: int = SAMPLE_RATE_IN) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def play_chime(rising: bool = True) -> None:
    """Play a brief 2-tone chime via paplay without external audio files."""
    freqs = (523.25, 783.99) if rising else (659.25, 440.0)
    samples = array.array("h")
    sr = SAMPLE_RATE_OUT
    for freq in freqs:
        n = int(sr * 0.09)
        for i in range(n):
            env = math.sin(math.pi * i / n)
            val = int(4200 * env * math.sin(2.0 * math.pi * freq * i / sr))
            samples.append(val)
    try:
        subprocess.run(
            ["paplay", "--raw", "--format=s16le", f"--rate={sr}", "--channels=1"],
            input=samples.tobytes(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=2,
            check=False,
        )
    except Exception:
        pass


def spawn_detached(cmd: str) -> None:
    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env["GDK_BACKEND"] = "x11"
    env["QT_QPA_PLATFORM"] = "xcb"
    env["GTK_USE_PORTAL"] = "0"
    subprocess.Popen(
        ["/bin/sh", "-c", cmd],
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def execute_tool_call(name: str, args: dict[str, Any]) -> dict[str, Any]:
    if name == "open_terminal":
        spawn_detached("x-terminal-emulator || gnome-terminal")
        return {"ok": True, "message": "Terminal launched."}

    if name == "set_volume":
        pct = max(0, min(100, int(args.get("percent", 50))))
        subprocess.run(
            ["pactl", "set-sink-volume", "@DEFAULT_SINK@", f"{pct}%"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return {"ok": True, "volume": pct}

    if name == "set_wallpaper":
        query = str(args.get("name", "")).lower().strip()
        repo_wp = Path(__file__).resolve().parent.parent / "assets" / "wallpapers"
        search_dirs = [repo_wp, Path("/usr/share/backgrounds"), Path.home() / "Pictures"]
        matched: Path | None = None
        for d in search_dirs:
            if not d.exists():
                continue
            for p in sorted(d.iterdir()):
                if p.suffix.lower() in (".jpg", ".jpeg", ".png") and query in p.stem.lower().replace("_", " "):
                    matched = p
                    break
            if matched:
                break
        if matched:
            cfg_dir = get_api_key_path().parent
            cfg_dir.mkdir(parents=True, exist_ok=True)
            (cfg_dir / "wallpaper").write_text(str(matched) + "\n", encoding="utf-8")
            return {"ok": True, "wallpaper": matched.name, "note": "Saved wallpaper; press Reload or reopen picker if needed."}
        return {"ok": False, "error": f"No wallpaper matching '{query}' found."}

    if name == "open_app":
        query = str(args.get("name", "")).lower().strip()
        app_dirs = [
            Path.home() / ".local/share/applications",
            Path("/var/lib/flatpak/exports/share/applications"),
            Path("/usr/share/applications"),
        ]
        for d in app_dirs:
            if not d.exists():
                continue
            for p in d.glob("*.desktop"):
                txt = p.read_text(encoding="utf-8", errors="ignore")
                app_name = ""
                exec_cmd = ""
                for line in txt.splitlines():
                    if line.startswith("Name=") and not app_name:
                        app_name = line.split("=", 1)[1].strip()
                    elif line.startswith("Exec=") and not exec_cmd:
                        exec_cmd = line.split("=", 1)[1].strip()
                if query and query in app_name.lower() and exec_cmd:
                    for tok in ("--file-forwarding", "@@u", "@@", "%U", "%u", "%F", "%f"):
                        exec_cmd = exec_cmd.replace(tok, "")
                    if "flatpak run" in exec_cmd:
                        disp = os.environ.get("DISPLAY", ":2")
                        exec_cmd = exec_cmd.replace(
                            "flatpak run",
                            f"flatpak run --env=DISPLAY={disp} --env=GDK_BACKEND=x11",
                            1,
                        )
                    spawn_detached(exec_cmd.strip())
                    return {"ok": True, "launched": app_name}
        return {"ok": False, "error": f"App '{query}' not found."}

    return {"ok": False, "error": f"Unknown tool '{name}'"}


class LunarLiveDaemon:
    def __init__(self) -> None:
        self.running = True
        self.wake_enabled = False
        self.live_requested = False
        self.stop_live_flag = False
        self.user_text = ""
        self.model_text = ""
        self.status = "idle"
        self.last_error = ""

    def sync_state(self) -> None:
        write_state(
            status=self.status,
            wake_enabled=self.wake_enabled,
            user_text=self.user_text,
            model_text=self.model_text,
            error=self.last_error,
        )

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
            if cmd == "toggle_live":
                if self.status in ("connecting", "live", "speaking"):
                    self.stop_live_flag = True
                else:
                    self.live_requested = True
            elif cmd == "start_live":
                self.live_requested = True
            elif cmd == "stop_live":
                self.stop_live_flag = True
            elif cmd == "toggle_wake":
                self.wake_enabled = not self.wake_enabled
            elif cmd == "quit":
                self.running = False
                self.stop_live_flag = True

    async def run_live_session(self, api_key: str, initial_command: str = "") -> None:
        self.stop_live_flag = False
        self.status = "connecting"
        self.last_error = ""
        self.sync_state()

        ws = None
        last_exc = ""
        for model in LIVE_MODELS:
            uri = (
                "wss://generativelanguage.googleapis.com/ws/"
                f"google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={api_key}"
            )
            try:
                ws = await websockets.connect(uri, max_size=8 * 1024 * 1024)
                setup_msg = {
                    "setup": {
                        "model": model,
                        "generationConfig": {
                            "responseModalities": ["AUDIO"],
                            "speechConfig": {
                                "voiceConfig": {"prebuiltVoiceConfig": {"voiceName": "Aoede"}}
                            },
                        },
                        "systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]},
                        "tools": TOOLS_SPEC,
                        "inputAudioTranscription": {},
                        "outputAudioTranscription": {},
                    }
                }
                await ws.send(json.dumps(setup_msg))
                first = json.loads(await asyncio.wait_for(ws.recv(), timeout=8.0))
                if "setupComplete" in first:
                    break
                await ws.close()
                ws = None
            except Exception as exc:
                last_exc = str(exc)
                if ws:
                    await ws.close()
                    ws = None

        if not ws:
            self.status = "error"
            self.last_error = last_exc or "Failed to connect to Gemini Live"
            self.sync_state()
            await asyncio.sleep(2.5)
            return

        self.status = "live"
        self.sync_state()
        play_chime(rising=True)

        if initial_command:
            self.user_text = initial_command
            self.sync_state()
            await ws.send(
                json.dumps(
                    {
                        "clientContent": {
                            "turns": [{"role": "user", "parts": [{"text": initial_command}]}],
                            "turnComplete": True,
                        }
                    }
                )
            )

        rec_proc = await asyncio.create_subprocess_exec(
            "parec",
            "--format=s16le",
            f"--rate={SAMPLE_RATE_IN}",
            "--channels=1",
            "--latency-msec=60",
            stdout=asyncio.subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        play_proc = await asyncio.create_subprocess_exec(
            "paplay",
            "--raw",
            "--format=s16le",
            f"--rate={SAMPLE_RATE_OUT}",
            "--channels=1",
            "--latency-msec=80",
            stdin=asyncio.subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

        last_activity = time.monotonic()

        async def send_audio() -> None:
            nonlocal last_activity
            assert rec_proc.stdout is not None
            while not self.stop_live_flag and self.running:
                self.poll_commands()
                if self.stop_live_flag or not self.running:
                    break
                try:
                    chunk = await asyncio.wait_for(rec_proc.stdout.readexactly(CHUNK_BYTES), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                except asyncio.IncompleteReadError:
                    break

                if pcm_rms(chunk) > 450:
                    last_activity = time.monotonic()
                elif time.monotonic() - last_activity > 22.0:
                    self.stop_live_flag = True
                    break

                b64 = base64.b64encode(chunk).decode("ascii")
                await ws.send(
                    json.dumps(
                        {
                            "realtimeInput": {
                                "mediaChunks": [
                                    {"mimeType": f"audio/pcm;rate={SAMPLE_RATE_IN}", "data": b64}
                                ]
                            }
                        }
                    )
                )

        async def recv_server() -> None:
            nonlocal last_activity
            assert play_proc.stdin is not None
            async for raw in ws:
                if self.stop_live_flag or not self.running:
                    break
                msg = json.loads(raw)

                sc = msg.get("serverContent")
                if sc:
                    if sc.get("interrupted"):
                        self.status = "live"
                        self.sync_state()

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
                            self.status = "speaking"
                            last_activity = time.monotonic()
                            self.sync_state()
                            pcm_out = base64.b64decode(inline["data"])
                            play_proc.stdin.write(pcm_out)
                            await play_proc.stdin.drain()

                    if sc.get("turnComplete"):
                        self.status = "live"
                        last_activity = time.monotonic()
                        self.sync_state()

                tc = msg.get("toolCall")
                if tc:
                    responses = []
                    for fc in tc.get("functionCalls", []):
                        res = execute_tool_call(fc.get("name", ""), fc.get("args", {}))
                        responses.append(
                            {
                                "id": fc.get("id", ""),
                                "name": fc.get("name", ""),
                                "response": res,
                            }
                        )
                    await ws.send(json.dumps({"toolResponse": {"functionResponses": responses}}))

        async def watch_stop() -> None:
            while not self.stop_live_flag and self.running:
                self.poll_commands()
                if self.stop_live_flag or not self.running:
                    break
                await asyncio.sleep(0.1)

        send_task = asyncio.create_task(send_audio())
        recv_task = asyncio.create_task(recv_server())
        watch_task = asyncio.create_task(watch_stop())
        try:
            done, pending = await asyncio.wait(
                {send_task, recv_task, watch_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            for t in done:
                exc = t.exception()
                if exc and not isinstance(exc, asyncio.CancelledError):
                    self.last_error = str(exc)
        except Exception as exc:
            self.last_error = str(exc)
        finally:
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
            self.status = "idle"
            self.sync_state()
            play_chime(rising=False)

    async def main_loop(self) -> None:
        while self.running:
            self.poll_commands()
            api_key = load_api_key()
            if not api_key:
                self.status = "needs_key"
                self.sync_state()
                await asyncio.sleep(0.25)
                continue

            if self.live_requested:
                self.live_requested = False
                self.user_text = ""
                self.model_text = ""
                await self.run_live_session(api_key)
                continue

            self.status = "idle"
            self.sync_state()
            await asyncio.sleep(0.15)


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
