# Shared by the CMRemote update helpers; the app writes it in front of each helper.
# Arguments: SESSION PID PACKAGE SHA256 VERSION TARGET UPDATES
# SESSION is a private directory the app created for this attempt. The helper
# acknowledges with SESSION/ready, then waits for PID to exit. It installs only
# when the app wrote SESSION/commit during its final close; a canceled quit or a
# crash leaves no commit and the helper exits without changing anything.
set -u
umask 077
session=$1; pid=$2; package=$3; sha=$4; version=$5; target=$6; updates=$7
result="$updates/result.json"; lock="$updates/install.lock"; locked=0

escape() { printf '%s' "$1" | tr '\n\t\r' '   ' | sed 's/\\/\\\\/g; s/"/\\"/g'; }
report() {
    tmp="$updates/.result.$$"
    printf '{"schema":1,"status":"%s","version":"%s","message":"%s"}\n' "$1" "$(escape "$version")" "$(escape "$2")" > "$tmp" &&
        mv -f "$tmp" "$result"
}
finish() { [ "$locked" = 1 ] && rmdir "$lock" 2>/dev/null; rm -rf "$session"; }
# The result is written last: once it exists, the helper has finished.
fail() { finish; report failed "$1"; exit 1; }
digest() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else /usr/bin/shasum -a 256 "$1"; fi | cut -d' ' -f1
}

: > "$session/ready" || exit 1
while kill -0 "$pid" 2>/dev/null; do sleep 0.2; done
if [ ! -f "$session/commit" ]; then finish; exit 0; fi
relaunch=$(cat "$session/commit")
if ! mkdir "$lock" 2>/dev/null; then rm -rf "$session"; report failed "Another update is being installed."; exit 1; fi
locked=1
[ -f "$package" ] && [ ! -L "$package" ] || fail "The downloaded update is missing."
[ "$(digest "$package")" = "$sha" ] || fail "The downloaded update changed after it was verified; it was not installed."
[ -e "$target" ] && [ ! -L "$target" ] || fail "$target is missing or is a link; nothing was replaced."
parent=$(dirname "$target"); name=$(basename "$target")
stage="$parent/.$name.updating-$$"; backup="$parent/.$name.previous-$$"
