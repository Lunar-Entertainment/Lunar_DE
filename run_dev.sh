#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
TEST_DISPLAY=":2"
PID_FILE="/tmp/lunar-de-x2.pids"
WM_LOG="/tmp/lunar-wm.log"
PANEL_LOG="/tmp/lunar-panel.log"
TERM_LOG="/tmp/lunar-term.log"
LUNAR_RT="/tmp/lunar-rt-$(id -u)-x2"

XEPHYR_FULLSCREEN=0
VIRTUAL_MONITORS=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fullscreen)
            XEPHYR_FULLSCREEN=1
            shift
            ;;
        -m)
            VIRTUAL_MONITORS="${2:-1}"
            if ! [[ "${VIRTUAL_MONITORS}" =~ ^[0-9]+$ ]] || (( VIRTUAL_MONITORS < 1 )); then
                echo "[lunar] Error: -m requires a positive integer (e.g. -m 2)" >&2
                exit 1
            fi
            shift 2
            ;;
        *)
            echo "Usage: $0 [--fullscreen] [-m <monitors>]" >&2
            exit 1
            ;;
    esac
done

echo "[lunar] Building project into ${BUILD_DIR}..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[lunar] Cleaning up any previous test instances on ${TEST_DISPLAY}..."
if [[ -f "${PID_FILE}" ]]; then
    while IFS= read -r pid; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
    done < "${PID_FILE}"
    rm -f "${PID_FILE}"
fi

pkill -f "${BUILD_DIR}/lunar-wm" 2>/dev/null || true
pkill -f "${BUILD_DIR}/lunar-panel" 2>/dev/null || true
pkill -f "${ROOT_DIR}/config/picom.conf" 2>/dev/null || true
pkill -f "dbus-daemon.*${LUNAR_RT}/bus" 2>/dev/null || true
sleep 0.2

if [[ -d "${LUNAR_RT}/doc" ]]; then
    fusermount3 -u "${LUNAR_RT}/doc" 2>/dev/null || true
fi
rm -rf "${LUNAR_RT}"
mkdir -p "${LUNAR_RT}"
chmod 700 "${LUNAR_RT}"

if [[ "${XEPHYR_FULLSCREEN}" == "1" ]] || (( VIRTUAL_MONITORS > 1 )) || ! pgrep -af "Xephyr.*-resizeable.*${TEST_DISPLAY}" >/dev/null 2>&1; then
    if xdpyinfo -display "${TEST_DISPLAY}" >/dev/null 2>&1; then
        pkill -f "Xephyr.*${TEST_DISPLAY}" 2>/dev/null || true
        sleep 0.3
    fi
fi

if ! xdpyinfo -display "${TEST_DISPLAY}" >/dev/null 2>&1; then
    XEPHYR_ARGS=(-br -ac -noreset -resizeable)
    if [[ "${XEPHYR_FULLSCREEN}" == "1" ]]; then
        echo "[lunar] Starting Xephyr on ${TEST_DISPLAY} (fullscreen)..."
        XEPHYR_ARGS+=(-fullscreen)
    elif (( VIRTUAL_MONITORS > 1 )); then
        MON_W=960
        MON_H=640
        TOT_W=$((VIRTUAL_MONITORS * MON_W))
        echo "[lunar] Starting Xephyr on ${TEST_DISPLAY} (${TOT_W}x${MON_H}, ${VIRTUAL_MONITORS} virtual monitors)..."
        XEPHYR_ARGS+=(-screen "${TOT_W}x${MON_H}")
    else
        echo "[lunar] Starting Xephyr on ${TEST_DISPLAY} (1280x720, resizeable)..."
        XEPHYR_ARGS+=(-screen 1280x720)
    fi
    setsid Xephyr "${XEPHYR_ARGS[@]}" "${TEST_DISPLAY}" </dev/null >/tmp/lunar-xephyr.log 2>&1 &
    XEPHYR_PID=$!
    echo "${XEPHYR_PID}" > /tmp/lunar-xephyr.pid

    for _ in $(seq 1 50); do
        if xdpyinfo -display "${TEST_DISPLAY}" >/dev/null 2>&1; then
            break
        fi
        if ! kill -0 "${XEPHYR_PID}" 2>/dev/null; then
            echo "[lunar] Error: Xephyr exited unexpectedly. See /tmp/lunar-xephyr.log" >&2
            exit 1
        fi
        sleep 0.1
    done

    if ! xdpyinfo -display "${TEST_DISPLAY}" >/dev/null 2>&1; then
        echo "[lunar] Error: Xephyr on ${TEST_DISPLAY} did not become ready in time." >&2
        exit 1
    fi
else
    echo "[lunar] Reusing existing Xephyr instance on ${TEST_DISPLAY}."
    pgrep -f "Xephyr.*${TEST_DISPLAY}" | head -n1 > /tmp/lunar-xephyr.pid || true
fi

