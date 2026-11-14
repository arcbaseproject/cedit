#!/bin/sh
# Cedit installer and updater.
#
#   curl -fsSL https://raw.githubusercontent.com/arcbaseproject/cedit/main/install.sh | sh
#
# Options:
#   --prefix DIR        install under DIR (default: ~/.local, or /usr/local as root)
#   --lua               build with Lua scripting
#   --no-auto-update    do not install the daily update timer
#   --check-update      update only if the VERSION on GitHub differs (used by the timer)
#   --uninstall         remove cedit, the updater and the timer
#   --quiet             print errors only (log goes to the update log)
#
# A new version is installed only if it builds and its unit tests pass.
# Auto-update runs `cedit-update --check-update` once a day: it downloads
# VERSION from raw.githubusercontent.com and rebuilds when it changed.
set -eu

REPO="arcbaseproject/cedit"
BRANCH="main"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"
TARBALL="https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz"

if [ "$(id -u)" -eq 0 ]; then PREFIX="/usr/local"; else PREFIX="$HOME/.local"; fi
LUA=0
AUTO_UPDATE=1
MODE=install
QUIET=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
        --prefix=*) PREFIX="${1#--prefix=}" ;;
        --lua) LUA=1 ;;
        --no-auto-update) AUTO_UPDATE=0 ;;
        --check-update) MODE=check ;;
        --uninstall) MODE=uninstall ;;
        --quiet) QUIET=1 ;;
        -h|--help) sed -n '2,17p' "$0" 2>/dev/null || true; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

