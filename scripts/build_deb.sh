#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DIST_DIR="${ROOT_DIR}/dist"
VERSION="${1:-${LUNAR_VERSION:-1.0.0}}"
ARCH="amd64"
PKG_NAME="lunar-de"
OUT_DEB="${DIST_DIR}/${PKG_NAME}_${VERSION}_${ARCH}.deb"

echo "[lunar] Building Release binaries for ${PKG_NAME} (${VERSION})..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

mkdir -p \
    "${STAGE_DIR}/DEBIAN" \
    "${STAGE_DIR}/usr/bin" \
    "${STAGE_DIR}/usr/share/applications" \
    "${STAGE_DIR}/usr/share/xsessions" \
    "${STAGE_DIR}/usr/share/xdg-desktop-portal" \
    "${STAGE_DIR}/usr/share/polkit-1/rules.d" \
    "${STAGE_DIR}/usr/share/lunar-de/config" \
    "${STAGE_DIR}/usr/share/lunar-de/scripts" \
    "${STAGE_DIR}/usr/share/lunar-de/assets/wallpapers" \
    "${STAGE_DIR}/usr/share/lunar-de/antigravity-plugin" \
    "${STAGE_DIR}/usr/share/icons/hicolor/scalable/apps" \
    "${STAGE_DIR}/etc/systemd/system"

install -m 0755 "${BUILD_DIR}/lunar-wm" "${STAGE_DIR}/usr/bin/lunar-wm"
install -m 0755 "${BUILD_DIR}/lunar-panel" "${STAGE_DIR}/usr/bin/lunar-panel"
install -m 0755 "${ROOT_DIR}/scripts/lunar-session" "${STAGE_DIR}/usr/bin/lunar-session"
install -m 0755 "${ROOT_DIR}/scripts/lunar-debug" "${STAGE_DIR}/usr/bin/lunar-debug"
install -m 0755 "${ROOT_DIR}/scripts/lunar-restart-update" "${STAGE_DIR}/usr/bin/lunar-restart-update"
install -m 0755 "${ROOT_DIR}/scripts/lunar-displays" "${STAGE_DIR}/usr/bin/lunar-displays"

install -m 0644 "${ROOT_DIR}/config/lunar-displays.desktop" "${STAGE_DIR}/usr/share/applications/lunar-displays.desktop"

cat > "${STAGE_DIR}/usr/share/xsessions/lunar.desktop" <<'EOF'
[Desktop Entry]
Name=Lunar DE
Comment=Minimal, modular C++20 X11 Desktop Environment
Exec=/usr/bin/lunar-session
Type=Application
DesktopNames=Lunar
Icon=lunar-de
EOF
chmod 0644 "${STAGE_DIR}/usr/share/xsessions/lunar.desktop"

install -m 0644 "${ROOT_DIR}/config/picom.conf" "${STAGE_DIR}/usr/share/lunar-de/config/picom.conf"
install -m 0644 "${ROOT_DIR}/config/lunar-portals.conf" "${STAGE_DIR}/usr/share/xdg-desktop-portal/lunar-portals.conf"
install -m 0644 "${ROOT_DIR}/config/49-lunar-de-autoupdate.rules" "${STAGE_DIR}/usr/share/polkit-1/rules.d/49-lunar-de-autoupdate.rules"
install -m 0755 "${ROOT_DIR}/scripts/lunar_live.py" "${STAGE_DIR}/usr/share/lunar-de/scripts/lunar_live.py"
install -m 0755 "${ROOT_DIR}/scripts/render_icons.py" "${STAGE_DIR}/usr/share/lunar-de/scripts/render_icons.py"

cp -f "${ROOT_DIR}/assets/wallpapers/"* "${STAGE_DIR}/usr/share/lunar-de/assets/wallpapers/"
chmod 0644 "${STAGE_DIR}/usr/share/lunar-de/assets/wallpapers/"*

