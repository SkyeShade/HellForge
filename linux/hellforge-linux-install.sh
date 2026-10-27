#!/usr/bin/env bash
# hellforge-linux-install.sh: guided HellForge setup on Linux (Proton). Experimental helper.
#
#   ./hellforge-linux-install.sh                                   no prefix: offers to restore a backup of it, or to install; otherwise lists the commands
#   ./hellforge-linux-install.sh install [HellForge-Setup.exe]     guided install (re-run it after a failure: done steps are skipped)
#   ./hellforge-linux-install.sh run                               start HellForge (hellforge-linux.sh with the right settings)
#   ./hellforge-linux-install.sh proton                            pick a different Proton (remembered for next time)
#   ./hellforge-linux-install.sh where                             show where the prefix, install, Steam and Proton are
#   ./hellforge-linux-install.sh build <mod-folder>                build a HellForge mod with a native Linux Zig and install it into
#                                                                  HellForge's mods/ folder (replaces build-hf-mod.fish)
#   ./hellforge-linux-install.sh sdk [mod-folder]                  show the mod SDK folder; with a mod folder, copy the SDK headers
#                                                                  (hellforge_mod.h ...) into <mod-folder>/src so the mod builds
#   ./hellforge-linux-install.sh backup [folder]                   back up the whole prefix (default: ~/hellforge-backups) and list the backups
#   ./hellforge-linux-install.sh restore [backup-file]             restore the prefix from a `backup` (only when there is no prefix)
#   ./hellforge-linux-install.sh update [HellForge-Setup.exe]      back up the HellForge folder, then run the newest setup in the prefix
#   ./hellforge-linux-install.sh shortcut                          add an app-menu entry and a `hellforge-linux` command
#   ./hellforge-linux-install.sh uninstall                         remove what this script set up (asks first)
#
# install, in order:
#   1. Gets the three HELLDIVERS 2 depots (first offers to delete leftovers of an earlier download: app_553850 and
#      state_553850_*.patch in Steam's content folder): prints the download_depot commands, opens Steam's console, waits.
#   2. Runs HellForge-Setup.exe in a Wine prefix of its own (you click Install, let it open, let it fail, close it).
#   3. Moves the depots to where setup looks for them inside that prefix:
#        <prefix>/pfx/drive_c/Program Files (x86)/Steam/steamapps/content/app_553850/
#      (moved, not copied: instant on one filesystem, no extra disk space, and no symlinks, which HellForge rejects).
#   4. Adds the app-menu entry and `hellforge-linux` command (same as `shortcut`, done automatically the first time),
#      makes app/linux/*.sh executable, handles Ubuntu's namespace permission, and starts hellforge-linux.sh.
# Works with Steam as a native package (Arch, Ubuntu/Debian .deb, ...), and, untested, as Flatpak or Snap.
#
# Settings are remembered in ~/.config/hellforge-linux/config. Environment variables override them:
#   PROTON      the proton script to use (otherwise the last one used; if that is gone, found, and asked if several)
#   PFX         the Wine prefix folder                (default ~/.local/share/hellforge-prefix)
#   STEAM_ROOT  the Steam client's folder             (default: found; asks if there are several)
#   SETUP       the HellForge-Setup.exe               (or give it as an argument; otherwise ./ and ~/Downloads are searched)
#   BACKUP_DIR  where backups go                      (default ~/hellforge-backups)
#   ZIG         the native Linux zig for `build`      (default ~/.local/share/zig-native/zig, else zig from PATH)
set -euo pipefail

APP=553850
# "<depot> <manifest>": the build HellForge pins. Change here if HellForge pins another build.
DEPOTS=("553853 2566883558412717494" "553854 9065746766815828060" "553851 1883032874832282552")
# total number of files in app_553850 once all three depots are fully downloaded (change here if the pinned build changes)
EXPECTED_FILES=157
# number of files in the HellForge folder (Documents/HellForge in the prefix) once setup has fully installed it
HF_FILES=748

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hellforge-linux"
CFG="$CFG_DIR/config"
BIN_COPY="$HOME/.local/bin/hellforge-linux"
DESKTOP_FILE="${XDG_DATA_HOME:-$HOME/.local/share}/applications/hellforge-linux.desktop"
DEFAULT_PFX="$HOME/.local/share/hellforge-prefix"
BACKUP_DIR="${BACKUP_DIR:-$HOME/hellforge-backups}"
NS_COPY=/usr/local/lib/hellforge/unshare          # made by HellForge's allow-namespace.sh (Ubuntu 24.04+)
NS_PROFILE=/etc/apparmor.d/hellforge-unshare

say()   { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn()  { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
pause() { read -r -p "$* [Enter] " _ || true; }
ask_yes() { local a=""; read -r -p "$1 [Y/n] " a || true; [[ ! "$a" =~ ^[Nn] ]]; }  # default yes
ask_yn() { local a=""; read -r -p "$1 [y/N] " a || true; [[ "$a" =~ ^[Yy] ]]; }

# ---- settings -------------------------------------------------------------------------------------------------------
PFX="" PROTON="" STEAM_ROOT="" PFX_OWNED="" SAVED_PROTON=""

load_settings() {  # saved config first, then environment variables win
  local e_pfx="${PFX:-}" e_proton="${PROTON:-}" e_steam="${STEAM_ROOT:-}"
  PFX="" PROTON="" STEAM_ROOT="" PFX_OWNED=""
  if [ -f "$CFG" ]; then
    # shellcheck disable=SC1090
    . "$CFG"
  fi
  SAVED_PROTON="$PROTON"   # what the config says, before an environment variable can override it
  [ -n "$e_pfx" ] && PFX="$e_pfx"
  [ -n "$e_proton" ] && PROTON="$e_proton"
  [ -n "$e_steam" ] && STEAM_ROOT="$e_steam"
  PFX="${PFX:-$DEFAULT_PFX}"
  return 0
}

save_settings() {
  mkdir -p "$CFG_DIR"
  {
    echo "# written by hellforge-linux-install.sh"
    printf 'PFX=%q\n' "$PFX"
    printf 'PROTON=%q\n' "$PROTON"
    printf 'STEAM_ROOT=%q\n' "$STEAM_ROOT"
    printf 'PFX_OWNED=%q\n' "$PFX_OWNED"
  } > "$CFG"
}

remember_proton() {  # store the Proton in use as the last used one; touches only the PROTON line of the config
  [ -n "$PROTON" ] && [ "$PROTON" != "$SAVED_PROTON" ] || return 0
  mkdir -p "$CFG_DIR"
  local line tmp
  line="$(printf 'PROTON=%q' "$PROTON")"
  if [ -f "$CFG" ] && grep -q '^PROTON=' "$CFG"; then
    tmp="$(mktemp "$CFG_DIR/config.XXXXXX")"
    while IFS= read -r l || [ -n "$l" ]; do
      case "$l" in PROTON=*) printf '%s\n' "$line" ;; *) printf '%s\n' "$l" ;; esac
    done < "$CFG" > "$tmp"
    mv "$tmp" "$CFG"
  else
    printf '%s\n' "$line" >> "$CFG"
  fi
  SAVED_PROTON="$PROTON"
  echo "Remembered Proton: $PROTON"
}

