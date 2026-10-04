"""The gesture mode screens at real pixel size."""
import math, sys
from PIL import Image, ImageDraw, ImageFont
W,H,S=128,64,6; OVER=[]
def fnt(s): return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S*s))
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def rect(d,x,y,w,h,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w): px(d,xx,yy,c)
def rrect(d,x,y,w,h,r,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w):
            dx=max(x+r-xx,0,xx-(x+w-1-r)); dy=max(y+r-yy,0,yy-(y+h-1-r))
            if math.hypot(dx,dy)<=r+0.2: px(d,xx,yy,c)
def line(d,a,b,c=255):
    n=max(abs(b[0]-a[0]),abs(b[1]-a[1]),1)
    for i in range(n+1): px(d,round(a[0]+(b[0]-a[0])*i/n),round(a[1]+(b[1]-a[1])*i/n),c)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)

def mic(muted):
    im,d=new(); cx=30
    rrect(d,cx-7,6,15,21,7)
    for a_ in range(20,161,3):
        px(d,cx+int(13*math.cos(math.radians(a_))),24+int(13*math.sin(math.radians(a_))))
    rect(d,cx,37,1,6); rect(d,cx-7,43,15,1)
    if muted:
        for i in (-1,0,1): line(d,(cx-16+i,2),(cx+16+i,46))
    at(d,54,12,"MUTED" if muted else "LIVE",2,255,"mic")
    at(d,54,34,"2 unmutes" if muted else "1 mutes",1,255,"mic")
    return im

def idle():
    im,d=new()
    ctr(d,"gesture mode",24,1,255,"g")
    ctr(d,"the pad is your Mac",36,1,255,"g")
    ctr(d,"hold 4s to stop",52,1,255,"g")
    return im

shots=[(idle(),"gesture mode, idle"),(mic(False),"on a call, live"),(mic(True),"muted")]
PAD=10;LBL=16;COLS=3
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),H*S+LBL+PAD*2),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",20)
for i,(im,lab) in enumerate(shots):
    cx=PAD+i*(W*S+PAD); cy=PAD
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_gest.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered -> out_gest.png")
