# Lunar DE

A minimal, modular **C++20 X11 Desktop Environment** built from the ground up on top of `libxcb`.

🌐 **Website:** https://lunar-entertainment.github.io/Lunar_DE/

## Features
- **`lunar-wm` (C++20 Window Manager)**: Multi-monitor RandR support, Aero Snap (drag-to-top maximize & left/right half snap with live preview), full `_NET_WM_MOVERESIZE` support for Electron/CSD apps, smooth wallpaper cross-fade & startup animation, persistent desktop icons, and GPU-accelerated `picom` compositing.
- **`lunar-panel` (C++20 Shell & Bar)**: Switchable **Top Bar** (28px) and **Windows 11 Taskbar** (44px centered with pinned apps), App Menu with PNG/SVG icon rendering, Quick Settings, **32 free wallpapers + Cycle All (15s)**, and **Gemini Live** voice assistant (`Super + G`).
- **Antigravity (`agy`) Plugin**: Included under `antigravity-plugin/lunar-de/` with a 9-tool stdio MCP server for desktop control.

## Quick Start
```bash
# Test live in Xephyr (single monitor or virtual multi-monitor)
./run_dev.sh
./run_dev.sh -m 2

# Install as a real system X11 session
./install.sh
```
