# macOS: replace the CMRemote application bundle from the verified disk image.
# Extra arguments: IDENTIFIER ARCHITECTURE
identifier=$8; architecture=$9
[ -d "$target" ] || fail "$target is not an application bundle."
mount=$(mktemp -d "$session/mount.XXXXXX") || fail "Cannot create a mount point."
detach() { hdiutil detach "$mount" -quiet 2>/dev/null || hdiutil detach "$mount" -force -quiet 2>/dev/null; }
hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mount" "$package" >/dev/null 2>&1 || fail "Cannot open the update disk image."
source="$mount/CMRemote.app"
plist="$source/Contents/Info.plist"
value() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
check() {
    [ -d "$source" ] && [ ! -L "$source" ] && [ -f "$plist" ] || { echo "The disk image does not contain CMRemote.app."; return 1; }
    [ "$(value CFBundleIdentifier)" = "$identifier" ] || { echo "The disk image contains a different application."; return 1; }
    [ "$(value CFBundleShortVersionString)" = "$version" ] || { echo "The disk image does not contain CMRemote $version."; return 1; }
    executable="$source/Contents/MacOS/$(value CFBundleExecutable)"
    [ -f "$executable" ] || { echo "The application in the disk image has no executable."; return 1; }
    lipo -archs "$executable" 2>/dev/null | tr ' ' '\n' | grep -qx "$architecture" || { echo "The update does not run on $architecture."; return 1; }
}
if ! problem=$(check); then detach; fail "$problem"; fi
if ! ditto "$source" "$stage" 2>/dev/null; then rm -rf "$stage"; detach; fail "Cannot copy the new version beside $target."; fi
detach
if ! mv "$target" "$backup"; then rm -rf "$stage"; fail "Cannot move the current version aside; it was left unchanged."; fi
if ! mv "$stage" "$target"; then
    mv "$backup" "$target" || fail "Replacing $target failed; the previous version is at $backup."
    rm -rf "$stage"; fail "Cannot move the new version into place; the previous version was restored."
fi
rm -rf "$backup"
rm -f "$package" "$updates/pending.json"
finish
report installed "CMRemote $version was installed."
[ "$relaunch" = 1 ] && open "$target"
exit 0