if (( VIRTUAL_MONITORS > 1 )) && command -v xrandr >/dev/null 2>&1; then
    MON_W=960
    MON_H=640
    OUT_NAME=$(xrandr --display "${TEST_DISPLAY}" --query | awk '/ connected/{print $1; exit}')
    for ((i = 0; i < VIRTUAL_MONITORS; ++i)); do
        xrandr --display "${TEST_DISPLAY}" --delmonitor "VMon-${i}" 2>/dev/null || true
        X_OFF=$((i * MON_W))
        xrandr --display "${TEST_DISPLAY}" --setmonitor "VMon-${i}" \
            "${MON_W}/254x${MON_H}/170+${X_OFF}+0" "${OUT_NAME:-none}" || true
    done
    echo "[lunar] Mounted ${VIRTUAL_MONITORS} virtual RandR monitors on ${TEST_DISPLAY}."
fi

HOST_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME}/.cache}"
export XDG_RUNTIME_DIR="${LUNAR_RT}"
export PULSE_SERVER="${PULSE_SERVER:-unix:${HOST_RUNTIME_DIR}/pulse/native}"
export DISPLAY="${TEST_DISPLAY}"
export LUNAR_XEPHYR=1
unset WAYLAND_DISPLAY GNOME_KEYRING_CONTROL
export GDK_BACKEND=x11
export QT_QPA_PLATFORM=xcb
export SDL_VIDEODRIVER=x11
export CLUTTER_BACKEND=x11
export ELECTRON_OZONE_PLATFORM_HINT=x11
export GTK_USE_PORTAL=0
export GIO_USE_VFS=local
export NO_AT_BRIDGE=1
export GTK_A11Y=none
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=Lunar

: > "${PID_FILE}"

if command -v dbus-daemon >/dev/null 2>&1; then
    DBUS_PID=$(dbus-daemon --session --fork --address="unix:path=${LUNAR_RT}/bus" --print-pid)
    export DBUS_SESSION_BUS_ADDRESS="unix:path=${LUNAR_RT}/bus"
    echo "${DBUS_PID}" >> "${PID_FILE}"
    if command -v dbus-update-activation-environment >/dev/null 2>&1; then
        dbus-update-activation-environment \
            DISPLAY LUNAR_XEPHYR XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME \
            PULSE_SERVER GDK_BACKEND QT_QPA_PLATFORM SDL_VIDEODRIVER \
            CLUTTER_BACKEND ELECTRON_OZONE_PLATFORM_HINT GTK_USE_PORTAL \
            GIO_USE_VFS NO_AT_BRIDGE GTK_A11Y XDG_SESSION_TYPE XDG_CURRENT_DESKTOP \
            2>/dev/null || true
    fi
fi

setsid "${BUILD_DIR}/lunar-wm" </dev/null >"${WM_LOG}" 2>&1 &
echo "$!" >> "${PID_FILE}"
sleep 0.3

if command -v picom >/dev/null 2>&1 && [[ -f "${ROOT_DIR}/config/picom.conf" ]]; then
    setsid picom --config "${ROOT_DIR}/config/picom.conf" </dev/null >/tmp/lunar-picom.log 2>&1 &
    echo "$!" >> "${PID_FILE}"
    sleep 0.2
fi

setsid "${BUILD_DIR}/lunar-panel" </dev/null >"${PANEL_LOG}" 2>&1 &
echo "$!" >> "${PID_FILE}"
sleep 0.2

pkill -f "scripts/lunar_live.py" 2>/dev/null || true
if command -v python3 >/dev/null 2>&1 && [[ -f "${ROOT_DIR}/scripts/lunar_live.py" ]]; then
    setsid python3 "${ROOT_DIR}/scripts/lunar_live.py" </dev/null >/tmp/lunar-live.log 2>&1 &
    echo "$!" >> "${PID_FILE}"
fi

if command -v x-terminal-emulator >/dev/null 2>&1; then
    setsid x-terminal-emulator </dev/null >"${TERM_LOG}" 2>&1 &
    echo "$!" >> "${PID_FILE}"
elif command -v gnome-terminal >/dev/null 2>&1; then
    setsid gnome-terminal </dev/null >"${TERM_LOG}" 2>&1 &
    echo "$!" >> "${PID_FILE}"
fi

sleep 1.2

if ! pgrep -f "${BUILD_DIR}/lunar-wm" >/dev/null 2>&1; then
    echo "[lunar] Error: lunar-wm is not running. Log:" >&2
    cat "${WM_LOG}" >&2 || true
    exit 1
fi

if ! pgrep -f "${BUILD_DIR}/lunar-panel" >/dev/null 2>&1; then
    echo "[lunar] Error: lunar-panel is not running. Log:" >&2
    cat "${PANEL_LOG}" >&2 || true
    exit 1
fi

echo "[lunar] Lunar DE is live on DISPLAY=${TEST_DISPLAY}!"
echo "[lunar]   WM log:       ${WM_LOG}"
echo "[lunar]   Panel log:    ${PANEL_LOG}"
echo "[lunar]   Terminal log: ${TERM_LOG}"
echo "[lunar] Keybindings: Super (App Menu), Super+G (Gemini Live), Super+Return (terminal), Super+Q (close), Super+M/Up (maximize), Super+Down (restore), Super+Left/Right (snap), Super+F (fullscreen), Super+Escape (exit WM)"
