"""Opt-in cursor/text-clipboard fixture; never uses the client's host clipboard."""
from pathlib import Path
import tkinter as tk


def run():
    root = tk.Tk()
    root.overrideredirect(True)
    root.title('Owned cursor and text clipboard fixture')
    background = tk.Label(root, text='Owned cursor / text clipboard fixture', anchor='s')
    background.pack(expand=True, fill='both')
    regions = [('hand', 'hand2', 60, 100), ('text', 'xterm', 280, 100),
               ('resize', 'sb_h_double_arrow', 540, 100), ('crosshair', 'crosshair', 60, 240),
               ('busy', 'watch', 280, 240), ('default', 'arrow', 540, 240)]
    for name, cursor, x, y in regions:
        widget = tk.Label(root, text=name, cursor=cursor, bg='#eeeeee', fg='#111111')
        widget.place(x=x, y=y, width=180, height=80)
        widget.bind('<Enter>', lambda event, name=name: record_cursor(name))
    root.clipboard_clear()
    root.clipboard_append('server-session-initial')

    def copy(text):
        # Tk otherwise keeps the same selection owner and xrdp sees no new-copy notification.
        root.selection_clear(selection='CLIPBOARD')
        root.clipboard_clear()
        root.clipboard_append(text)
        root.update_idletasks()

    def paste(event):
        try:
            text = root.clipboard_get().replace('\r\n', '\n').replace('\r', '\n')
            Path('/tmp/rm-session-paste').write_text(text)
            copy('paste-verified' if text == 'host-session-clipboard\nemoji: 😀' else 'paste-mismatch')
        except tk.TclError:
            copy('paste-unavailable')

    def record_cursor(name):
        with Path('/tmp/rm-cursors').open('a') as out:
            out.write(name + '\n')

    def record(event):
        with Path('/tmp/rm-input').open('a') as out:
            out.write(str(event.type) + ' ' + str(getattr(event, 'keysym', '')) + ' ' +
                      str(getattr(event, 'x_root', -1)) + ' ' + str(getattr(event, 'y_root', -1)) + '\n')

    root.bind_all('<KeyPress>', record)
    root.bind_all('<ButtonPress>', record)
    root.bind('<F6>', lambda event: copy('server-session-clipboard\nemoji: 😀'))
    root.bind('<F7>', lambda event: copy('server-session-disabled'))
    root.bind('<F8>', paste)
    root.focus_force()
    step = 0

    def tick():
        nonlocal step
        step += 1
        root.geometry(str(root.winfo_screenwidth()) + 'x' + str(root.winfo_screenheight()) + '+0+0')
        background.configure(bg='#205050' if step % 2 else '#405020')
        Path('/tmp/rm-session-ready').touch()
        root.after(100, tick)

    root.after(100, tick)
    root.mainloop()
