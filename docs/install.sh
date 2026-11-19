#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "[lunar] Please run as root (e.g., curl -fsSL https://lunar-entertainment.github.io/Lunar_DE/install.sh | sudo bash)" >&2
    exit 1
fi

REPO_BASE="https://lunar-entertainment.github.io/Lunar_DE/apt"
KEYRING_PATH="/usr/share/keyrings/lunar-archive-keyring.gpg"
LIST_PATH="/etc/apt/sources.list.d/lunar-de.list"

export DEBIAN_FRONTEND=noninteractive

if ! command -v curl >/dev/null 2>&1; then
    echo "[lunar] Installing curl and ca-certificates..."
    apt-get update
    apt-get install -y curl ca-certificates
fi

install -d -m 0755 /usr/share/keyrings /etc/apt/sources.list.d

echo "[lunar] Downloading Lunar DE APT archive signing key..."
curl -fsSL "${REPO_BASE}/lunar-archive-keyring.gpg" -o "${KEYRING_PATH}"
chmod 0644 "${KEYRING_PATH}"

echo "[lunar] Adding APT repository ${REPO_BASE}..."
echo "deb [arch=amd64 signed-by=${KEYRING_PATH}] ${REPO_BASE} noble main" > "${LIST_PATH}"
chmod 0644 "${LIST_PATH}"

echo "[lunar] Updating package lists and installing lunar-de..."
apt-get update
apt-get install -y lunar-de

echo "[lunar] Lunar DE installed! Auto-updates on reboot are enabled via lunar-de-autoupdate.service."
echo "[lunar] Log out and select 'Lunar DE' at your login screen to start."
