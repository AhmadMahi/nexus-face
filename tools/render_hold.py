"""The one overlay left: five seconds in, counting down to off."""
import math, sys
from PIL import Image, ImageDraw, ImageFont
W,H,S=128,64,6; OVER=[]
COUNT_MS = 3000
def fnt(s): return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S*s))
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def rect(d,x,y,w,h,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w): px(d,xx,yy,c)
def circ(d,cx,cy,r,c=255):
    for yy in range(cy-r-1,cy+r+2):
        for xx in range(cx-r-1,cx+r+2):
            if abs(math.hypot(xx-cx,yy-cy)-r)<0.7: px(d,xx,yy,c)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def ringArc(d,cx,cy,r,frac):
    frac=max(0.0,min(1.0,frac)); SEG=140
    for i in range(int(frac*SEG)):
        a=-1.5708+i*(6.2832/SEG)
        for rr in range(r-2,r+1): px(d,cx+int(rr*math.cos(a)),cy+int(rr*math.sin(a)))
def over(gone):
    im,d=new()
    gone=min(gone,COUNT_MS); left=1.0-gone/COUNT_MS
    secs=max(1,(COUNT_MS-gone+999)//1000)
    ctr(d,"GOING TO SLEEP",0,1,255,"t")
    CX,CY,R=W//2,33,19
    for dg in range(0,360,9):
        a_=math.radians(dg); px(d,CX+int(R*math.cos(a_)),CY+int(R*math.sin(a_)))
    for i in range(3):
        a_=-1.5708+i*2.0944
        for rr in range(R+1,R+4): px(d,CX+int(rr*math.cos(a_)),CY+int(rr*math.sin(a_)))
    ringArc(d,CX,CY,R,left)
    ctr(d,str(secs),21,3,255,"t")
    ctr(d,"hold 3s to wake me",55,1,255,"t")
    return im

def menu():
    im,d=new(); rect(d,0,0,W,11); ctr(d,"SETTINGS",2,1,0)
    for i,(a,b) in enumerate([("Brightness","100%"),("Watch face","dial"),
                              ("Sleep after","2m"),("Page turn","touch")]):
        y=14+i*12
        at(d,3,y,a,1,255); at(d,W-3-len(b)*6,y,b,1,255)
    return im
shots=[(menu(),"0 to 4s  nothing at all"),(over(0),"4.0s  decided"),
       (over(1100),"2  pad or no pad"),(over(2200),"1"),(over(2950),"7.0s  off")]
PAD=10;LBL=16;COLS=5
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),H*S+LBL+PAD*2),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+i*(W*S+PAD); cy=PAD
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_hold.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered -> out_hold.png")
