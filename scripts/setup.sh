#!/usr/bin/env bash
# Install the lab tools on a fresh Ubuntu Arm instance.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]] || ! command -v dpkg >/dev/null \
    || [[ "$(dpkg --print-architecture)" != arm64 ]]; then
    echo "Run this setup on your Ubuntu Arm lab instance." >&2
    exit 1
fi

if [[ "$EUID" -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

LOG=/var/log/lab_setup.log
export DEBIAN_FRONTEND=noninteractive
APT=(apt-get -y -o DPkg::Lock::Timeout=600)
TMPD=""
trap 'rm -rf "${TMPD:-}"' EXIT
trap 'echo "Setup failed. Run: sudo tail -n 80 /var/log/lab_setup.log" >&2' ERR

echo "Waiting for first-boot setup (cloud-init)"
cloud-init status --wait || true

echo "Installing developer tools"
"${APT[@]}" update >>"$LOG" 2>&1
"${APT[@]}" install build-essential gcc g++ clang gdb make cmake git curl unzip \
    python3 python3-yaml ca-certificates >>"$LOG" 2>&1

echo "Installing Linux perf"
"${APT[@]}" install "linux-tools-$(uname -r)" >>"$LOG" 2>&1 \
    || "${APT[@]}" install linux-tools-aws >>"$LOG" 2>&1
perf --version

echo "Finding the latest APerf Arm64 package"
APERF_URL="$(curl -fsSL --retry 3 --retry-delay 2 \
    https://api.github.com/repos/aws/aperf/releases/latest \
    | python3 -c 'import json, sys; print(next(asset["browser_download_url"] for asset in json.load(sys.stdin)["assets"] if asset["name"].endswith("arm64.deb")))')" \
    || APERF_URL=""

if [[ -z "$APERF_URL" ]]; then
    echo "The GitHub API did not answer. Using the release link."
    TAG="$(curl -fsSL --retry 3 -o /dev/null -w '%{url_effective}' \
        https://github.com/aws/aperf/releases/latest | sed 's#.*/tag/##')"
    [[ -n "$TAG" && "$TAG" != */* ]]
    APERF_URL="https://github.com/aws/aperf/releases/download/$TAG/aperf-$TAG-arm64.deb"
fi

echo "Installing APerf from $APERF_URL"
TMPD="$(mktemp -d)"
chmod 755 "$TMPD"
curl -fsSL --retry 3 -o "$TMPD/aperf.deb" "$APERF_URL"
"${APT[@]}" install "$TMPD/aperf.deb" >>"$LOG" 2>&1
aperf --version

echo "Enabling performance counters and profiling"
cat > /etc/sysctl.d/99-aperf.conf <<'SYSCTL'
kernel.perf_event_paranoid = -1
kernel.kptr_restrict = 0
SYSCTL
sysctl -p /etc/sysctl.d/99-aperf.conf

echo "Checking the installed tools"
gcc --version | head -n 1
make --version | head -n 1
python3 --version
perf stat -e cycles,instructions -- sleep 1

echo "Setup is complete. You can now run make benchmark."