# ---- Steam (native, Flatpak or Snap) --------------------------------------------------------------------------------
steam_kind() {
  case "$STEAM_ROOT" in
    *com.valvesoftware.Steam*) echo flatpak ;;
    */snap/steam/*) echo snap ;;
    *) echo native ;;
  esac
}

steam_candidates() {
  local c
  {
    for c in "$HOME/.local/share/Steam" "$(readlink -f "$HOME/.steam/steam" 2>/dev/null || true)" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
      if [ -n "$c" ] && { [ -d "$c/steamapps" ] || [ -d "$c/ubuntu12_32" ] || [ -d "$c/steamrt64" ]; }; then echo "$c"; fi
    done
  } | awk '!seen[$0]++'
}

resolve_steam_root() {  # $1 = need (die when none) | interactive (also ask when several) | quiet
  [ -z "$STEAM_ROOT" ] || return 0
  local found=()
  mapfile -t found < <(steam_candidates)
  case ${#found[@]} in
    0) [ "${1:-quiet}" = quiet ] || die "Steam folder not found; set STEAM_ROOT (the folder that holds steamapps/)" ;;
    1) STEAM_ROOT="${found[0]}" ;;
    *) if [ "${1:-quiet}" = interactive ] && [ -t 0 ]; then
         echo "Which Steam?"; select STEAM_ROOT in "${found[@]}"; do [ -n "${STEAM_ROOT:-}" ] && break; done
       else STEAM_ROOT="${found[0]}"; fi ;;
  esac
}

steam_running() {
  pgrep -u "$(id -u)" -x steam >/dev/null 2>&1 || pgrep -u "$(id -u)" -f 'ubuntu12_32/steam' >/dev/null 2>&1
}

start_steam_console() {
  case "$(steam_kind)" in
    flatpak) command -v flatpak >/dev/null || die "'flatpak' not found"; (flatpak run com.valvesoftware.Steam -console >/dev/null 2>&1 &) ;;
    *)       command -v steam >/dev/null || die "'steam' not found in PATH"; (steam -console >/dev/null 2>&1 &) ;;
  esac
}

set_paths() {  # everything derived from PFX and STEAM_ROOT
  # Where Steam's console puts download_depot files: steamrt64 or ubuntu12_32 depending on the Steam client
  # (plain steamapps is a fallback). All of them are checked, counted and cleaned.
  SRC_CANDIDATES=("$STEAM_ROOT/steamrt64/steamapps/content/app_$APP" "$STEAM_ROOT/ubuntu12_32/steamapps/content/app_$APP" "$STEAM_ROOT/steamapps/content/app_$APP")
  CONTENT_SRC="${SRC_CANDIDATES[0]}"
  PFX_STEAM="$PFX/pfx/drive_c/Program Files (x86)/Steam"
  CONTENT_DST="$PFX_STEAM/steamapps/content/app_$APP"
}

# ---- dependencies, Proton, helpers ----------------------------------------------------------------------------------
dep_hint() {
  if   command -v pacman  >/dev/null; then echo "sudo pacman -S --needed iproute2 socat util-linux procps-ng"
  elif command -v apt-get >/dev/null; then echo "sudo apt install iproute2 socat util-linux procps"
  elif command -v dnf     >/dev/null; then echo "sudo dnf install iproute socat util-linux procps-ng"
  elif command -v zypper  >/dev/null; then echo "sudo zypper install iproute2 socat util-linux procps"
  fi
}

missing_deps() {
  local c; for c in socat ip ss unshare pgrep; do command -v "$c" >/dev/null || echo "$c"; done
}

check_deps() {  # $1 = interactive: offer to install what is missing
  local missing hint
  missing="$(missing_deps | tr '\n' ' ')"
  [ -n "$missing" ] || return 0
  hint="$(dep_hint)"
  warn "missing: $missing"
  if [ "${1:-}" = interactive ] && [ -n "$hint" ] && [ -t 0 ] && ask_yn "Install them now with: $hint ?"; then
    bash -c "$hint" || die "the install command failed"
    [ -z "$(missing_deps)" ] || die "still missing: $(missing_deps | tr '\n' ' ')"
    return 0
  fi
  die "install them first${hint:+: $hint}"
}

find_proton() {
  if [ -n "$PROTON" ] && [ ! -x "$PROTON" ]; then warn "The last used Proton is gone: $PROTON; looking again"; PROTON=""; fi
  if [ -n "$PROTON" ]; then remember_proton; return 0; fi
  local found=()
  mapfile -t found < <(ls -d "$STEAM_ROOT"/compatibilitytools.d/*/proton "$STEAM_ROOT"/steamapps/common/Proton*/proton 2>/dev/null | sort -V || true)
  case ${#found[@]} in
    0) die "no Proton found under $STEAM_ROOT; set PROTON to a proton script" ;;
    1) PROTON="${found[0]}" ;;
    *) echo "Which Proton?"; select PROTON in "${found[@]}"; do [ -n "${PROTON:-}" ] && break; done ;;
  esac
  [ -x "$PROTON" ] || die "not executable: $PROTON"
  remember_proton
}

pr() { env STEAM_COMPAT_DATA_PATH="$PFX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" "$PROTON" run "$@"; }

depots_in() {  # all three depot folders present in $1
  local d; for d in "${DEPOTS[@]}"; do [ -d "$1/depot_${d%% *}" ] || return 1; done
}

locate_src() {  # sets CONTENT_SRC to the candidate that holds all three depots; false when none does
  local c
  for c in "${SRC_CANDIDATES[@]}"; do
    if depots_in "$c"; then CONTENT_SRC="$c"; return 0; fi
  done
  return 1
}

src_where() {  # the download folder in use: the candidate holding the most files (else the first)
  local c best="${SRC_CANDIDATES[0]}" n max=0
  for c in "${SRC_CANDIDATES[@]}"; do
    [ -d "$c" ] || continue
    n="$(find "$c" -type f 2>/dev/null | wc -l)"
    if [ "$n" -gt "$max" ]; then max="$n"; best="$c"; fi
  done
  echo "$best"
}

src_where_all() {  # every place the download may land, for messages shown before anything exists
  local c out="" sep=""
  for c in "${SRC_CANDIDATES[@]:0:2}"; do out="$out$sep$c"; sep=" or "; done
  echo "$out"
}

count_files() {  # files in app_553850 (the most any candidate folder holds)
  local c n=0 m
  for c in "${SRC_CANDIDATES[@]}"; do
    [ -d "$c" ] || continue
    m="$(find "$c" -type f 2>/dev/null | wc -l)"
    if [ "$m" -gt "$n" ]; then n="$m"; fi
  done
  echo "$n"
}

src_kb() {  # disk space used by the download folder, in KiB (grows while Steam writes)
  local c n=0 m
  for c in "${SRC_CANDIDATES[@]}"; do
    [ -d "$c" ] || continue
    m="$(du -sk "$c" 2>/dev/null | cut -f1)"
    if [ "${m:-0}" -gt "$n" ]; then n="$m"; fi
  done
  echo "$n"
}

download_active() {  # true if files or data are still arriving (Steam keeps downloading after Ctrl-C); looks for ~8 seconds
  local n0 k0 i
  n0="$(count_files)"; k0="$(src_kb)"
  for i in 1 2 3 4 5 6 7 8; do
    sleep 1
    if [ "$(count_files)" != "$n0" ] || [ "$(src_kb)" != "$k0" ]; then return 0; fi
  done
  return 1
}

src_complete() {  # all three depot folders and all EXPECTED_FILES files
  locate_src && [ "$(count_files)" -ge "$EXPECTED_FILES" ]
}

