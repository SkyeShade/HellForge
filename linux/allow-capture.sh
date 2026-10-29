#!/usr/bin/env bash
# Install HellForge's root-owned Sync Inventory helper once (docs/linux.md, "Account sync"):
#   sudo bash allow-capture.sh
#   sudo bash allow-capture.sh --remove
# It copies capture-helper.sh to /usr/local/lib/hellforge/capture-helper, records the SHA-256 of the installed copy and
# gives the account that ran sudo a sudoers rule for that one root-owned program only. The launcher then starts/stops the
# helper without asking again, and refuses to sync when the shipped helper and the installed copy differ. The helper
# validates its own arguments and can only change HellForge's marked /etc/hosts lines and the loopback 443 relay; it never
# runs Wine, a game, or a command chosen by the caller.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo bash $0 ${*:-}" >&2; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_helper="$here/capture-helper.sh"
helper=/usr/local/lib/hellforge/capture-helper
hash_file=/usr/local/lib/hellforge/capture-helper.sha256
sudoers=/etc/sudoers.d/hellforge-capture

if [ "${1:-}" = "--remove" ]; then
  # Revoke the rule first; the installed helper is root-owned and still available for this cleanup.
  rm -f "$sudoers"
  [ -x "$helper" ] && "$helper" cleanup || true
  rm -f "$helper" "$hash_file"
  rmdir /usr/local/lib/hellforge 2>/dev/null || true
  echo "Removed HellForge's Account Sync helper."
  exit 0
fi

[ -f "$source_helper" ] || { echo "capture-helper.sh is missing beside this installer." >&2; exit 1; }
bash -n "$source_helper" || { echo "capture-helper.sh is not a valid script." >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is missing; install coreutils first." >&2; exit 1; }
user="${SUDO_USER:-}"
[ -n "$user" ] && [ "$user" != root ] || { echo "run it with sudo from the account that plays HellForge" >&2; exit 1; }
[[ "$user" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]] || { echo "the account name is not safe for a sudoers rule: $user" >&2; exit 1; }
id -u "$user" >/dev/null 2>&1 || { echo "no such account: $user" >&2; exit 1; }
command -v visudo >/dev/null || { echo "visudo is missing; install sudo first." >&2; exit 1; }

install -d -m 755 /usr/local/lib/hellforge
install -m 755 -o root -g root "$source_helper" "$helper"
hash="$(sha256sum "$helper" | awk '{print $1}')"
[[ "$hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed helper." >&2; exit 1; }
printf '%s\n' "$hash" > "$hash_file"
chmod 644 "$hash_file"
chown root:root "$hash_file"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
# No wildcards: the helper's start reads the proxy pid from stdin and its relay port is the fixed 18443.
printf '%s ALL=(root) NOPASSWD: %s check, %s start, %s stop, %s cleanup\n' \
  "$user" "$helper" "$helper" "$helper" "$helper" > "$tmp"
chmod 440 "$tmp"
visudo -cf "$tmp" >/dev/null
install -m 440 -o root -g root "$tmp" "$sudoers"
rm -f "$tmp"
trap - EXIT
echo "HellForge may now run its Account Sync helper without another password prompt ($helper, for $user)."
echo "Installed helper SHA-256: $hash"
echo "Remove it with: sudo bash $0 --remove"
