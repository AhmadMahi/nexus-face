"""Render the six vehicle templates at real pixel size, mirroring
drawBike() line for line, and shout about anything that falls off the
128x64 edge. Drawn from the same field values the firmware defaults to,
because a template that only fits a short plate is not a template."""
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
def box(d,x,y,w,h,c=255):
    for xx in range(x,x+w): px(d,xx,y,c); px(d,xx,y+h-1,c)
    for yy in range(y,y+h): px(d,x,yy,c); px(d,x+w-1,yy,c)
def plateBox(d,x,y,w,h):
    box(d,x,y,w,h)
    for cx,cy in ((x,y),(x+w-1,y),(x,y+h-1),(x+w-1,y+h-1)): px(d,cx,cy,0)
def hline(d,x,y,w,c=255): rect(d,x,y,w,1,c)
def vline(d,x,y,h,c=255): rect(d,x,y,1,h,c)
def circ(d,cx,cy,r,c=255,fill=False):
    for yy in range(cy-r-1,cy+r+2):
        for xx in range(cx-r-1,cx+r+2):
            dd=math.hypot(xx-cx,yy-cy)
            if (dd<=r) if fill else (abs(dd-r)<0.7): px(d,xx,yy,c)
def line(d,a,b,c=255):
    n=max(abs(b[0]-a[0]),abs(b[1]-a[1]),1)
    for i in range(n+1):
        px(d,round(a[0]+(b[0]-a[0])*i/n),round(a[1]+(b[1]-a[1])*i/n),c)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size; hgt=8*size
    if x<0 or x+w>W+1 or y<0 or y+hgt-1>H:
        OVER.append(f"{tag}: {s!r} at x={x} y={y} is {w}x{hgt}, "
                    f"right={x+w} bottom={y+hgt-1}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def bar(d,t):
    rect(d,0,0,W,11); ctr(d,t,2,1,0,"bar")

def bikeIcon(d,x,y):
    circ(d,x+5,y+11,4); circ(d,x+27,y+11,4)
    px(d,x+5,y+11); px(d,x+27,y+11)
    for a,b in (((x+5,y+11),(x+13,y+4)),((x+13,y+4),(x+22,y+5)),
                ((x+22,y+5),(x+27,y+11)),((x+13,y+4),(x+16,y+11)),
                ((x+16,y+11),(x+27,y+11))): line(d,a,b)
    rect(d,x+10,y+2,7,2); rect(d,x+20,y+1,2,4); rect(d,x+17,y+6,5,3)
def dots(d,x0,x1,y):
    x=x0
    while x<x1: px(d,x,y); x+=3
def tornEdge(d,y):
    x=4
    while x<W: circ(d,x,y,3,0,True); x+=9

PLATE="KA 50 HJ 5683"; MAKE="Royal Enfield"; MODEL="Meteor 350"; OWNER="Ahmed"
def split():
    p=PLATE.split(" ")
    if len(p)>=3: return " ".join(p[:2]), " ".join(p[2:])
    return PLATE, ""
TOP,BOT=split()

def tpl(n, edit=False):
    im,d=new(); t=f"tpl{n}"
    if n==1:
        ctr(d,PLATE,2,1,255,t); hline(d,0,12,W)
        bikeIcon(d,3,20); vline(d,40,15,47)
        at(d,44,17,MAKE,1,255,t); at(d,44,29,MODEL,1,255,t)
        at(d,44,43,OWNER,1,255,t)
    elif n==2:
        bar(d,"VEHICLE")
        K=["PLATE","MAKE","MODEL","OWNER"]; V=[PLATE,MAKE,MODEL,OWNER]
        for i in range(4):
            y=14+i*10; at(d,4,y,K[i],1,255,t)
            vw=len(V[i])*6; at(d,W-4-vw,y,V[i],1,255,t)
            if i<3: dots(d,6+len(K[i])*6,W-6-vw,y+4)
    elif n==3:
        rect(d,0,0,W,13); ctr(d,MAKE,3,1,0,t); tornEdge(d,13)
        if BOT: ctr(d,TOP,19,2,255,t); ctr(d,BOT,36,2,255,t)
        else:   ctr(d,TOP,28,2,255,t)
        ctr(d,f"{MODEL} / {OWNER}",55,1,255,t)
    elif n==4:
        CX,CY,R=64,50,36
        for dg in range(180,361,2):
            px(d,CX+int(R*math.cos(math.radians(dg))),CY+int(R*math.sin(math.radians(dg))))
        for i in range(7):
            a=math.radians(180+30*i); ln=3 if i&1 else 6
            for r in range(R-ln,R+1):
                px(d,CX+int(r*math.cos(a)),CY+int(r*math.sin(a)))
        a=math.radians(180+30*4.2)
        line(d,(CX,CY),(CX+int(20*math.cos(a)),CY+int(20*math.sin(a))))
        circ(d,CX,CY,2,255,True)
        ctr(d,MODEL,2,1,255,t); plateBox(d,18,51,92,13); ctr(d,PLATE,54,1,255,t)
    elif n==5:
        if BOT: ctr(d,TOP,16,2,255,t); ctr(d,BOT,36,2,255,t)
        else:   ctr(d,TOP,24,2,255,t)
    else:
        plateBox(d,6,2,W-12,38 if BOT else 24)
        if BOT: ctr(d,TOP,5,2,255,t); ctr(d,BOT,22,2,255,t)
        else:   ctr(d,TOP,9,2,255,t)
        ctr(d,MAKE,43,1,255,t); ctr(d,f"{MODEL}  {OWNER}",53,1,255,t)
    if edit:
        rect(d,0,53,W,11); ctr(d,f"{n+1} of 6  hold to keep",56,1,0,t+"edit")
    return im

NAMES=["plate card","badge","garage board","ticket stub","speedo","minimal"]
PAD=10; LBL=16
sheet=Image.new("L",(W*S*3+PAD*4,(H*S+LBL)*2+PAD*3),40)
dd=ImageDraw.Draw(sheet); lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",22)
for i in range(6):
    im=tpl(i, edit=(i==2))
    cx=PAD+(i%3)*(W*S+PAD); cy=PAD+(i//3)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy))
    dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),f"{i+1}  {NAMES[i]}"+("   (edit strip)" if i==2 else ""),
            fill=230,font=lf)
sheet.save("out_bike.png")
if OVER:
    print("OVERFLOW:")
    for o in OVER: print("  ",o)
    sys.exit(1)
print("six templates rendered, nothing off the edge -> out_bike.png")
