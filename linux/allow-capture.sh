#!/usr/bin/env bash
# Install HellForge's root-owned Sync Inventory helper and its relay (docs/linux.md, "Account sync"):
#   sudo bash allow-capture.sh
#   sudo bash allow-capture.sh --remove
# The launcher runs the same script through pkexec, so the desktop's polkit dialog asks for the password:
#   pkexec /usr/bin/bash allow-capture.sh [--remove]
# It copies capture-helper.sh and capture-relay to /usr/local/lib/hellforge, records the SHA-256 of each installed copy
# and gives the calling account (SUDO_USER, or PKEXEC_UID under pkexec) a sudoers rule for that one root-owned helper
# only. The launcher then starts/stops the helper without asking again, and reinstalls when either shipped file and the
# installed copy differ. The helper validates its own arguments and can only change HellForge's marked /etc/hosts lines
# and run the installed relay; it never runs Wine, a game, or a command chosen by the caller.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_helper="$here/capture-helper.sh"
source_relay="$here/capture-relay"
helper=/usr/local/lib/hellforge/capture-helper
hash_file=/usr/local/lib/hellforge/capture-helper.sha256
relay=/usr/local/lib/hellforge/capture-relay
relay_hash_file=/usr/local/lib/hellforge/capture-relay.sha256
sudoers=/etc/sudoers.d/hellforge-capture

# sudo names the caller in SUDO_USER; pkexec names the uid in PKEXEC_UID (test-only --test-user prints this account).
resolve_user() {
  local user=''
  if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    user=$SUDO_USER
  elif [ -n "${PKEXEC_UID:-}" ]; then
    [[ "$PKEXEC_UID" =~ ^[0-9]+$ ]] || {
      echo "PKEXEC_UID is not a numeric uid: $PKEXEC_UID" >&2
      return 1
    }
    user="$(id -nu "$PKEXEC_UID" 2>/dev/null)" || {
      echo "no account for PKEXEC_UID $PKEXEC_UID" >&2
      return 1
    }
  fi
  [ -n "$user" ] && [ "$user" != root ] || {
    echo "run it with sudo or pkexec from the account that plays HellForge" >&2
    return 1
  }
  [[ "$user" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]] || {
    echo "the account name is not safe for a sudoers rule: $user" >&2
    return 1
  }
  id -u "$user" >/dev/null 2>&1 || {
    echo "no such account: $user" >&2
    return 1
  }
  printf '%s' "$user"
}

if [ "${1:-}" = "--test-user" ]; then
  resolve_user
  exit 0
fi
[ "$(id -u)" = 0 ] || { echo "run it with sudo or pkexec: bash $0 ${*:-}" >&2; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  # Revoke the rule first; the installed helper is root-owned and still available for this cleanup.
  rm -f "$sudoers"
  [ -x "$helper" ] && "$helper" cleanup || true
  rm -f "$helper" "$hash_file" "$relay" "$relay_hash_file"
  rmdir /usr/local/lib/hellforge 2>/dev/null || true
  echo "Removed HellForge's Account Sync helper and relay."
  exit 0
fi

[ -f "$source_helper" ] || { echo "capture-helper.sh is missing beside this installer." >&2; exit 1; }
[ -f "$source_relay" ] || { echo "capture-relay is missing beside this installer." >&2; exit 1; }
bash -n "$source_helper" || { echo "capture-helper.sh is not a valid script." >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is missing; install coreutils first." >&2; exit 1; }
user="$(resolve_user)"
command -v visudo >/dev/null || { echo "visudo is missing; install sudo first." >&2; exit 1; }

install -d -m 755 /usr/local/lib/hellforge
install -m 755 -o root -g root "$source_helper" "$helper"
install -m 755 -o root -g root "$source_relay" "$relay"
helper_hash="$(sha256sum "$helper" | awk '{print $1}')"
relay_hash="$(sha256sum "$relay" | awk '{print $1}')"
[[ "$helper_hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed helper." >&2; exit 1; }
[[ "$relay_hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed relay." >&2; exit 1; }
printf '%s\n' "$helper_hash" > "$hash_file"
printf '%s\n' "$relay_hash" > "$relay_hash_file"
chmod 644 "$hash_file" "$relay_hash_file"
chown root:root "$hash_file" "$relay_hash_file"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
# No wildcards: the helper's start reads the proxy pid from stdin and its relay ports are fixed (443 -> 18443).
printf '%s ALL=(root) NOPASSWD: %s check, %s start, %s stop, %s cleanup\n' \
  "$user" "$helper" "$helper" "$helper" "$helper" > "$tmp"
chmod 440 "$tmp"
visudo -cf "$tmp" >/dev/null
install -m 440 -o root -g root "$tmp" "$sudoers"
rm -f "$tmp"
trap - EXIT
echo "HellForge may now run its Account Sync helper without another password prompt ($helper, for $user)."
echo "Installed helper SHA-256: $helper_hash"
echo "Installed relay SHA-256: $relay_hash"
echo "Remove them with: sudo bash $0 --remove"
