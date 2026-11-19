#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEB_PATH="${1:-}"
PAGES_DIR="${2:-${ROOT_DIR}/docs}"

if [[ -z "${DEB_PATH}" ]]; then
    DEB_PATH="$(ls -t "${ROOT_DIR}/dist"/lunar-de_*_amd64.deb 2>/dev/null | head -n1 || true)"
fi

if [[ -z "${DEB_PATH}" || ! -f "${DEB_PATH}" ]]; then
    echo "[lunar] Error: No .deb package found. Pass a .deb path or run ./scripts/build_deb.sh first." >&2
    exit 1
fi

for tool in dpkg-scanpackages apt-ftparchive gpg gzip; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "[lunar] Error: Required tool '${tool}' not found in PATH." >&2
        exit 1
    fi
done

if [[ -z "${LUNAR_GPG_KEY:-}" ]]; then
    echo "[lunar] Error: Set LUNAR_GPG_KEY to your local GPG key ID or fingerprint before publishing." >&2
    echo "[lunar] Example: gpg --quick-generate-key 'Lunar DE Repo <lunar@users.noreply.github.com>' rsa4096 sign 2y" >&2
    exit 1
fi

if ! gpg --list-secret-keys "${LUNAR_GPG_KEY}" >/dev/null 2>&1; then
    echo "[lunar] Error: Secret GPG key '${LUNAR_GPG_KEY}' not found in local keyring." >&2
    exit 1
fi

APT_DIR="${PAGES_DIR}/apt"
POOL_DIR="${APT_DIR}/pool/main"
BIN_DIR="${APT_DIR}/dists/noble/main/binary-amd64"
DIST_DIR="${APT_DIR}/dists/noble"

mkdir -p "${POOL_DIR}" "${BIN_DIR}"
cp -f "${DEB_PATH}" "${POOL_DIR}/"

(
    cd "${APT_DIR}"
    dpkg-scanpackages --arch amd64 --multiversion pool/main > "${BIN_DIR}/Packages"
    gzip -9c "${BIN_DIR}/Packages" > "${BIN_DIR}/Packages.gz"
)

(
    cd "${DIST_DIR}"
    rm -f Release Release.gpg InRelease
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="Lunar DE" \
        -o APT::FTPArchive::Release::Label="Lunar DE" \
        -o APT::FTPArchive::Release::Suite="noble" \
        -o APT::FTPArchive::Release::Codename="noble" \
        -o APT::FTPArchive::Release::Architectures="amd64" \
        -o APT::FTPArchive::Release::Components="main" \
        release . > Release

    gpg --batch --yes --default-key "${LUNAR_GPG_KEY}" -abs -o Release.gpg Release
    gpg --batch --yes --default-key "${LUNAR_GPG_KEY}" --clearsign -o InRelease Release
)

gpg --batch --yes --export "${LUNAR_GPG_KEY}" > "${APT_DIR}/lunar-archive-keyring.gpg"
touch "${PAGES_DIR}/.nojekyll"

echo "[lunar] Published APT repository into ${APT_DIR}"
echo "[lunar]   Keyring:  ${APT_DIR}/lunar-archive-keyring.gpg"
echo "[lunar]   Packages: ${BIN_DIR}/Packages(.gz)"
echo "[lunar]   Signed:   ${DIST_DIR}/InRelease & Release.gpg"
