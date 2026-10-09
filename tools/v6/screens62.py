from gfx import OLED, W, H
from PIL import Image
shots=[]
def shot(n,o): o.png(f'62_{n}.png'); shots.append((n,o))
HOLD=700; MAX=max(HOLD+1500,2000); MID=HOLD+(MAX-HOLD)//2
def base_list():
    o=OLED(); o.titleBar('NOTIFICATIONS','4')
    for r,t in enumerate(['*WhatsApp: Sara','*Mail: Invoice 4432','Messages: Mom','Calendar: Standup']):
        y=14+r*12
        if r==1: o.rect(0,y-2,W,12,1); o.col=0
        o.text(3,y,t[:20]); o.col=1
    return o
def strip(o,held,name):
    f=max(0,min(1,(held-HOLD)/(MAX-HOLD))); back=held>=MID
    o.rect(0,51,W,13,0); o.rect(0,51,W,1,1)
    o.rrect(2,54,80,8,3,1); o.rrect(3,55,78,6,2,0)
    w=int(76*f)
    if w>1: o.rrect(4,56,w,4,1,1)
    tick=4+int(76*(MID-HOLD)/(MAX-HOLD)); o.rect(tick,52,1,2,1)
    o.rrect(86,53,40,10,3,1); lab='BACK' if back else 'OPEN'
    o.col=0; o.text(86+(40-len(lab)*6)//2,54,lab,1,name); o.col=1
    shot(name,o)
strip(base_list(),1000,'strip_open'); strip(base_list(),1800,'strip_back'); strip(base_list(),3000,'strip_back_full')
for t,n in [('iPhone connected','ban_on'),('iPhone disconnected','ban_off')]:
    o=OLED(); o.ctr(t,28,1,n); shot(n,o)
print('OVERFLOWS:',[x for _,o in shots for x in o.over] or 'none')
tiles=[Image.open(f'62_{n}.png') for n,_ in shots]; cols=3; tw,th=tiles[0].size; rows=(len(tiles)+cols-1)//cols
sh=Image.new('RGB',(cols*tw+(cols+1)*10,rows*th+(rows+1)*10),(20,20,20))
for i,t in enumerate(tiles): sh.paste(t,(10+(i%cols)*(tw+10),10+(i//cols)*(th+10)))
sh.save('sheet62.png')
