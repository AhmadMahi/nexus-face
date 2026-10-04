"""Render the reminder tile and the reading view at real pixel size,
mirroring drawReminders(). Four lengths of text and both the with-time
and no-clock cases, because the fit rule is the thing being checked."""
import math, sys
from PIL import Image, ImageDraw, ImageFont
W,H,S = 128,64,6
OVER=[]
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
    w=len(s)*6*size; hgt=8*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} w={w} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def bar(d,t): rect(d,0,0,W,11); ctr(d,t,2,1,0,"bar")

def bellIcon(d,cx,cy,r):
    dr=r*0.78
    for dg in range(180,361,2):
        px(d,cx+int(dr*math.cos(math.radians(dg))),cy-2+int(dr*math.sin(math.radians(dg))))
    vline(d,cx-int(dr),cy-2,r-1); vline(d,cx+int(dr),cy-2,r-1)
    hline(d,cx-r,cy+r-3,r*2+1)
    circ(d,cx,cy+r,2,255,True)
    px(d,cx,cy-3-int(dr))

def wrap(t,cols,maxLines=10):
    out=[];pos=0;ln=len(t)
    while pos<ln and len(out)<maxLines:
        take=min(ln-pos,cols)
        if pos+take<ln:
            sp=take
            while sp>0 and t[pos+sp]!=' ': sp-=1
            if sp>0: take=sp
        out.append(t[pos:pos+take]); pos+=take
        while pos<ln and t[pos]==' ': pos+=1
    return out

def tile(p, total):
    im,d=new(); bar(d,"REMINDERS"); bellIcon(d,W//2,30,11)
    if not total: c="nothing waiting"
    elif p==1:    c="1 reminder"
    elif p:       c=f"{p} reminders"
    else:         c="all done"
    ctr(d,c,46,1,255,"tile"); ctr(d,"hold to read" if total else "Rafiq puts them here",55,1,255,"tile")
    return im

def reader(text, idx, n, when=None, done=False, ms=0):
    im,d=new()
    top = 13 if when else 3; bottom=52; h=bottom-top+1
    size=2; lines=wrap(text,10)
    if len(lines)*18>h: size=1; lines=wrap(text,21)
    lh=18 if size==2 else 10; blockH=len(lines)*lh
    if blockH>h:
        travel=blockH-h; climb=travel*1000//9; cycle=1800+climb+1800
        t=ms%cycle
        off = 0 if t<1800 else (int((t-1800)*travel/climb) if t<1800+climb else travel)
    else: off=-(h-blockH)//2
    mid = size==2 or len(lines)<=2
    for i,s in enumerate(lines):
        y=top+i*lh-off
        if y>bottom or y+8*size<top: continue
        if mid: ctr(d,s,y,size,255,"body")
        else:   at(d,2,y,s,size,255,"body")
    rect(d,0,0,W,top,0); rect(d,0,bottom+1,W,H-bottom-1,0)
    if when: at(d,2,2,when,1,255,"when")
    if done: at(d,W-2-4*6,2,"done",1,255,"done")
    ctr(d,f"{idx+1} of {n}",56,1,255,"ofN")
    return im

SHORT="Call Amma"
MED="Service the bike at 10"
LONG="Submit the Workday integration report before the review call"
XLONG=("Pick up the parcel from the gate office and sign for it before six, "
       "it closes early")[:95]
shots=[
 (tile(3,3),"tile  3 waiting"),
 (tile(0,0),"tile  empty"),
 (reader(SHORT,0,3,"09:30  Mon 06 Oct"),"short  size 2"),
 (reader(MED,1,3,"10:00  Tue 07 Oct"),"medium  size 2"),
 (reader(LONG,2,3,"18:15  Wed 08 Oct"),"long  size 1 left"),
 (reader(LONG,2,3,None),"long  no clock"),
 (reader(XLONG,0,2,"06:00  Thu 09 Oct",ms=0),"crawl  top"),
 (reader(XLONG,0,2,"06:00  Thu 09 Oct",ms=2600),"crawl  moved"),
]
PAD=10; LBL=16; COLS=4
rows=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL)*rows+PAD*(rows+1)),40)
dd=ImageDraw.Draw(sheet); lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",20)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_rem.png")
if OVER:
    print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("reminder screens rendered, nothing off the edge -> out_rem.png")
