"""The screens v5.3.0 changes, at real pixel size: the stopwatch in
three states, the focus row with its icon, and the card that says
reminders landed."""
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
def hline(d,x,y,w,c=255): rect(d,x,y,w,1,c)
def vline(d,x,y,h,c=255): rect(d,x,y,1,h,c)
def circ(d,cx,cy,r,c=255,fill=False):
    for yy in range(cy-r-1,cy+r+2):
        for xx in range(cx-r-1,cx+r+2):
            dd=math.hypot(xx-cx,yy-cy)
            if (dd<=r) if fill else (abs(dd-r)<0.7): px(d,xx,yy,c)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def bar(d,t): rect(d,0,0,W,11); ctr(d,t,2,1,0,"bar")
def titleBar(d,t,r):
    rect(d,0,0,W,11); ctr(d,t,2,1,0,"tb")
    if r: at(d,W-2-len(r)*6,2,r,1,0,"tb")
def bellIcon(d,cx,cy,r):
    dr=r*0.78
    for dg in range(180,361,2):
        px(d,cx+int(dr*math.cos(math.radians(dg))),cy-2+int(dr*math.sin(math.radians(dg))))
    vline(d,cx-int(dr),cy-2,r-1); vline(d,cx+int(dr),cy-2,r-1)
    hline(d,cx-r,cy+r-3,r*2+1); circ(d,cx,cy+r,2,255,True); px(d,cx,cy-3-int(dr))

def swstr(ms):
    s=ms//1000
    if s>=3600: return f"{s//3600}:{(s//60)%60:02d}:{s%60:02d}"
    return f"{s//60}:{s%60:02d}.{(ms//100)%10}"

def stopwatch(ms, run):
    im,d=new(); e=swstr(ms); bar(d,"STOPWATCH")
    sz = 2 if len(e)>5 else 3
    at(d,(W-len(e)*6*sz)//2, 20 if sz==3 else 24, e, sz,255,"sw")
    if run:
        rect(d,58,44,3,8); rect(d,64,44,3,8)
    else:
        for i in range(8): vline(d,60+i,44+i//2,8-(i//2)*2)
    ctr(d,"1 stop   hold zero" if run else ("1 go   hold zero" if ms else "1 to start"),56,1,255,"sw")
    return im

def focuslist(sel, ms):
    im,d=new(); titleBar(d,"TO DO","3/3")
    rows=[("Write the report","25m",False),("Reply to Imran","10m",True)]
    for k,(nm,m,done) in enumerate(rows):
        y=14+k*12; on=(sel==k)
        if on: rect(d,0,y-2,W,12)
        c=0 if on else 255
        if done:
            for i in range(3): px(d,4+i,y+4+i,c)
            for i in range(5): px(d,6+i,y+6-i,c)
        at(d,13,y,nm[:14],1,c,"row"); at(d,W-4-len(m)*6,y,m,1,c,"row")
    y=14+2*12; on=(sel==2)
    if on: rect(d,0,y-2,W,12)
    c=0 if on else 255
    circ(d,8,y+3,4,c); vline(d,8,y,4,c); hline(d,6,y-2,5,c)
    at(d,16,y,"Stopwatch",1,c,"row")
    if ms:
        e=swstr(ms); at(d,W-4-len(e)*6,y,e,1,c,"row")
    return im

def card(a,b):
    im,d=new(); bar(d,"REMINDERS")
    if b: bellIcon(d,W//2,24,7); ctr(d,a,38,1,255,"card"); ctr(d,b,50,1,255,"card")
    else: bellIcon(d,W//2,26,8); ctr(d,a,44,1,255,"card")
    return im

shots=[(stopwatch(0,False),"stopwatch  idle"),
       (stopwatch(94300,True),"running"),
       (stopwatch(94300,False),"stopped, holding its number"),
       (stopwatch(3725000,True),"past the hour"),
       (focuslist(2,94300),"to do, stopwatch selected"),
       (card("1 reminder added","first at 09:30"),"one landed"),
       (card("3 reminders added","first at 18:15"),"three landed"),
       (card("2 reminders added","4 waiting"),"no time on them")]
PAD=10;LBL=16;COLS=4;rows=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL)*rows+PAD*(rows+1)),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_v53.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered, nothing off the edge -> out_v53.png")
