---
name: lunar-desktop
description: Control the user's Linux PC and Lunar DE desktop environment via the lunar-de MCP server (launch apps, inspect and manage windows, take screenshots, set volume, switch wallpapers, change bar layout, and read system/monitor status).
---

# Lunar DE PC Control

Use the `lunar-de` MCP server tools whenever the user asks to control or inspect their Linux desktop or Lunar DE session.

## Available MCP Tools (`lunar-de`)

- `get_system_info`: Inspect connected monitors (`xrandr`), CPU/RAM/disk usage, audio volume, and active `DISPLAY`.
- `list_windows`: List visible X11 windows with IDs and titles.
- `launch_app`: Launch any installed desktop application by name (e.g., `brave`, `files`, `terminal`, `discord`, `antigravity`).
- `window_action`: Focus, close, maximize, or snap a window by title substring or X11 window ID.
- `take_screenshot`: Capture the active X11 display to a PNG file (then inspect it with `read_file` if visual verification is needed).
- `set_volume`: Set system volume (`0`–`100`) or toggle mute.
- `set_wallpaper`: List wallpapers, switch to a wallpaper by keyword, or enable `CYCLE_ALL` auto-rotation.
- `set_bar_layout`: Switch Lunar DE between `top` (Top Bar), `taskbar` (Bottom Taskbar), and `taskbar-top` (Top Taskbar).
- `send_input`: Send key presses or mouse clicks via `xdotool` when explicitly requested by the user.

## Guidelines

- Always confirm destructive or disruptive actions (like closing unsaved windows) if ambiguous.
- Prefer `display=":0"` on the user's main desktop session, or pass `display=":2"` when targeting the Xephyr test session.
