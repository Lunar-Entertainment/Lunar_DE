#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

echo "[lunar] Building Release binaries..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[lunar] Installing binaries and session files (requires sudo)..."
sudo install -Dm755 "${BUILD_DIR}/lunar-wm" /usr/local/bin/lunar-wm
sudo install -Dm755 "${BUILD_DIR}/lunar-panel" /usr/local/bin/lunar-panel
sudo install -Dm755 "${ROOT_DIR}/scripts/lunar-session" /usr/local/bin/lunar-session
sudo install -Dm755 "${ROOT_DIR}/scripts/lunar-debug" /usr/local/bin/lunar-debug
sudo install -Dm755 "${ROOT_DIR}/scripts/lunar-restart-update" /usr/local/bin/lunar-restart-update

sudo install -Dm644 "${ROOT_DIR}/config/picom.conf" /usr/local/share/lunar-de/config/picom.conf
sudo install -Dm644 "${ROOT_DIR}/config/lunar-portals.conf" /usr/share/xdg-desktop-portal/lunar-portals.conf
sudo install -Dm644 "${ROOT_DIR}/config/49-lunar-de-autoupdate.rules" /usr/share/polkit-1/rules.d/49-lunar-de-autoupdate.rules
sudo install -Dm755 "${ROOT_DIR}/scripts/lunar_live.py" /usr/local/share/lunar-de/scripts/lunar_live.py
sudo install -Dm755 "${ROOT_DIR}/scripts/render_icons.py" /usr/local/share/lunar-de/scripts/render_icons.py

sudo mkdir -p /usr/local/share/lunar-de/assets/wallpapers
sudo cp -f "${ROOT_DIR}/assets/wallpapers/"* /usr/local/share/lunar-de/assets/wallpapers/

sudo install -Dm644 "${ROOT_DIR}/scripts/lunar.desktop" /usr/share/xsessions/lunar.desktop

echo "[lunar] Installed! Log out and select 'Lunar DE' on the login screen."
