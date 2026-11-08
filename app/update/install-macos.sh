# macOS: replace the CMRemote application bundle from the verified disk image.
# Extra arguments: IDENTIFIER ARCHITECTURE
identifier=$8; architecture=$9
mount=
detach() {
    [ -n "$mount" ] || return 0
    hdiutil detach "$mount" -quiet 2>/dev/null || hdiutil detach "$mount" -force -quiet 2>/dev/null
    rmdir "$mount" 2>/dev/null; mount=
}
value() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
check() {
    [ -d "$source" ] && [ ! -L "$source" ] && [ -f "$plist" ] || { echo "The disk image does not contain CMRemote.app."; return 1; }
    [ "$(value CFBundleIdentifier)" = "$identifier" ] || { echo "The disk image contains a different application."; return 1; }
    [ "$(value CFBundleShortVersionString)" = "$version" ] || { echo "The disk image does not contain CMRemote $version."; return 1; }
    executable="$source/Contents/MacOS/$(value CFBundleExecutable)"
    [ -f "$executable" ] || { echo "The application in the disk image has no executable."; return 1; }
    lipo -archs "$executable" 2>/dev/null | tr ' ' '\n' | grep -qx "$architecture" || { echo "The update does not run on $architecture."; return 1; }
}
# While CMRemote runs: the new bundle is copied beside the installed one.
stage_update() {
    [ -d "$target" ] || refuse "$target is not an application bundle."
    # Outside the session, which may be removed before the image finishes ejecting.
    mount=$(mktemp -d "$updates/.mount.XXXXXX") || refuse "Cannot create a mount point."
    if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mount" "$package" >/dev/null 2>&1; then
        rmdir "$mount"; mount=; refuse "Cannot open the update disk image."
    fi
    source="$mount/CMRemote.app"; plist="$source/Contents/Info.plist"
    if ! problem=$(check); then detach; refuse "$problem"; fi
    total=$(du -sk "$source" | cut -f1)
    if ! copy_with_progress "$stage" "$total" ditto "$source" "$stage" 2>/dev/null; then
        detach; refuse "Cannot copy the new version beside $target."
    fi
    # Ejecting the image takes seconds; nothing waits for it.
    detach &
}
# After CMRemote exits: only the two bundles trade places.
install_update() {
    mv "$target" "$backup" || fail "Cannot move the current version aside; it was left unchanged."
    if ! mv "$stage" "$target"; then
        mv "$backup" "$target" || fail "Replacing $target failed; the previous version is at $backup."
        fail "Cannot move the new version into place; the previous version was restored."
    fi
    stage=; rm -rf "$backup"
}
relaunch_update() { open "$target"; }
main
