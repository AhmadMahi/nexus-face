"""The ring the screen shows while you hold the pad to wake it."""
import math, sys
from PIL import Image, ImageDraw, ImageFont
W,H,S=128,64,6
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def circ(d,cx,cy,r,c=255):
    for yy in range(cy-r-1,cy+r+2):
        for xx in range(cx-r-1,cx+r+2):
            if abs(math.hypot(xx-cx,yy-cy)-r)<0.7: px(d,xx,yy,c)
def ringArc(d,cx,cy,r,frac):
    frac=max(0.0,min(1.0,frac)); SEG=140
    for i in range(int(frac*SEG)):
        a=-1.5708+i*(6.2832/SEG)
        for rr in range(r-2,r+1): px(d,cx+int(rr*math.cos(a)),cy+int(rr*math.sin(a)))
RING_AFTER, NEED = 500, 3000
def shot(held):
    im,d=new()
    if held>=RING_AFTER:
        ringArc(d,W//2,H//2,16,(held-RING_AFTER)/(NEED-RING_AFTER))
        circ(d,W//2,H//2,16)
    return im
shots=[(shot(300),"0.3s  screen still off"),(shot(800),"0.8s"),
       (shot(1700),"1.7s"),(shot(2600),"2.6s"),(shot(3000),"3.0s  it wakes")]
PAD=10;LBL=16;COLS=5
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),H*S+LBL+PAD*2),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+i*(W*S+PAD); cy=PAD
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_wake.png")
print("wake ring rendered -> out_wake.png")
