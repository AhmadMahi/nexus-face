from gfx import OLED, W, H
from PIL import Image
shots = []
def shot(name, o): o.png(f'{name}.png'); shots.append((name, o))

# phone guard, both halves of the blink (ported from drawGuard)
for b in (0, 1):
    o = OLED(); fg = 0 if b else 1; bg = 1 if b else 0
    if b: o.rect(0, 0, W, H, 1)
    o.rrect(30, 8, 24, 18, 6, fg); o.rrect(74, 8, 24, 18, 6, fg)
    o.tri((30, 6), (54, 6), (30, 14), bg); o.tri((98, 6), (74, 6), (98, 14), bg)
    o.col = fg; o.ctr('WAIT FOR ME', 34, 1, 'guard'); o.ctr('Phone is far away', 48, 1, 'guard'); o.col = 1
    shot(f'guard_{b}', o)
# find me
for b in (0, 1):
    o = OLED()
    if b: o.rect(0, 0, W, H, 1)
    o.col = 0 if b else 1; o.ctr("I'M HERE", 18, 2, 'find'); o.ctr('press to stop', 46, 1, 'find'); o.col = 1
    shot(f'find_{b}', o)
# tamper countdown
o = OLED(); o.titleBarC('TAMPER ALARM'); o.ctr('7', 18, 3, 'tamper'); o.ctr('press to cancel', 52, 1, 'tamper'); shot('tamper_count', o)
# two-line cards, exactly as drawToast lays them out
def card(head, a, b, name):
    o = OLED(); o.titleBar(head, '14:32'); o.ctr(a, 22, 1, name); o.ctr(b, 34, 1, name); shot(name, o)
card('HOTSPOT', 'RAFIQ-SETUP password', '192.168.4.1/ota', 'hotspot')
card('RAFIQ', 'BT paired 82%', 'sync 3h ago guard', 'status')
card('RAFIQ', 'BT no radio 100%', 'sync 23h ago guard', 'status_longest')
# tamper log
o = OLED(); o.titleBar('TAMPER LOG', '6')
rows = ['06/10 18:02 Disarmed', '06/10 17:41 Moved', '06/10 17:40 Touched', '06/10 17:12 Moved']
for r, t in enumerate(rows):
    y = 14 + r*12
    if r == 0: o.rect(0, y-2, W, 12, 1); o.col = 0
    o.text(3, y, t[:20], 1, 'tlog'); o.col = 1
o.text(0, 0, '')
shot('tamper_log', o)
# settings groups
def group(title, rows, sel, name):
    o = OLED(); o.titleBar(title, '14:32')
    for r, (n, v) in enumerate(rows):
        y = 14 + r*12
        if r == sel: o.rect(0, y-2, W, 12, 1); o.col = 0
        o.text(3, y, n, 1, name); o.text(W - 3 - len(v)*6, y, v, 1, name); o.col = 1
    shot(name, o)
group('Wireless', [('Pair a Mac', 'paired'), ('Prayer times', 'saved'), ('Check update', ''), ('Phone guard', 'on')], 3, 'set_wireless')
group('Wireless', [('Network', 'no signal'), ('Hotspot', 'hotspot'), ('Pair a Mac', 'paired'), ('Prayer times', 'saved')], 0, 'set_network_longest')
group('System', [('Power down', '2 min'), ('Battery full', '4.10V'), ('Tamper alarm', 'hold'), ('Tamper log', 'hold')], 2, 'set_system')
# weather bottom line, worst cases
o = OLED()
for i, t in enumerate(['Bengaluru  3h ago', 'Thiruvananth  59m ago'[:0] + 'Thiruvanant  59m ago', 'Thiruvanant  23h ago', 'Thiruvanant  99d ago']):
    o.ctr(t, 4 + i*12, 1, 'weather')
shot('weather_age_lines', o)

over = [x for _, o in shots for x in o.over]
print('OVERFLOWS:', over if over else 'none')
# one contact sheet
tiles = [Image.open(f'{n}.png') for n, _ in shots]
cols = 3; tw, th = tiles[0].size
sheet = Image.new('RGB', (cols*tw + (cols+1)*10, ((len(tiles)+cols-1)//cols)*th + ((len(tiles)+cols-1)//cols + 1)*10), (20, 20, 20))
for i, t in enumerate(tiles): sheet.paste(t, (10 + (i % cols)*(tw+10), 10 + (i//cols)*(th+10)))
sheet.save('sheet.png'); print(len(tiles), 'screens')
