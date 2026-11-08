# Linux: replace the running AppImage file from the verified download.
# While CMRemote runs: the new image is copied beside the installed one.
stage_update() {
    [ -f "$target" ] || refuse "$target is not an AppImage file."
    # AppImage type 2: an ELF executable with "AI" and 2 at offset 8.
    magic=$(od -An -tx1 -N11 "$package" | tr -d ' \n')
    [ "$magic" = "7f454c46$(printf '%s' "$magic" | cut -c9-16)414902" ] || refuse "The download is not an AppImage."
    mode=$(stat -c %a "$target" 2>/dev/null || stat -f %Lp "$target") || refuse "Cannot read the permissions of $target."
    total=$(( ($(wc -c < "$package") + 1023) / 1024 ))
    if ! copy_with_progress "$stage" "$total" cp "$package" "$stage" || ! chmod "$mode" "$stage" || ! chmod u+x "$stage"; then
        refuse "Cannot write the new version beside $target; the previous version was left unchanged."
    fi
}
# After CMRemote exits: the copy replaces the image in one rename.
install_update() {
    # A hard link keeps the previous image until the replacement is in place.
    ln "$target" "$backup" 2>/dev/null || backup=
    if ! mv -f "$stage" "$target"; then
        [ -n "$backup" ] && rm -f "$backup"
        fail "Cannot move the new version into place; the previous version was left unchanged."
    fi
    stage=; [ -n "$backup" ] && rm -f "$backup"
}
relaunch_update() { nohup "$target" >/dev/null 2>&1 & }
main