draw_bar() {  # $1 = files now, $2 = optional note, $3 = total (default EXPECTED_FILES); 100% only when all are there
  local n="$1" t="${3:-$EXPECTED_FILES}" w=30 pct filled bar
  if [ "$n" -gt "$t" ]; then n="$t"; fi
  pct=$((n * 100 / t)); filled=$((n * w / t))
  bar="$(printf '%*s' "$filled" '' | tr ' ' '#')$(printf '%*s' "$((w - filled))" '' | tr ' ' '-')"
  if [ -t 1 ]; then
    printf '\r\033[K  [%s] %3d%%  %d/%d files%s' "$bar" "$pct" "$n" "$t" "${2:-}"
  else
    printf '  [%s] %3d%%  %d/%d files%s\n' "$bar" "$pct" "$n" "$t" "${2:-}"
  fi
}

watch_download() {  # progress bar by file count; the count is checked every second and the bar redrawn when it changes
  local n prev=-1 last=-1 stable=0 size rc
  while true; do
    n="$(count_files)"
    if [ "$n" != "$prev" ]; then
      if [ "$n" -ge "$EXPECTED_FILES" ]; then draw_bar "$n" "  (finishing...)"; else draw_bar "$n"; fi
      prev="$n"
    fi
    if [ "$n" -ge "$EXPECTED_FILES" ] && locate_src; then  # all files there: wait until Steam stops writing
      size="$(du -sk "$CONTENT_SRC" 2>/dev/null | cut -f1)"
      if [ "$size" = "$last" ]; then stable=$((stable + 1)); else stable=0; fi
      last="$size"
      if [ "$stable" -ge 6 ]; then break; fi
    else
      stable=0; last=-1
    fi
    rc=0; read -r -t 1 -s _ || rc=$?  # Enter = "it is done"; EOF or timeout = keep watching
    if [ "$rc" -eq 0 ]; then
      n="$(count_files)"
      if [ "$n" -ge "$EXPECTED_FILES" ] && locate_src; then break; fi  # Enter only counts once every file is there
      if [ -t 1 ]; then echo; fi
      echo "Not all files are there yet ($n of $EXPECTED_FILES). The install cannot continue until the download is complete; still watching (Ctrl-C to stop)."
      prev=-1
    elif [ "$rc" -le 128 ]; then sleep 1; fi
  done
  draw_bar "$(count_files)" "  "
  if [ -t 1 ]; then echo; fi
}

find_install() {  # the HellForge installation inside the prefix
  local f="$PFX/pfx/drive_c/users/steamuser/Documents/HellForge/hellforge-install.json"
  if [ -f "$f" ]; then dirname "$f"; return 0; fi
  f="$(find "$PFX/pfx/drive_c" -maxdepth 6 -name hellforge-install.json 2>/dev/null | head -n 1 || true)"
  [ -n "$f" ] && dirname "$f"
}

hellforge_running() {  # Wine shows Windows paths (with backslashes) in process command lines
  pgrep -u "$(id -u)" -f '\\HellForge|LocalBackend' >/dev/null 2>&1
}

wait_hellforge_exit() {
  for _ in $(seq 1 15); do hellforge_running && break; sleep 1; done
  if hellforge_running; then echo "Waiting for HellForge to close (Ctrl-C to stop waiting)..."; fi
  while hellforge_running; do sleep 2; done
}

hf_dir() {  # the HellForge folder inside the prefix (empty output if it is not there yet)
  local d="$PFX/pfx/drive_c/users/steamuser/Documents/HellForge"
  if [ -d "$d" ]; then echo "$d"; else find_install 2>/dev/null || true; fi
}

hf_file_count() {
  local d; d="$(hf_dir)"
  if [ -n "$d" ] && [ -d "$d" ]; then find "$d" -type f 2>/dev/null | wc -l; else echo 0; fi
}

matching_pids() {  # processes of this user whose command line matches $1, never this script or anything that started it
  local pid skip=" " p=$$
  while [ -n "$p" ] && [ "$p" -gt 1 ] 2>/dev/null; do  # this script and all its parent shells/terminals
    skip="$skip$p "
    p="$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')"
  done
  for pid in $(pgrep -u "$(id -u)" -f "$1" 2>/dev/null || true); do
    case "$skip" in *" $pid "*) continue ;; esac
    echo "$pid"
  done
}

kill_matching() {  # $1 = pattern, $2 = signal
  local pid
  for pid in $(matching_pids "$1"); do kill "-$2" "$pid" 2>/dev/null || true; done
}

stop_setup() {  # close HellForge-Setup and HellForge so nothing can be clicked or changed any more
  local name i
  name="$(basename "$SETUP")"
  kill_matching "$name" TERM
  kill_matching '\\HellForge|LocalBackend' TERM
  for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -z "$(matching_pids "$name")" ] && ! hellforge_running; then return 0; fi
    sleep 1
  done
  kill_matching "$name" KILL
  kill_matching '\\HellForge|LocalBackend' KILL
  sleep 1
}

run_setup() {  # runs HellForge-Setup.exe in the prefix; when all HF_FILES files are installed it closes it by itself
  local pr_pid n prev=-1 last=-1 stable=0 auto_off=0
  SETUP_AUTO=0
  env STEAM_COMPAT_DATA_PATH="$PFX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" "$PROTON" run "$SETUP" &
  pr_pid=$!
  while kill -0 "$pr_pid" 2>/dev/null || hellforge_running; do
    n="$(hf_file_count)"
    if [ "$n" != "$prev" ]; then draw_bar "$n" "" "$HF_FILES"; prev="$n"; fi
    # more files than expected: this build is not the one the number was made for, so never close anything by itself
    if [ "$n" -gt "$HF_FILES" ] && [ "$auto_off" = 0 ]; then
      auto_off=1
      if [ -t 1 ]; then echo; fi
      warn "Found $n files, more than the expected $HF_FILES: this HellForge build differs, so automatic closing is switched off."
      echo "Finish the setup yourself, close HellForge completely, then press Enter when asked."
    fi
    # done only when exactly the expected files are there, nothing new is being written, and HellForge's install record exists
    if [ "$auto_off" = 0 ] && [ "$n" -eq "$HF_FILES" ] && [ -n "$(find_install 2>/dev/null || true)" ]; then
      if [ "$n" = "$last" ]; then stable=$((stable + 1)); else stable=0; fi
      if [ "$stable" -ge 3 ]; then SETUP_AUTO=1; break; fi
    else
      stable=0
    fi
    last="$n"
    sleep 1
  done
  if [ -t 1 ]; then echo; fi
  if [ "$SETUP_AUTO" = 1 ]; then
    echo "All $HF_FILES HellForge files are installed; closing HellForge-Setup so nothing can be changed by accident..."
    stop_setup
    wait "$pr_pid" 2>/dev/null || true
  else
    wait "$pr_pid" || warn "proton returned an error (often harmless; continuing)"
  fi
}

