"""The reminder card as it fires, and the reading view, at real pixel
size. Mirrors drawToast() and drawReminders() including the shared
fitText()."""
import math, sys
from PIL import Image, ImageDraw, ImageFont
W,H,S=128,64,6; OVER=[]
def fnt(s): return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S*s))
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def get(im,x,y):
    return im.getpixel((x*S, y*S))
def rect(d,x,y,w,h,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w): px(d,xx,yy,c)
def invert(im):
    return Image.eval(im, lambda v: 255 - v)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)

def wrap(t,cols,maxLines=10):
    out,pos=[],0
    while pos<len(t) and len(out)<maxLines:
        take=min(len(t)-pos,cols)
        if pos+take<len(t):
            sp=take
            while sp>0 and t[pos+sp]!=" ": sp-=1
            if sp>0: take=sp
        out.append(t[pos:pos+take]); pos+=take
        while pos<len(t) and t[pos]==" ": pos+=1
    return out

def fitText(d, text, top, bottom, ms):
    h=bottom-top+1
    size, lines = 2, wrap(text,10)
    if len(lines)*18>h: size, lines = 1, wrap(text,21)
    lh=18 if size==2 else 10; blockH=len(lines)*lh
    if blockH>h:
        travel=blockH-h; climb=travel*1000//9; cycle=1800+climb+1800
        t=ms%cycle
        off = 0 if t<1800 else (int((t-1800)*travel/climb) if t<1800+climb else travel)
    else: off=-(h-blockH)//2
    mid = size==2 or len(lines)<=2
    for i,ln in enumerate(lines):
        y=top+i*lh-off
        if y>bottom or y+8*size<top: continue
        if mid: ctr(d,ln,y,size,255,"body")
        else:   at(d,2,y,ln,size,255,"body")
    rect(d,0,0,W,top,0); rect(d,0,bottom+1,W,H-bottom-1,0)

def card(text, ms=0, flash=False):
    im,d=new()
    fitText(d,text,14,62,ms)
    rect(d,0,0,W,12); ctr(d,"2 to snooze 15 min",2,1,0,"ribbon")
    return invert(im) if flash else im

def reader(text, idx, n, day=None, hm=None, done=False, ms=0):
    im,d=new()
    top = 14 if day else 3; bottom=53
    fitText(d,text,top,bottom,ms)
    if day:
        rect(d,0,0,W,12)
        at(d,2,2,day,1,0,"ribbon")
        right = "done" if done else hm
        at(d,W-2-len(right)*6,2,right,1,0,"ribbon")
    elif done:
        at(d,W-2-4*6,2,"done",1,255,"done")
    ctr(d,f"{idx+1} of {n}",56,1,255,"ofN")
    return im

SHORT="Call Amma"
MED="Service the bike at 10"
LONG="Submit the Workday integration report before the review call"
XL=("Pick up the parcel from the gate office and sign for it before six, "
    "they close early on a Friday")[:95]
shots=[
 (card(SHORT),"card  short"),
 (card(LONG),"card  long"),
 (card(XL,ms=2600),"card  crawling"),
 (card(MED,flash=True),"card  the flash, every 3s"),
 (reader(SHORT,0,5,"Mon 06 Oct","09:30"),"reader  ribbon"),
 (reader(LONG,2,5,"Wed 08 Oct","18:15"),"reader  long"),
 (reader(MED,1,5,"Tue 07 Oct","10:00",done=True),"reader  done"),
 (reader(LONG,2,5),"reader  no clock"),
]
PAD=10;LBL=16;COLS=4;rows=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL)*rows+PAD*(rows+1)),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_card.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered, nothing off the edge -> out_card.png")