case "$PREFIX" in /*) ;; *) PREFIX="$(pwd)/$PREFIX" ;; esac
BIN="$PREFIX/bin"
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/cedit"
[ "$(id -u)" -eq 0 ] && STATE="/var/lib/cedit"
LOG="$STATE/update.log"

say() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
die() { printf 'cedit install: %s\n' "$*" >&2; exit 1; }

fetch() { # fetch URL [OUTFILE]
    if command -v curl >/dev/null 2>&1; then
        if [ $# -eq 2 ]; then curl --proto '=https' --tlsv1.2 -fsSL "$1" -o "$2"
        else curl --proto '=https' --tlsv1.2 -fsSL "$1"; fi
    elif command -v wget >/dev/null 2>&1; then
        if [ $# -eq 2 ]; then wget --https-only -qO "$2" "$1"
        else wget --https-only -qO- "$1"; fi
    else
        die "need curl or wget"
    fi
}

installed_version() {
    [ -x "$BIN/cedit" ] && "$BIN/cedit" --version 2>/dev/null | awk '{print $2}' || true
}

# ---- auto-update timer ----

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SYSTEMCTL="systemctl --user"
if [ "$(id -u)" -eq 0 ]; then UNIT_DIR="/etc/systemd/system"; SYSTEMCTL="systemctl"; fi
CRON_TAG="# cedit-auto-update"

remove_timer() {
    if [ -f "$UNIT_DIR/cedit-update.timer" ]; then
        $SYSTEMCTL disable --now cedit-update.timer >/dev/null 2>&1 || true
        rm -f "$UNIT_DIR/cedit-update.timer" "$UNIT_DIR/cedit-update.service"
        $SYSTEMCTL daemon-reload >/dev/null 2>&1 || true
    fi
    if command -v crontab >/dev/null 2>&1 && crontab -l 2>/dev/null | grep -q "$CRON_TAG"; then
        crontab -l 2>/dev/null | grep -v "$CRON_TAG" | crontab -
    fi
}

install_timer() {
    cmd="$BIN/cedit-update --check-update --quiet --prefix $PREFIX"
    [ "$LUA" -eq 1 ] && cmd="$cmd --lua"
    if command -v systemctl >/dev/null 2>&1 && $SYSTEMCTL show-environment >/dev/null 2>&1; then
        mkdir -p "$UNIT_DIR"
        cat > "$UNIT_DIR/cedit-update.service" <<EOF
[Unit]
Description=Update the cedit text editor
Wants=network-online.target
After=network-online.target

[Service]
Type=oneshot
ExecStart=$cmd
EOF
        cat > "$UNIT_DIR/cedit-update.timer" <<EOF
[Unit]
Description=Daily cedit update check

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
EOF
        $SYSTEMCTL daemon-reload
        $SYSTEMCTL enable --now cedit-update.timer >/dev/null 2>&1 ||
            die "could not enable the systemd timer"
        say "Auto-update: systemd timer cedit-update.timer (daily)"
    elif command -v crontab >/dev/null 2>&1; then
        { crontab -l 2>/dev/null | grep -v "$CRON_TAG" || true
          echo "17 4 * * * $cmd $CRON_TAG"; } | crontab -
        say "Auto-update: cron job (daily, 04:17)"
    else
        say "Auto-update: no systemd or cron found; run 'cedit-update --check-update' yourself"
    fi
}

# ---- uninstall ----

if [ "$MODE" = uninstall ]; then
    remove_timer
    rm -f "$BIN/cedit" "$BIN/cedit-update" "$PREFIX/share/man/man1/cedit.1"
    rm -rf "$PREFIX/share/doc/cedit"
    say "Removed cedit from $PREFIX (your ~/.config/cedit is untouched)"
    exit 0
fi

# ---- check for a new version ----

mkdir -p "$STATE"
if [ "$MODE" = check ]; then
    exec >>"$LOG" 2>&1
    echo "== $(date '+%Y-%m-%d %H:%M:%S') update check"
    remote="$(fetch "$RAW/VERSION")" || die "cannot fetch $RAW/VERSION"
    remote="$(printf '%s' "$remote" | tr -d ' \r\n')"
    current="$(installed_version)"
    case "$remote" in ''|*[!0-9A-Za-z.+-]*) die "bad VERSION from GitHub: '$remote'" ;; esac
    if [ "$remote" = "$current" ]; then
        echo "up to date ($current)"
        exit 0
    fi
    echo "updating $current -> $remote"
fi

# ---- build and install ----

for tool in make tar; do
    command -v "$tool" >/dev/null 2>&1 || die "need $tool"
done
CC="${CC:-}"
if [ -z "$CC" ]; then
    for c in cc gcc clang; do command -v "$c" >/dev/null 2>&1 && CC="$c" && break; done
fi
[ -n "$CC" ] || die "need a C compiler (cc, gcc or clang)"

LOCK="$STATE/update.lock"
mkdir "$LOCK" 2>/dev/null || die "another install/update is running ($LOCK)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM

if [ -n "${CEDIT_SOURCE_DIR:-}" ]; then # local testing
    cp -R "$CEDIT_SOURCE_DIR" "$WORK/src"
    rm -rf "$WORK/src/build" "$WORK/src/cedit"
    SRC="$WORK/src"
else
    say "Downloading $REPO ($BRANCH)..."
    fetch "$TARBALL" "$WORK/src.tar.gz" || die "download failed"
    tar -xzf "$WORK/src.tar.gz" -C "$WORK"
    SRC="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
fi
[ -f "$SRC/Makefile" ] || die "downloaded source has no Makefile"

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
say "Building with $CC..."
if ! make -C "$SRC" -j"$JOBS" CC="$CC" LUA="$LUA" >"$WORK/build.log" 2>&1; then
    tail -n 20 "$WORK/build.log" >&2
    die "build failed; nothing was installed"
fi
say "Running tests..."
if ! make -C "$SRC" CC="$CC" LUA="$LUA" test >"$WORK/test.log" 2>&1; then
    tail -n 20 "$WORK/test.log" >&2
    die "tests failed; nothing was installed"
fi

mkdir -p "$BIN" 2>/dev/null || die "cannot create $BIN (try sudo, or --prefix)"
[ -w "$BIN" ] || die "cannot write to $BIN (try sudo, or --prefix)"
make -C "$SRC" PREFIX="$PREFIX" install >/dev/null
# This script may be running as cedit-update: replace it by rename so
# the shell keeps reading the old file instead of the new bytes.
install -m 755 "$SRC/install.sh" "$BIN/.cedit-update.new"
mv -f "$BIN/.cedit-update.new" "$BIN/cedit-update"

if [ "$MODE" = check ]; then
    echo "installed $(installed_version)"
    exit 0
fi
say "Installed cedit $(installed_version) to $BIN/cedit"

if [ "$AUTO_UPDATE" -eq 1 ]; then install_timer; else remove_timer; fi
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) say "Note: $BIN is not in your PATH. Add it, e.g.: export PATH=\"$BIN:\$PATH\"" ;;
esac
