# Shared by the CMRemote update helpers; the app writes it in front of each helper.
# Arguments: SESSION PID PACKAGE SHA256 VERSION TARGET UPDATES
# SESSION is a private directory the app created for this attempt. The helper
# acknowledges with SESSION/started and, while CMRemote still runs, copies the new
# version beside TARGET (SESSION/progress: "<KiB copied> <KiB total>"). It then
# reports SESSION/ready, or SESSION/error with the reason nothing will be installed,
# and waits for PID to exit. It replaces TARGET only when the app wrote SESSION/commit
# during its final close; a canceled quit or a crash leaves no commit and the helper
# removes the copy without changing anything. The platform part defines
# stage_update, install_update and relaunch_update, then calls main.
set -u
umask 077
session=$1; pid=$2; package=$3; sha=$4; version=$5; target=$6; updates=$7
result="$updates/result.json"; lock="$updates/install.lock"; locked=0; stage=

escape() { printf '%s' "$1" | tr '\n\t\r' '   ' | sed 's/\\/\\\\/g; s/"/\\"/g'; }
report() {
    tmp="$updates/.result.$$"
    printf '{"schema":1,"status":"%s","version":"%s","message":"%s"}\n' "$1" "$(escape "$version")" "$(escape "$2")" > "$tmp" &&
        mv -f "$tmp" "$result"
}
discard() { [ -n "$stage" ] && rm -rf "$stage"; stage=; }
finish() { [ "$locked" = 1 ] && rmdir "$lock" 2>/dev/null; rm -rf "$session"; }
# Before CMRemote closes: it shows the reason and stays open; nothing was changed.
refuse() { discard; printf '%s\n' "$1" > "$session/.error" && mv -f "$session/.error" "$session/error"; exit 1; }
# After the commit: the result is written last; once it exists, the helper has finished.
fail() { discard; finish; report failed "$1"; exit 1; }
progress() { printf '%s %s\n' "${1:-0}" "$2" > "$session/.progress" && mv -f "$session/.progress" "$session/progress"; }
# Runs a copy in the background, reporting the size of PATH against TOTAL KiB until it ends.
copy_with_progress() {
    path=$1; total=$2; shift 2
    "$@" & copier=$!
    while kill -0 "$copier" 2>/dev/null; do
        progress "$(du -sk "$path" 2>/dev/null | cut -f1)" "$total"; sleep 0.2
    done
    wait "$copier" || return 1
    progress "$total" "$total"
}
digest() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else /usr/bin/shasum -a 256 "$1"; fi | cut -d' ' -f1
}

main() {
    : > "$session/started" || exit 1
    [ -f "$package" ] && [ ! -L "$package" ] || refuse "The downloaded update is missing."
    [ "$(digest "$package")" = "$sha" ] || refuse "The downloaded update changed after it was verified; it was not installed."
    [ -e "$target" ] && [ ! -L "$target" ] || refuse "$target is missing or is a link; nothing was replaced."
    parent=$(dirname "$target"); name=$(basename "$target")
    stage="$parent/.$name.updating-$$"; backup="$parent/.$name.previous-$$"
    stage_update
    : > "$session/ready" || refuse "Cannot report that the update is ready."
    while kill -0 "$pid" 2>/dev/null; do sleep 0.2; done
    if [ ! -f "$session/commit" ]; then discard; finish; exit 0; fi
    relaunch=$(cat "$session/commit")
    if ! mkdir "$lock" 2>/dev/null; then discard; rm -rf "$session"; report failed "Another update is being installed."; exit 1; fi
    locked=1
    install_update
    rm -f "$package" "$updates/pending.json"
    finish
    report installed "CMRemote $version was installed."
    [ "$relaunch" = 1 ] && relaunch_update
    exit 0
}
