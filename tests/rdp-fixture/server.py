from pathlib import Path
import tkinter as tk

if Path('/tmp/rm-performance-control.json').is_file():
    from performance import run
    run()
    raise SystemExit(0)

if Path('/tmp/rm-session-features').is_file():
    from session_features import run
    run()
    raise SystemExit(0)

root = tk.Tk()
root.overrideredirect(True)
root.title('RemoteManager owned RDP fixture')
label = tk.Label(root, text='Owned localhost RDP fixture', font=('sans', 24))
label.pack(expand=True, fill='both')
root.clipboard_clear()
root.clipboard_append('server-clipboard-fixture')


def record(event):
    with Path('/tmp/rm-input').open('a') as out:
        out.write(str(event.type) + ' ' + str(getattr(event, 'keysym', '')) + ' ' +
                  str(getattr(event, 'x_root', -1)) + ' ' + str(getattr(event, 'y_root', -1)) + '\n')


root.bind_all('<KeyPress>', record)
root.bind_all('<ButtonPress>', record)
root.focus_force()
step = 0


def tick():
    global step
    step += 1
    if step == 20:
        root.clipboard_clear()
        root.clipboard_append('server-clipboard-fixture')
    root.geometry(str(root.winfo_screenwidth()) + 'x' + str(root.winfo_screenheight()) + '+0+0')
    label.configure(bg='#205050' if step % 2 else '#405020')
    try:
        text = root.clipboard_get()
        if text == 'client-clipboard-fixture': Path('/tmp/rm-clipboard').write_text(text)
    except tk.TclError:
        pass
    root.after(100, tick)


root.after(100, tick)
root.mainloop()
