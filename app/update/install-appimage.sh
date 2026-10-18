# Linux: replace the running AppImage file from the verified download.
[ -f "$target" ] || fail "$target is not an AppImage file."
# AppImage type 2: an ELF executable with "AI" and 2 at offset 8.
magic=$(od -An -tx1 -N11 "$package" | tr -d ' \n')
[ "$magic" = "7f454c46$(printf '%s' "$magic" | cut -c9-16)414902" ] || fail "The download is not an AppImage."
mode=$(stat -c %a "$target" 2>/dev/null || stat -f %Lp "$target") || fail "Cannot read the permissions of $target."
if ! cp "$package" "$stage" || ! chmod "$mode" "$stage" || ! chmod u+x "$stage"; then
    rm -f "$stage"; fail "Cannot write the new version beside $target; the previous version was left unchanged."
fi
# A hard link keeps the previous image until the replacement is in place.
ln "$target" "$backup" 2>/dev/null || backup=
if ! mv -f "$stage" "$target"; then
    rm -f "$stage"; [ -n "$backup" ] && rm -f "$backup"
    fail "Cannot move the new version into place; the previous version was left unchanged."
fi
[ -n "$backup" ] && rm -f "$backup"
rm -f "$package" "$updates/pending.json"
finish
report installed "CMRemote $version was installed."
if [ "$relaunch" = 1 ]; then nohup "$target" >/dev/null 2>&1 & fi
exit 0
