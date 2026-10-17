"""Deterministic synthetic scenes; the normal clipboard/resize fixture is unchanged."""
FRAMES = 180
PERIOD_NS = 16_666_667
BACKGROUND = 0x183048


def scene(workload, step, width, height):
    if workload not in range(6) or not 0 <= step <= FRAMES:
        raise ValueError('Invalid synthetic workload')
    if not 320 <= width <= 8192 or not 240 <= height <= 8192 or width * height > 16 * 1024 * 1024:
        raise ValueError('Invalid synthetic geometry')
    rectangles = []
    background = BACKGROUND
    if workload == 1:
        for i in range(4):
            rectangles.append([32 + i * (width - 64) // 4, 64 + i * 24, 8, 8,
                               0xf0c020 if (step + i) % 2 else 0x20c0f0])
    elif workload == 2:
        for i, color in enumerate((0xf08020, 0x20c080, 0x8060e0)):
            x = 24 + (step * (5 + i) + i * 60) % (width - 224)
            y = 48 + i * 40
            rectangles.append([x, y, 180, 100, color])
    elif workload == 3:
        for i in range(12):
            y = 32 + (i * 36 + step * 4) % (height - 72)
            rectangles.append([32, y, width - 64, 12, 0xe0c080 if i % 2 else 0x60a0e0])
    elif workload == 4:
        background = ((step * 3 % 256) << 16) | ((step * 5 % 256) << 8) | (step * 7 % 256)
    elif workload == 5:
        rectangles.append([64, 64, 64, 32, 0x20c080])
    return {'background': background, 'rectangles': rectangles}


def sample_points(width, height):
    return [[x, y] for y in range(32, height, 32) for x in range(16, width, 32)]


def expected_pixels(workload, step, width, height):
    state = scene(workload, step, width, height)
    result = []
    for x, y in sample_points(width, height):
        color = state['background']
        for left, top, w, h, value in state['rectangles']:
            if left <= x < left + w and top <= y < top + h:
                color = value
        result.append(color)
    return result


def run():
    import json
    import time
    import tkinter as tk
    from pathlib import Path
    config = json.loads(Path('/tmp/rm-performance-control.json').read_text())
    workload = config['workload']
    root = tk.Tk(); root.overrideredirect(True)
    root.title('Owned synthetic RDP performance fixture')
    width, height = root.winfo_screenwidth(), root.winfo_screenheight()
    root.geometry(f'{width}x{height}+0+0')
    state = scene(workload, 0, width, height)
    canvas = tk.Canvas(root, bg=f"#{state['background']:06x}", highlightthickness=0, borderwidth=0)
    canvas.pack(expand=True, fill='both')
    items = []
    for x, y, w, h, color in state['rectangles']:
        items.append(canvas.create_rectangle(x, y, x + w, y + h, fill=f'#{color:06x}', outline=''))
    # Eight frame bits and one completion bit, sampled only from owned synthetic pixels.
    markers = [canvas.create_rectangle(i * 8, 0, (i + 1) * 8, 8, fill='#000000', outline='') for i in range(9)]
    def record(event):
        with Path('/tmp/rm-input').open('a') as out:
            out.write(str(event.type) + ' ' + str(getattr(event, 'keysym', '')) + '\n')
    root.bind_all('<KeyPress>', record); root.bind_all('<ButtonPress>', record); root.focus_force()
    start = 0
    last_marker = 0
    def tick(step):
        nonlocal state, start, last_marker
        if step == 1: start = time.monotonic_ns()
        next_state = scene(workload, step, width, height)
        if next_state['background'] != state['background']:
            canvas.configure(bg=f"#{next_state['background']:06x}")
        for i, rectangle in enumerate(next_state['rectangles']):
            x, y, w, h, color = rectangle
            previous = state['rectangles'][i]
            if rectangle[:4] != previous[:4]: canvas.coords(items[i], x, y, x + w, y + h)
            if color != previous[4]: canvas.itemconfigure(items[i], fill=f'#{color:06x}')
        marker = step | (256 if step == FRAMES else 0)
        # Idle has only start/completion damage, never a simulated continuous desktop tick.
        if workload == 0 and step != FRAMES: marker = 0
        for i, item in enumerate(markers):
            if (marker ^ last_marker) & (1 << i):
                canvas.itemconfigure(item, fill='#ffffff' if marker & (1 << i) else '#000000')
        last_marker = marker; state = next_state
        if step < FRAMES:
            due = start + step * PERIOD_NS
            root.after(max(1, (due - time.monotonic_ns() + 999_999) // 1_000_000), tick, step + 1)
        else:
            report = {'schema': 1, 'workload': workload, 'width': width, 'height': height,
                      'frames': FRAMES, 'elapsed_ns': time.monotonic_ns() - start,
                      'pixels': expected_pixels(workload, FRAMES, width, height)}
            Path('/tmp/rm-performance-result.json').write_text(json.dumps(report))
    root.after(1200, tick, 1)
    root.mainloop()
