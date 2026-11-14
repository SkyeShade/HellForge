#!/usr/bin/env bash
# Install HellForge's root-owned Sync Inventory helper and its relay (docs/linux.md, "Account sync"):
#   sudo bash allow-capture.sh
#   sudo bash allow-capture.sh --remove
# The launcher runs the same script through pkexec, so the desktop's polkit dialog asks for the password:
#   pkexec /usr/bin/bash allow-capture.sh [--remove]
# It copies capture-helper.sh, capture-relay and the Linux capture (app/capture-linux, as capture-proxy) to
# /usr/local/lib/hellforge (on SteamOS to /opt/hellforge, see below), records the SHA-256 of each installed copy (of the
# capture, of its file list: proxy_manifest) and gives the calling account (SUDO_USER, or PKEXEC_UID under pkexec) a
# sudoers rule for that one root-owned helper only. The launcher then starts/stops the helper without asking again, and
# reinstalls when either shipped file and the installed copy differ. The helper validates its own arguments and can only
# change HellForge's marked /etc/hosts lines and run the installed relay; it never runs Wine, a game, or a command
# chosen by the caller.
# sudo reads /etc/sudoers.d in lexical order and its last matching rule wins, so the rule's name starts with zz-: it
# then sorts after a distribution's own files (SteamOS ships "sudo" and "wheel", which ask for a password).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_helper="$here/capture-helper.sh"
source_relay="$here/capture-relay"
# The helper runs this root-owned copy as its signing service: the signer's key is only as safe as the program holding it.
source_proxy="$(cd "$here/.." && pwd)/capture-linux"
# Every folder a helper may be in, and the ones this system installs to, first choice first. SteamOS's /usr/local is on
# its read-only image, which an update replaces; its /opt is on /home, writable and kept by updates, and /var/lib is
# next. LinuxCapture.HelperFolders and capture-helper.sh name the same folders.
all_folders=(/usr/local/lib/hellforge /opt/hellforge /var/lib/hellforge)
folders=("${all_folders[@]}")
steamos=''
if grep -qxE '[[:space:]]*ID="?steamos"?[[:space:]]*' /etc/os-release 2>/dev/null; then
  steamos=1
  folders=("${all_folders[@]:1}")
fi
# From SteamOS 3.6 an update drops every /etc change that is not on its keep list; this drop-in adds the rule to it.
keep_list=/etc/atomic-update.conf.d/hellforge.conf
sudoers=/etc/sudoers.d/zz-hellforge-capture
# The rule's name before it sorted last; an installation that still has it is moved to the new name.
old_sudoers=/etc/sudoers.d/hellforge-capture

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