ensure_userns() {  # Ubuntu 24.04+: only allowed programs may make HellForge's network namespace
  local inst="$1"
  [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" = 1 ] || return 0
  [ ! -x "$NS_COPY" ] || return 0
  warn "This system (Ubuntu 24.04+ AppArmor) only lets allowed programs make the network namespace HellForge runs in."
  echo "HellForge's allow-namespace.sh installs its own copy of unshare plus an AppArmor profile (needs sudo; uninstall removes it)."
  if ask_yn "Run it now?"; then
    sudo "$inst/app/linux/allow-namespace.sh" || die "allow-namespace.sh failed"
  else
    die "needed before HellForge can start: sudo $inst/app/linux/allow-namespace.sh"
  fi
}

leftover_paths() {  # what an earlier download_depot left in Steam's folder: app_553850 and the state_553850_*.patch files
  local c
  {
    for c in "${SRC_CANDIDATES[@]}"; do
      if [ -e "$c" ]; then echo "$c"; fi
    done
    patch_paths
  } | awk '!seen[$0]++'
}

patch_paths() {  # Steam's state_553850_*.patch files: next to app_553850, inside it, or elsewhere under steamapps
  local c base f r
  {
    for c in "${SRC_CANDIDATES[@]}"; do
      base="$(dirname "$c")"
      for f in "$base"/state_${APP}_*.patch "$c"/state_${APP}_*.patch; do
        if [ -e "$f" ]; then echo "$f"; fi
      done
    done
    for r in "$STEAM_ROOT/steamrt64/steamapps" "$STEAM_ROOT/ubuntu12_32/steamapps" "$STEAM_ROOT/steamapps"; do
      [ -d "$r" ] || continue
      find "$r" -maxdepth 4 -name "state_${APP}_*.patch" -not -path '*/common/*' 2>/dev/null || true
    done
  } | awk '!seen[$0]++'
}

remove_leftovers() {  # delete the given paths and make sure they are really gone
  local p
  for p in "$@"; do
    safe_rm "$p" || true
    if [ -e "$p" ]; then warn "could not delete $p; delete it yourself: rm -rf \"$p\""; fi
  done
}

clean_leftovers() {  # before a depot download: offer to delete leftovers of an earlier one
  local left=()
  mapfile -t left < <(leftover_paths)
  [ "${#left[@]}" -gt 0 ] || return 0
  warn "Leftovers from an earlier depot download were found in Steam's folder:"
  printf '  %s\n' "${left[@]}"
  echo "These need to be deleted if you want the depot download to work right."
  if ask_yn "Delete them now?"; then
    remove_leftovers "${left[@]}"
    echo "Done."
  else
    warn "Keeping them; the depot download may not work right."
  fi
}

find_setup() {  # newest HellForge-Setup*.exe in the current folder or the Downloads folder
  local dl; dl="$(xdg-user-dir DOWNLOAD 2>/dev/null || true)"; [ -n "$dl" ] || dl="$HOME/Downloads"
  ls -t ./HellForge-Setup*.exe "$dl"/HellForge-Setup*.exe "$HOME"/Downloads/HellForge-Setup*.exe 2>/dev/null | head -n 1 || true
}

make_archive() {  # $1 = folder, $2 = name; writes $BACKUP_DIR/<name>-<date>.tar[.zst] and sets ARCHIVE
  local src="$1" label="$2" size avail ext part
  mkdir -p "$BACKUP_DIR"
  size="$(du -sb "$src" 2>/dev/null | cut -f1)"
  avail="$(df -P -B1 "$BACKUP_DIR" 2>/dev/null | awk 'NR==2{print $4}')"
  if [ -n "$size" ] && [ -n "$avail" ] && [ "$size" -gt "$avail" ]; then
    warn "free space in $BACKUP_DIR ($((avail / 1048576)) MiB) is less than the folder ($((size / 1048576)) MiB); compression may or may not save you"
  fi
  if command -v zstd >/dev/null; then ext=tar.zst; else ext=tar; fi
  ARCHIVE="$BACKUP_DIR/$label-$(date +%Y%m%d-%H%M%S).$ext"
  part="$ARCHIVE.part"
  echo "Writing $ARCHIVE ..."
  if [ "$ext" = tar.zst ]; then
    tar -C "$(dirname "$src")" -cf - "$(basename "$src")" | zstd -T0 -q -o "$part" || { rm -f "$part"; die "backup failed"; }
  else
    tar -C "$(dirname "$src")" -cf "$part" "$(basename "$src")" || { rm -f "$part"; die "backup failed"; }
  fi
  mv "$part" "$ARCHIVE"
  echo "Backup saved: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
}

list_backups() {
  local f found=0
  echo "Backups in $BACKUP_DIR:"
  for f in $(ls -1t "$BACKUP_DIR"/hellforge-*.tar* 2>/dev/null || true); do
    printf '  %s  (%s)\n' "$f" "$(du -h "$f" | cut -f1)"; found=1
  done
  [ "$found" = 1 ] || echo "  (none)"
}

launch() {  # chmod, Ubuntu permission, then hellforge-linux.sh in place of this script
  local linux_dir="$INSTALL_DIR/app/linux"
  [ -f "$linux_dir/hellforge-linux.sh" ] || die "$linux_dir/hellforge-linux.sh not found; this HellForge build has no Linux scripts"
  chmod +x "$linux_dir"/*.sh
  ensure_userns "$INSTALL_DIR"
  steam_running || warn "Steam does not seem to be running; start it and sign in first (HellForge needs it)"
  echo "Starting $linux_dir/hellforge-linux.sh"
  exec env STEAM_COMPAT_DATA_PATH="$PFX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" PROTON="$PROTON" \
    "$linux_dir/hellforge-linux.sh"
}

# ---- install --------------------------------------------------------------------------------------------------------
cmd_install() {
  load_settings
  resolve_steam_root interactive
  [ -n "$STEAM_ROOT" ] && [ -d "$STEAM_ROOT" ] || die "Steam folder not found; set STEAM_ROOT (the folder that holds steamapps/)"
  set_paths
  check_deps interactive
  find_proton
  if [ -z "$PFX_OWNED" ]; then  # a prefix this script makes (or the default one) is ours to delete on uninstall
    if [ "$PFX" = "$DEFAULT_PFX" ] || [ ! -e "$PFX" ]; then PFX_OWNED=1; else PFX_OWNED=0; fi
  fi
  save_settings
  echo "Steam:   $STEAM_ROOT ($(steam_kind))"
  echo "Proton:  $PROTON"
  echo "Prefix:  $PFX"
  if [ "$(steam_kind)" != native ]; then
    warn "Steam as Flatpak/Snap is untested with HellForge. Proton runs outside Steam's sandbox here; if the Steam bridge finds no ports, tell me what it printed."
  fi

  # ---- 1. depots
  local d id nfiles
  if depots_in "$CONTENT_DST"; then
    say "1/4 Depots are already in the prefix; skipping the download"
  elif src_complete; then
    say "1/4 Depots are already downloaded in Steam ($CONTENT_SRC, $(count_files) files); skipping the download"
  else
    say "1/4 Download the HELLDIVERS 2 depots"
    nfiles="$(count_files)"
    local resumed=0
    if [ "$nfiles" -gt 0 ]; then  # something is there, but not everything
      echo "$nfiles of $EXPECTED_FILES files found; checking whether Steam is still downloading..."
      if download_active; then  # e.g. Ctrl-C by accident: Steam does not stop, so just pick the progress up again
        echo "New files are still arriving, so the download is still running. Carrying on."
        resumed=1
      else
        warn "Incomplete download: $(count_files) of $EXPECTED_FILES files found in $(src_where) (nothing new is arriving)"
        local pf=() patched=0
        mapfile -t pf < <(patch_paths)
        if [ "${#pf[@]}" -eq 0 ]; then
          echo "(No state_${APP}_*.patch files found under Steam's folder, so deleting only the .patch files is not an option.)"
        else  # first try the gentle fix: only Steam's .patch state files
          echo "Steam keeps .patch state files for this download. Deleting only those (and keeping the files already"
          echo "downloaded) should let the download carry on from here instead of fetching the large files again:"
          printf '  %s\n' "${pf[@]}"
          if ask_yes "Delete just the .patch files and run the download from there?"; then
            remove_leftovers "${pf[@]}"
            patched=1
          fi
        fi
        if [ "$patched" = 1 ]; then
          echo "Kept the downloaded files; running the download from there."
        else
          echo "It needs to be deleted and downloaded again."
          if ask_yn "Delete them and download again?"; then
            local old=()
            mapfile -t old < <(leftover_paths)
            remove_leftovers "${old[@]}"
          else
            die "incomplete download kept; run install again and answer y to delete it and download again"
          fi
        fi
      fi
    else
      clean_leftovers
    fi
    if [ "$resumed" = 0 ]; then
      echo "Steam's console opens next. Paste these three lines into it (one at a time is fine):"
      echo
      for d in "${DEPOTS[@]}"; do printf '  '; bold "download_depot $APP $d"; done
      echo
      echo "Your account must own HELLDIVERS 2. Wait for all three to finish."
      echo "(If Steam was already running and no console appears, run this in a terminal:"
      printf '  '; bold "xdg-open steam://open/console"
      echo ")"
      pause "Press Enter to start Steam with its console"
      start_steam_console
    fi
    echo "Watching $(src_where_all). It continues by itself once all $EXPECTED_FILES files are there (Enter only skips the short final wait)."
    watch_download
    locate_src || die "the three depot folders are not all in $(src_where)"
    nfiles="$(count_files)"
    [ "$nfiles" -ge "$EXPECTED_FILES" ] || die "incomplete download: $nfiles of $EXPECTED_FILES files; run install again to delete it and download again"
  fi

  # ---- 2. HellForge-Setup in its own prefix
  INSTALL_DIR="$(find_install || true)"
  if [ -n "$INSTALL_DIR" ]; then
    say "2/4 HellForge is already installed in the prefix ($INSTALL_DIR); skipping setup"
  else
    say "2/4 Run HellForge-Setup.exe"
    if [ -z "${SETUP:-}" ]; then SETUP="$(find_setup)"; fi
    [ -f "$SETUP" ] || die "HellForge-Setup.exe not found; give its path as an argument"
    mkdir -p "$PFX"
    echo "Setup: $SETUP"
    echo "In the window: click Install, let it finish and open HellForge, press Start (it is expected to fail),"
    echo "then close HellForge completely."
    echo "This script counts the files HellForge installs ($HF_FILES in total); once they are all there it closes"
    echo "HellForge-Setup by itself and carries on. If there are more files than that, or you close everything yourself first,"
    echo "it does not close anything and asks you to press Enter instead."
    run_setup
    if [ "$SETUP_AUTO" = 1 ]; then echo "Setup was closed automatically; continuing."; else pause "Press Enter once HellForge is installed and closed"; fi
    wait_hellforge_exit
    INSTALL_DIR="$(find_install || true)"
    [ -n "$INSTALL_DIR" ] || die "no HellForge installation (hellforge-install.json) found under $PFX/pfx/drive_c"
  fi
  echo "Installation: $INSTALL_DIR"

  local docs="$PFX/pfx/drive_c/users/steamuser/Documents"
  if [ -L "$docs" ]; then warn "$docs is a symlink; HellForge refuses linked folders and will likely fail. Fix that first."; fi

  # ---- 3. depots into the prefix's Steam folder
  say "3/4 Put the depots where setup looks for them"
  mkdir -p "$PFX_STEAM/steamapps/content"
  if depots_in "$CONTENT_DST"; then
    echo "Already in place: $CONTENT_DST"
  else
    locate_src || die "the three depot folders are not all in $(src_where)"
    [ "$(count_files)" -ge "$EXPECTED_FILES" ] || die "incomplete download: $(count_files) of $EXPECTED_FILES files in $CONTENT_SRC; run install again to delete it and download again"
    mkdir -p "$CONTENT_DST"
    if [ "$(stat -c %d "$CONTENT_SRC")" != "$(stat -c %d "$PFX_STEAM")" ]; then
      warn "Steam and the prefix are on different filesystems: this move is a full copy and needs disk space."
    fi
    for d in "${DEPOTS[@]}"; do
      id="${d%% *}"
      if [ -d "$CONTENT_DST/depot_$id" ]; then warn "depot_$id already in the prefix; leaving it"; continue; fi
      echo "Moving depot_$id ..."
      mv "$CONTENT_SRC/depot_$id" "$CONTENT_DST/depot_$id"
    done
    rmdir "$CONTENT_SRC" 2>/dev/null || true
  fi
  depots_in "$CONTENT_DST" || die "depots are not all in $CONTENT_DST"

  # The depots are in the prefix now. What download_depot left in Steam's folder (app_553850, state_*.patch) must go,
  # or Steam thinks the depots are still downloaded and a later download_depot would not fetch anything.
  local lft=()
  mapfile -t lft < <(leftover_paths)
  if [ "${#lft[@]}" -gt 0 ]; then
    echo "Removing leftover download files from Steam's folder (a later depot download needs them gone):"
    printf '  %s\n' "${lft[@]}"
    remove_leftovers "${lft[@]}"
  fi

  # HellForge reads the Steam folder from the prefix's registry; it must be the C: one (no link in the path).
  echo "Pointing the prefix's SteamPath at C:\\Program Files (x86)\\Steam ..."
  pr reg add 'HKCU\Software\Valve\Steam' /v SteamPath /t REG_SZ /d 'C:\Program Files (x86)\Steam' /f >/dev/null 2>&1 \
    || warn "could not set SteamPath (continuing)"
  pr cmd /c 'reg query HKCU\Software\Valve\Steam /v SteamPath > C:\hf-steampath.txt' >/dev/null 2>&1 || true
  if [ -f "$PFX/pfx/drive_c/hf-steampath.txt" ]; then
    tr -d '\r' < "$PFX/pfx/drive_c/hf-steampath.txt" | sed -n 's/^ *SteamPath */SteamPath now: /p'
    rm -f "$PFX/pfx/drive_c/hf-steampath.txt"
  fi

  # ---- 4. scripts and launch
  say "4/4 Start HellForge"
  if [ ! -e "$DESKTOP_FILE" ] || [ ! -e "$BIN_COPY" ]; then cmd_shortcut; fi
  echo "Next time: hellforge-linux run, or the 'HellForge (Linux)' app-menu entry ('where' shows where everything is)"
  launch
}

# ---- run ------------------------------------------------------------------------------------------------------------
cmd_run() {
  load_settings
  resolve_steam_root need
  set_paths
  check_deps
  find_proton
  INSTALL_DIR="$(find_install 2>/dev/null || true)"
  [ -n "$INSTALL_DIR" ] || die "HellForge is not installed in $PFX; run: $(basename "$0") install"
  launch
}

# ---- proton ---------------------------------------------------------------------------------------------------------
cmd_proton() {  # choose a Proton again and remember it
  load_settings
  resolve_steam_root need
  PROTON=""
  find_proton
  echo "Proton: $PROTON"
}

# ---- sdk ------------------------------------------------------------------------------------------------------------
sdk_dir() {  # the mod SDK folder (headers such as hellforge_mod.h); empty output if there is none
  local h d
  h="$(hf_dir)"
  [ -n "$h" ] || return 0
  d="$h/app/sdk"
  if [ -f "$d/hellforge_mod.h" ]; then echo "$d"; return 0; fi
  h="$(find "$h" -maxdepth 4 -name hellforge_mod.h 2>/dev/null | head -n 1 || true)"
  [ -z "$h" ] || dirname "$h"
}

cmd_sdk() {  # $1 = optional mod folder (the one that holds src/)
  load_settings
  local sdk mod dst f n=0
  sdk="$(sdk_dir)"
  [ -n "$sdk" ] || die "mod SDK (hellforge_mod.h) not found in $PFX; is HellForge installed? (run: $(basename "$0") where)"
  echo "SDK folder: $sdk"
  mod="${1:-}"
  if [ -z "$mod" ]; then
    echo
    echo "Give a mod folder to copy the SDK headers into its src/, e.g.:"
    echo "  $(basename "$0") sdk \"/path/to/my-mod\""
    echo "or point your compiler at it:  -I\"$sdk\""
    return 0
  fi
  [ -d "$mod" ] || die "$mod is not a folder"
  if [ -d "$mod/src" ]; then dst="$mod/src"; else dst="$mod"; warn "no src/ in $mod; copying into $mod itself"; fi
  for f in "$sdk"/*.h "$sdk"/*.hpp; do
    [ -f "$f" ] || continue
    if [ -f "$dst/$(basename "$f")" ] && cmp -s "$f" "$dst/$(basename "$f")"; then continue; fi
    cp -f -- "$f" "$dst/"; echo "copied: $(basename "$f") -> $dst/"; n=$((n+1))
  done
  if [ "$n" -eq 0 ]; then echo "Nothing to copy: the headers in $dst are already up to date."; else echo "Done: $n file(s). Build the mod again."; fi
}

# ---- build ----------------------------------------------------------------------------------------------------------
# Builds a HellForge mod with a native Linux Zig instead of HellForge's bundled Windows zig.exe (which hits a Wine
# NtLockFile bug: manifest_create Unexpected on every build). Reads the mod's CMakeLists.txt for the language and the
# source list (hellforge_add_mod(...)); without one, uses src/*.cpp or src/*.c and the "dll" name in hellforge-mod.json.
# Compiles to a Windows DLL and installs it, with hellforge-mod.json and data/, into <HellForge>/mods/<mod-folder-name>/.
cmd_build() {  # $1 = the mod folder (the one with hellforge-mod.json)
  load_settings
  local proj="${1:-}" hf sdk zig is_cxx=0 name="" dll dllname lang_line call inner f s folder target
  local sources=() toks=() cpp=() cfiles=() src_args=()
  [ -n "$proj" ] || die "usage: $(basename "$0") build <mod-folder>"
  [ -d "$proj" ] || die "$proj is not a folder"
  proj="$(realpath "$proj")"
  [ -f "$proj/hellforge-mod.json" ] || die "no hellforge-mod.json in $proj"

  hf="$(hf_dir)"
  [ -n "$hf" ] || die "HellForge is not installed in $PFX; run: $(basename "$0") install"
  sdk="$(sdk_dir)"
  [ -n "$sdk" ] || die "mod SDK (hellforge_mod.h) not found under $hf"

  zig="${ZIG:-$HOME/.local/share/zig-native/zig}"
  [ -x "$zig" ] || zig="$(command -v zig 2>/dev/null || true)"
  [ -n "$zig" ] && [ -x "$zig" ] || die "native Linux zig not found; install it to ~/.local/share/zig-native/zig, have 'zig' in PATH, or set ZIG"

  if [ -f "$proj/CMakeLists.txt" ]; then
    lang_line="$(grep -m1 '^project(' "$proj/CMakeLists.txt" || true)"
    case "$lang_line" in *CXX*) is_cxx=1 ;; esac
    # the first hellforge_add_mod(...) call, which may span several lines
    call="$(awk '/hellforge_add_mod\(/{f=1} f{printf "%s ", $0} f&&/\)/{exit}' "$proj/CMakeLists.txt")"
    [ -n "$call" ] || die "couldn't find hellforge_add_mod(...) in CMakeLists.txt"
    inner="${call#*hellforge_add_mod(}"
    inner="${inner%%)*}"
    read -ra toks <<< "$inner" || true
    name="${toks[0]:-}"
    sources=("${toks[@]:1}")
    [ -n "$name" ] && [ "${#sources[@]}" -gt 0 ] || die "couldn't read the mod name and sources from hellforge_add_mod(...)"
  else
    # no CMakeLists.txt: everything comes from hellforge-mod.json and src/*.cpp (or src/*.c)
    dllname="$(grep -Po '"dll"\s*:\s*"\K[^"]+' "$proj/hellforge-mod.json" || true)"
    [ -n "$dllname" ] || die "no \"dll\" entry in $proj/hellforge-mod.json"
    name="${dllname%.dll}"
    mapfile -t cpp < <(find "$proj/src" -maxdepth 1 -name '*.cpp' 2>/dev/null | sort)
    if [ "${#cpp[@]}" -gt 0 ]; then
      is_cxx=1
      for f in "${cpp[@]}"; do sources+=("${f#"$proj"/}"); done
    else
      mapfile -t cfiles < <(find "$proj/src" -maxdepth 1 -name '*.c' 2>/dev/null | sort)
      for f in "${cfiles[@]}"; do [ -z "$f" ] || sources+=("${f#"$proj"/}"); done
    fi
    [ "${#sources[@]}" -gt 0 ] || die "no CMakeLists.txt and no src/*.c or src/*.cpp found"
  fi

  echo "mod: $name  (c++: $is_cxx)"
  echo "sources: ${sources[*]}"
  echo "sdk: $sdk"

  dll="$proj/$name.dll"
  for s in "${sources[@]}"; do src_args+=("$proj/$s"); done

  if [ "$is_cxx" = 1 ]; then
    "$zig" c++ -target x86_64-windows-gnu -shared -std=c++17 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" -o "$dll" "${src_args[@]}" || die "build failed"
  else
    "$zig" cc -target x86_64-windows-gnu -shared -std=c11 -Wall -Werror -Wno-unused-parameter -s \
      -I "$sdk" -o "$dll" "${src_args[@]}" || die "build failed"
  fi

  ! command -v file >/dev/null || file "$dll"

  folder="$(basename "$proj")"
  target="$hf/mods/$folder"
  mkdir -p "$target"
  cp -f -- "$proj/hellforge-mod.json" "$dll" "$target/"
  if [ -d "$proj/data" ]; then
    mkdir -p "$target/data"
    cp -r -- "$proj/data/." "$target/data/"
  fi
  echo "installed to $target"
}

# ---- where ----------------------------------------------------------------------------------------------------------
cmd_where() {
  load_settings
  resolve_steam_root quiet
  set_paths
  local inst state
  inst="$(find_install 2>/dev/null || true)"
  if [ -d "$PFX" ]; then state="exists"; else state="not created yet"; fi
  echo "Prefix folder:      $PFX  ($state)"
  echo "Wine prefix:        $PFX/pfx"
  echo "Windows C: drive:   $PFX/pfx/drive_c"
  echo "HellForge install:  ${inst:-not installed yet}"
  [ -z "$inst" ] || echo "Linux scripts:      $inst/app/linux/"
  [ -z "$inst" ] || echo "Mod SDK headers:    $(sdk_dir 2>/dev/null || true)"
  echo "Depots in prefix:   $CONTENT_DST  ($(depots_in "$CONTENT_DST" && echo present || echo none))"
  if [ -n "$STEAM_ROOT" ]; then echo "Steam:              $STEAM_ROOT  ($(steam_kind))"; else echo "Steam:              not found"; fi
  echo "Proton:             ${PROTON:-not chosen yet}"
  echo "Settings file:      $CFG  ($([ -f "$CFG" ] && echo saved || echo none))"
  echo "Backups:            $BACKUP_DIR"
  [ ! -e "$BIN_COPY" ] || echo "Command:            $BIN_COPY"
  [ ! -e "$DESKTOP_FILE" ] || echo "Menu entry:         $DESKTOP_FILE"
  [ ! -d "$PFX" ] || { echo; echo "Open it:  xdg-open \"$PFX\""; }
}

# ---- default (no command) -------------------------------------------------------------------------------------------
prefix_present() { [ -d "$PFX/pfx" ] || [ -f "$PFX/version" ]; }

prefix_backups() {  # backups of the whole prefix, newest first
  ls -1t "$BACKUP_DIR"/hellforge-prefix-*.tar "$BACKUP_DIR"/hellforge-prefix-*.tar.zst 2>/dev/null || true
}

cmd_default() {
  load_settings
  if prefix_present; then  # a prefix exists: just show the commands
    usage
    if [ -z "$(find_install 2>/dev/null || true)" ]; then
      echo; echo "Note: HellForge is not installed in the prefix $PFX yet. Run: $(basename "$0") install"
    fi
    return 0
  fi

  local bk=() setup
  mapfile -t bk < <(prefix_backups)
  if [ "${#bk[@]}" -gt 0 ]; then  # no prefix, but a backup of one
    echo "No prefix found at $PFX."
    echo "But I found ${#bk[@]} backup(s) of it in $BACKUP_DIR (newest: ${bk[0]})."
    if ask_yn "Restore it?"; then cmd_restore "" noconfirm; return 0; fi
    echo; usage
    echo; echo "(For a fresh install instead: $(basename "$0") install)"
    return 0
  fi

  echo "No prefix found at $PFX."
  setup="$(find_setup)"
  if [ -z "$setup" ]; then
    echo "To install: download HellForge-Setup.exe into your Downloads folder, then run this again."
    echo; usage; return 0
  fi
  echo "Found the installer: $setup"
  if ask_yn "Install HellForge now?"; then
    [ -f "${SETUP:-}" ] || SETUP="$setup"
    cmd_install; return 0
  fi
  echo; usage
}

# ---- restore --------------------------------------------------------------------------------------------------------
cmd_restore() {  # $1 = backup file (else choose one), $2 = noconfirm
  load_settings
  local a="${1:-}" bk=() top
  if [ -z "$a" ]; then
    mapfile -t bk < <(prefix_backups)
    case ${#bk[@]} in
      0) die "no prefix backups in $BACKUP_DIR; make one with: $(basename "$0") backup" ;;
      1) a="${bk[0]}" ;;
      *) echo "Which backup? (newest first)"; select a in "${bk[@]}"; do [ -n "${a:-}" ] && break; done ;;
    esac
  fi
  [ -f "$a" ] || die "backup not found: $a"
  case "$a" in
    *.tar.zst) command -v zstd >/dev/null || die "zstd is needed to read $a; install it first" ;;
    *.tar) ;;
    *) die "not a prefix backup (.tar or .tar.zst): $a" ;;
  esac
  hellforge_running && die "HellForge is running; close it first"
  if [ -e "$PFX" ] && [ -n "$(ls -A "$PFX" 2>/dev/null)" ]; then
    die "$PFX already exists; move or delete it first (restore never overwrites)"
  fi
  say "Restore the prefix"
  echo "Backup: $a  ($(du -h "$a" | cut -f1))"
  echo "Into:   $PFX"
  if [ "${2:-}" != noconfirm ]; then ask_yn "Restore it now?" || { echo "Cancelled."; return 0; }; fi
  mkdir -p "$PFX"
  echo "Extracting (this can take a while)..."
  tar -xf "$a" -C "$PFX" --strip-components=1 || { warn "extracting failed; $PFX may be incomplete"; die "restore failed"; }
  if [ -z "$PFX_OWNED" ]; then PFX_OWNED=1; fi  # we made this folder, so uninstall may remove it
  resolve_steam_root quiet
  save_settings
  INSTALL_DIR="$(find_install 2>/dev/null || true)"
  if [ -n "$INSTALL_DIR" ]; then
    echo "Restored. HellForge: $INSTALL_DIR"
    echo "Start it with: $(basename "$0") run"
  else
    warn "restored, but no HellForge installation (hellforge-install.json) was found in it; this may be a backup of an unfinished install"
  fi
}

# ---- backup ---------------------------------------------------------------------------------------------------------
cmd_backup() {
  load_settings
  [ -z "${1:-}" ] || BACKUP_DIR="$1"
  [ -d "$PFX" ] || die "no prefix at $PFX; nothing to back up"
  hellforge_running && die "HellForge is running; close it first"
  say "Back up the prefix"
  echo "Prefix:        $PFX  ($(du -sh "$PFX" 2>/dev/null | cut -f1))"
  echo "Backup folder: $BACKUP_DIR"
  echo "The prefix holds HellForge, its profiles and mods, the game copy and the depots, so this can be large."
  ask_yn "Back it up now?" || { echo "Cancelled."; return 0; }
  make_archive "$PFX" hellforge-prefix
  echo; list_backups
  echo; echo "To restore: close HellForge, move the current prefix away, then: $(basename "$0") restore \"$ARCHIVE\""
}

# ---- update ---------------------------------------------------------------------------------------------------------
cmd_update() {
  load_settings
  resolve_steam_root need
  set_paths
  find_proton
  hellforge_running && die "HellForge is running; close it first"
  INSTALL_DIR="$(find_install 2>/dev/null || true)"
  [ -n "$INSTALL_DIR" ] || die "HellForge is not installed in $PFX; run: $(basename "$0") install"
  SETUP="${1:-${SETUP:-}}"
  [ -n "$SETUP" ] || SETUP="$(find_setup)"
  [ -f "$SETUP" ] || die "no HellForge-Setup.exe found. Download the newest one into your Downloads folder, or give its path: $(basename "$0") update /path/HellForge-Setup.exe"

  say "Update HellForge"
  echo "HellForge folder: $INSTALL_DIR  ($(du -sh "$INSTALL_DIR" 2>/dev/null | cut -f1))"
  echo "Newest setup:     $SETUP  (modified $(date -r "$SETUP" '+%Y-%m-%d %H:%M'))"
  echo "Backup folder:    $BACKUP_DIR"
  echo
  echo "1. The HellForge folder above is backed up first."
  echo "2. Then $(basename "$SETUP") runs inside the prefix: $PFX"
  echo "   (updating means running the newest setup against the same prefix)."
  ask_yn "Back up and continue?" || { echo "Cancelled."; return 0; }

  make_archive "$INSTALL_DIR" hellforge-folder
  echo
  warn "About to run $(basename "$SETUP") in the prefix $PFX"
  echo "Follow its window; when it has finished and HellForge opens, close HellForge completely."
  ask_yn "Run the setup now?" || { echo "Cancelled. Your backup is kept: $ARCHIVE"; return 0; }
  pr "$SETUP" || warn "proton returned an error (often harmless; continuing)"
  pause "Press Enter once the update is done and HellForge is closed"
  wait_hellforge_exit
  INSTALL_DIR="$(find_install 2>/dev/null || true)"
  if [ -z "$INSTALL_DIR" ]; then
    die "the HellForge installation is missing now; your backup is $ARCHIVE"
  fi
  echo "Updated: $INSTALL_DIR"
  echo "Start it with: $(basename "$0") run   (backup kept: $ARCHIVE)"
}

# ---- shortcut -------------------------------------------------------------------------------------------------------
cmd_shortcut() {
  mkdir -p "$(dirname "$BIN_COPY")" "$(dirname "$DESKTOP_FILE")"
  if [ "$SELF" != "$(readlink -f "$BIN_COPY" 2>/dev/null || true)" ]; then install -m 755 "$SELF" "$BIN_COPY"; fi
  cat > "$DESKTOP_FILE" <<DESK
[Desktop Entry]
Type=Application
Name=HellForge (Linux)
Comment=Offline HELLDIVERS 2 with HellForge
Exec="$BIN_COPY" run
Terminal=true
Categories=Game;
DESK
  command -v update-desktop-database >/dev/null && update-desktop-database "$(dirname "$DESKTOP_FILE")" 2>/dev/null || true
  echo "Added: $BIN_COPY  and the app-menu entry 'HellForge (Linux)'."
  case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "(~/.local/bin is not in your PATH; use the app-menu entry or $BIN_COPY run)" ;; esac
}

# ---- uninstall ------------------------------------------------------------------------------------------------------
safe_rm() {  # refuses anything that is not clearly one of ours
  local p="$1"
  [ -n "$p" ] && [ -e "$p" ] || return 0
  case "$p" in /|"$HOME"|"$HOME"/|"${STEAM_ROOT:-/nonexistent}"|"${STEAM_ROOT:-/nonexistent}"/) die "refusing to delete $p" ;; esac
  [ "${#p}" -ge 12 ] || die "refusing to delete short path $p"
  rm -rf --one-file-system -- "$p"
}

cmd_uninstall() {
  load_settings
  resolve_steam_root quiet
  set_paths
  hellforge_running && die "HellForge is running; close it (and anything else in the prefix) first"
  local inst has_ns=0 lo=() p c
  inst="$(find_install 2>/dev/null || true)"
  if [ -e "$NS_COPY" ] || [ -e "$NS_PROFILE" ]; then has_ns=1; fi
  if [ -n "$STEAM_ROOT" ]; then
    mapfile -t lo < <(leftover_paths)
  fi

  say "Uninstall HellForge"
  echo "This will remove:"
  if [ "$PFX_OWNED" = 1 ] && [ -d "$PFX" ]; then
    echo "  - the whole prefix: $PFX"
    echo "    (HellForge, its profiles and mods, the game copy and the depots inside it; measuring size...)"
    echo "    size: $(du -sh "$PFX" 2>/dev/null | cut -f1)"
  else
    [ -z "$inst" ] || echo "  - the HellForge installation: $inst (its profiles and mods go with it)"
    [ ! -d "$CONTENT_DST" ] || echo "  - the depots in the prefix: $CONTENT_DST"
    echo "  (the prefix itself is not ours and stays: $PFX)"
  fi
  if [ "${#lo[@]}" -gt 0 ]; then
    echo "  - leftover depot-download files in Steam's folder:"
    printf '      %s\n' "${lo[@]}"
  fi
  [ ! -f "$CFG" ] || echo "  - the saved settings: $CFG"
  [ ! -e "$BIN_COPY" ] || echo "  - the command: $BIN_COPY"
  [ ! -e "$DESKTOP_FILE" ] || echo "  - the menu entry: $DESKTOP_FILE"
  echo "It does not touch Steam, HELLDIVERS 2, your Proton versions, or anything outside the paths above."
  echo "Your backups in $BACKUP_DIR are kept unless you say so in the next questions."
  echo "Back up any HellForge profiles or mods you want to keep first."
  c=""; read -r -p "Type 'yes' to continue: " c || true
  [ "$c" = yes ] || { echo "Cancelled."; return 0; }

  # HellForge's Ubuntu permission (needs sudo); done first, while its script still exists
  if [ "$has_ns" = 1 ] && ask_yn "Also remove HellForge's network-namespace permission for AppArmor/Ubuntu (needs sudo)?"; then
    if [ -x "$inst/app/linux/allow-namespace.sh" ]; then
      sudo "$inst/app/linux/allow-namespace.sh" --remove || warn "removing the namespace permission failed"
    else
      sudo bash -c "apparmor_parser -R '$NS_PROFILE' 2>/dev/null || true; rm -f '$NS_PROFILE' '$NS_COPY'; rmdir /usr/local/lib/hellforge 2>/dev/null || true" \
        || warn "removing the namespace permission failed"
    fi
  fi

  # backups made by this script (only its own hellforge-*.tar* files are ever deleted)
  local bk=() f
  mapfile -t bk < <(ls -1t "$BACKUP_DIR"/hellforge-*.tar* 2>/dev/null || true)
  if [ "${#bk[@]}" -gt 0 ]; then
    echo; echo "Backups in $BACKUP_DIR:"
    for f in "${bk[@]}"; do printf '  %s  (%s)\n' "$f" "$(du -h "$f" | cut -f1)"; done
    if ask_yn "Delete these ${#bk[@]} backup file(s) too? (No keeps them)"; then
      for f in "${bk[@]}"; do rm -f -- "$f"; done
      rmdir "$BACKUP_DIR" 2>/dev/null || true
      echo "Backups deleted."
    else
      echo "Backups kept in $BACKUP_DIR"
    fi
  fi

  # what download_depot left in Steam's folder (app_553850 and the state_553850_*.patch files)
  if [ "${#lo[@]}" -gt 0 ]; then remove_leftovers "${lo[@]}"; fi

  if [ "$PFX_OWNED" = 1 ] && [ -d "$PFX" ]; then
    [ -d "$PFX/pfx" ] || [ -f "$PFX/version" ] || [ -z "$(ls -A "$PFX")" ] || die "$PFX does not look like a Proton prefix; not deleting it"
    echo "Deleting $PFX ..."
    safe_rm "$PFX"
  else
    if [ -n "$inst" ]; then
      [ -f "$inst/hellforge-install.json" ] || die "$inst is not a HellForge installation; not deleting it"
      echo "Deleting $inst ..."; safe_rm "$inst"
    fi
    safe_rm "$CONTENT_DST"
  fi

  rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/hf-steam-*.sock
  rm -f "$BIN_COPY" "$DESKTOP_FILE" "$CFG"
  rmdir "$CFG_DIR" 2>/dev/null || true
  echo "Done."
}

usage() {
  cat <<USAGE
Usage: $(basename "$0") <command>
  install [HellForge-Setup.exe]  guided install (safe to re-run; finished steps are skipped)
  run                            start HellForge (with the Proton used last time)
  proton                         pick a different Proton (remembered for next time)
  backup [folder]                back up the whole prefix and show where backups are
  restore [backup-file]          restore the prefix from a backup (only if there is no prefix)
  update [HellForge-Setup.exe]   back up the HellForge folder, then run the newest setup in the prefix
  where                          show where the prefix, install, Steam and Proton are
  build <mod-folder>             build a mod with native Linux Zig and install it into HellForge's mods/
  sdk [mod-folder]               show the mod SDK folder; copy its headers into <mod-folder>/src
  shortcut                       add an app-menu entry and a 'hellforge-linux' command
  uninstall                      remove what this script set up (asks first)
With no command: if there is no prefix, offers to restore a backup of it or to install; otherwise this list is shown. See the top of this file for settings (PROTON, PFX, STEAM_ROOT, SETUP).
USAGE
}

# ---- main -----------------------------------------------------------------------------------------------------------
case "${1:-}" in
  "")                 cmd_default ;;
  install)            shift || true; SETUP="${1:-${SETUP:-}}"; cmd_install ;;
  run|start)          cmd_run ;;
  proton)             cmd_proton ;;
  backup)             shift || true; cmd_backup "${1:-}" ;;
  restore)            shift || true; cmd_restore "${1:-}" ;;
  update)             shift || true; cmd_update "${1:-}" ;;
  where|prefix|info)  cmd_where ;;
  sdk)                shift || true; cmd_sdk "${1:-}" ;;
  build)              shift || true; cmd_build "${1:-}" ;;
  shortcut)           cmd_shortcut ;;
  uninstall|remove)   cmd_uninstall ;;
  help|-h|--help)     usage ;;
  *)                  if [ -f "$1" ]; then SETUP="$1"; cmd_install; else usage; exit 2; fi ;;
esac