if [[ -d "${ROOT_DIR}/antigravity-plugin/lunar-de" ]]; then
    cp -a "${ROOT_DIR}/antigravity-plugin/lunar-de" "${STAGE_DIR}/usr/share/lunar-de/antigravity-plugin/"
    chmod 0755 "${STAGE_DIR}/usr/share/lunar-de/antigravity-plugin/lunar-de/scripts/lunar_mcp.py"
    cat > "${STAGE_DIR}/usr/share/lunar-de/antigravity-plugin/lunar-de/mcp_config.json" <<'EOF'
{
  "mcpServers": {
    "lunar-de": {
      "command": "python3",
      "args": [
        "/usr/share/lunar-de/antigravity-plugin/lunar-de/scripts/lunar_mcp.py"
      ]
    }
  }
}
EOF
    if [[ -f "${ROOT_DIR}/antigravity-plugin/lunar-de/assets/logo.svg" ]]; then
        install -m 0644 "${ROOT_DIR}/antigravity-plugin/lunar-de/assets/logo.svg" \
            "${STAGE_DIR}/usr/share/icons/hicolor/scalable/apps/lunar-de.svg"
    fi
fi

cat > "${STAGE_DIR}/etc/systemd/system/lunar-de-autoupdate.service" <<'EOF'
[Unit]
Description=Auto-update Lunar DE from GitHub Pages APT repository
After=network-online.target
Wants=network-online.target
Before=display-manager.service

[Service]
Type=oneshot
TimeoutStartSec=25
Environment=DEBIAN_FRONTEND=noninteractive
ExecStart=/bin/sh -c 'if [ -f /etc/apt/sources.list.d/lunar-de.list ]; then apt-get update -o Dir::Etc::sourcelist="sources.list.d/lunar-de.list" -o Dir::Etc::sourceparts="-" -o APT::Get::List-Cleanup="0" && apt-get install -y --only-upgrade lunar-de; fi'

[Install]
WantedBy=multi-user.target
EOF
chmod 0644 "${STAGE_DIR}/etc/systemd/system/lunar-de-autoupdate.service"

INSTALLED_KB="$(du -sk "${STAGE_DIR}" | awk '{print $1}')"

cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: x11
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_KB}
Maintainer: Lunar Entertainment <lunar@users.noreply.github.com>
Homepage: https://lunar-entertainment.github.io/Lunar_DE/
Depends: libc6, libstdc++6, libxcb1, libxcb-util1, libxcb-ewmh2, libxcb-icccm4, libxcb-keysyms1, libxcb-randr0, libxkbcommon0, libjpeg-turbo8 | libjpeg8, libpng16-16t64 | libpng16-16, libfontconfig1, libfreetype6, picom, python3, python3-gi, python3-gi-cairo, gir1.2-gtk-3.0, x11-xserver-utils, xdotool, pulseaudio-utils
Recommends: python3-websockets, ffmpeg, wmctrl, xclip
Description: Minimal modular C++20 X11 Desktop Environment
 Lunar DE is built from scratch in C++20 directly on libxcb, featuring
 multi-monitor RandR support, Aero Snap, Top Bar & Windows 11 Taskbar
 layouts, 32 bundled wallpapers with smooth cross-fades, Gemini Live voice
 integration, and boot auto-updates.
EOF
chmod 0644 "${STAGE_DIR}/DEBIAN/control"

cat > "${STAGE_DIR}/DEBIAN/postinst" <<'EOF'
#!/usr/bin/env bash
set -e
if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    systemctl daemon-reload || true
    systemctl enable lunar-de-autoupdate.service || true
fi
exit 0
EOF
chmod 0755 "${STAGE_DIR}/DEBIAN/postinst"

cat > "${STAGE_DIR}/DEBIAN/prerm" <<'EOF'
#!/usr/bin/env bash
set -e
if [[ "${1:-}" == "remove" ]] && command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    systemctl disable lunar-de-autoupdate.service || true
fi
exit 0
EOF
chmod 0755 "${STAGE_DIR}/DEBIAN/prerm"

mkdir -p "${DIST_DIR}"
dpkg-deb --root-owner-group --build "${STAGE_DIR}" "${OUT_DEB}"
echo "[lunar] Built package: ${OUT_DEB}"