# Why the account must not be able to change a folder from / down to the helper's, or '' when it cannot: it could
# put its own program where sudo then runs the helper as root.
unsafe_folder() {
  local path user="$2" owner mode group
  path="$(readlink -f -- "$1")" || { printf '%s cannot be resolved' "$1"; return 0; }
  while :; do
    owner="$(stat -c %u -- "$path")"; mode="$(stat -c %a -- "$path")"; group="$(stat -c %g -- "$path")"
    if [ "$owner" != 0 ]; then printf '%s is not owned by root' "$path"; return 0; fi
    if (( (8#$mode & 8#002) != 0 )); then printf '%s is writable by every account' "$path"; return 0; fi
    if (( (8#$mode & 8#020) != 0 )) && id -G "$user" | tr ' ' '\n' | grep -qx "$group"; then
      printf '%s is writable by a group %s is in' "$path" "$user"; return 0
    fi
    [ "$path" = / ] && return 0
    path="$(dirname "$path")"
  done
}

# The first of this system's folders that can be made and that only root can change. Sets folder, or says why none.
choose_folder() {
  local user="$1" candidate why refused=''
  folder=''
  for candidate in "${folders[@]}"; do
    if ! why="$(install -d -m 755 -o root -g root "$candidate" 2>&1)"; then
      refused+="  $candidate: ${why##*: }"$'\n'
      continue
    fi
    why="$(unsafe_folder "$candidate" "$user")"
    # SteamOS binds /opt from a folder on /home; the way to that folder must be root's alone too.
    if [ -z "$why" ] && [ "$candidate" = /opt/hellforge ] && [ /opt -ef /home/.steamos/offload/opt ]; then
      why="$(unsafe_folder /home/.steamos/offload/opt "$user")"
    fi
    if [ -z "$why" ]; then folder=$candidate; return 0; fi
    refused+="  $candidate: $why"$'\n'
    rmdir "$candidate" 2>/dev/null || true
  done
  printf 'No folder for the root-owned helper can be used:\n%s' "$refused" >&2
  return 1
}

# One SHA-256 over every file's SHA-256 and path, in byte order. capture-helper.sh and LinuxCapture.FolderManifestSha256
# compute the same.
proxy_manifest() {
  (cd "$1" && LC_ALL=C find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) | sha256sum | awk '{print $1}'
}

root_only() {
  [ -e "$1" ] && [ ! -L "$1" ] && [ "$(stat -c %u -- "$1")" = 0 ] && (( (8#$(stat -c %a -- "$1") & 8#022) == 0 ))
}

# The helper's files in one folder, and the folder when nothing else is in it. A read-only folder is left as it is.
remove_copies() {
  rm -f "$1/capture-helper" "$1/capture-helper.sha256" "$1/capture-relay" "$1/capture-relay.sha256" 2>/dev/null || true
  rm -rf "$1/capture-proxy" "$1/capture-proxy.new" "$1/capture-proxy.sha256" 2>/dev/null || true
  rmdir "$1" 2>/dev/null || true
}

if [ "${1:-}" = "--test-user" ]; then
  resolve_user
  exit 0
fi
[ "$(id -u)" = 0 ] || { echo "run it with sudo or pkexec: bash $0 ${*:-}" >&2; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  # Revoke the rule first; the installed helper is root-owned and still available for this cleanup.
  rm -f "$sudoers" "$old_sudoers" "$keep_list"
  for installed in "${all_folders[@]}"; do
    # Run as root only a helper that root alone could have put there.
    if root_only "$installed" && root_only "$installed/capture-helper" && [ -x "$installed/capture-helper" ]; then
      "$installed/capture-helper" cleanup || true
    fi
    remove_copies "$installed"
  done
  # The signer goes with the helper: no other program may sign for this PC.
  rm -f /var/lib/hellforge/snapshot-signer.pub
  rm -rf /var/lib/private/hellforge-signer /var/lib/hellforge-signer
  rmdir /var/lib/hellforge 2>/dev/null || true
  echo "Removed HellForge's Account Sync helper, relay and snapshot signer."
  exit 0
fi

[ -f "$source_helper" ] || { echo "capture-helper.sh is missing beside this installer." >&2; exit 1; }
[ -f "$source_relay" ] || { echo "capture-relay is missing beside this installer." >&2; exit 1; }
[ -x "$source_proxy/HellForge.CaptureProxy" ] || { echo "the Linux capture ($source_proxy) is missing." >&2; exit 1; }
bash -n "$source_helper" || { echo "capture-helper.sh is not a valid script." >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is missing; install coreutils first." >&2; exit 1; }
user="$(resolve_user)"
command -v visudo >/dev/null || { echo "visudo is missing; install sudo first." >&2; exit 1; }

choose_folder "$user"
helper=$folder/capture-helper
hash_file=$folder/capture-helper.sha256
relay=$folder/capture-relay
relay_hash_file=$folder/capture-relay.sha256
install -m 755 -o root -g root "$source_helper" "$helper"
install -m 755 -o root -g root "$source_relay" "$relay"
proxy=$folder/capture-proxy
proxy_hash_file=$folder/capture-proxy.sha256
rm -rf "$proxy.new"
cp -R --no-preserve=ownership --preserve=mode "$source_proxy" "$proxy.new"
chown -R root:root "$proxy.new"
chmod -R u+rwX,go+rX,go-w "$proxy.new"
rm -rf "$proxy"
mv "$proxy.new" "$proxy"
proxy_hash="$(proxy_manifest "$proxy")"
helper_hash="$(sha256sum "$helper" | awk '{print $1}')"
relay_hash="$(sha256sum "$relay" | awk '{print $1}')"
[[ "$helper_hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed helper." >&2; exit 1; }
[[ "$relay_hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed relay." >&2; exit 1; }
[[ "$proxy_hash" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "could not hash the installed capture." >&2; exit 1; }
printf '%s\n' "$helper_hash" > "$hash_file"
printf '%s\n' "$relay_hash" > "$relay_hash_file"
printf '%s\n' "$proxy_hash" > "$proxy_hash_file"
chmod 644 "$hash_file" "$relay_hash_file" "$proxy_hash_file"
chown root:root "$hash_file" "$relay_hash_file" "$proxy_hash_file"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
# No wildcards: the helper's start reads the Steam account and the API's address from stdin, and its ports are fixed.
printf '%s ALL=(root) NOPASSWD: %s check, %s start, %s stop, %s cleanup\n' \
  "$user" "$helper" "$helper" "$helper" "$helper" > "$tmp"
chmod 440 "$tmp"
visudo -cf "$tmp" >/dev/null
install -m 440 -o root -g root "$tmp" "$sudoers"
rm -f "$tmp" "$old_sudoers"
trap - EXIT
# The rule now names this folder's helper; a copy an earlier version left in another folder is of no use.
for installed in "${all_folders[@]}"; do
  [ "$installed" = "$folder" ] || remove_copies "$installed"
done
if [ -n "$steamos" ]; then
  install -d -m 755 "$(dirname "$keep_list")"
  printf '%s\n' "$sudoers" > "$keep_list"
  chmod 644 "$keep_list"
fi
echo "HellForge may now run its Account Sync helper without another password prompt ($helper, for $user)."
echo "Installed helper SHA-256: $helper_hash"
echo "Installed relay SHA-256: $relay_hash"
echo "Installed capture manifest SHA-256: $proxy_hash"
echo "Remove them with: sudo bash $0 --remove"
