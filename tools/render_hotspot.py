"""The notification screens at real pixel size."""
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
def line(d,a,b,c=255):
    n=max(abs(b[0]-a[0]),abs(b[1]-a[1]),1)
    for i in range(n+1): px(d,round(a[0]+(b[0]-a[0])*i/n),round(a[1]+(b[1]-a[1])*i/n),c)
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
def titleBar(d,title,right):
    rect(d,0,0,W,11)
    at(d,3,2,title,1,0,"ribbon-l")
    at(d,W-3-len(right)*6,2,right,1,0,"ribbon-r")

def hotspot():
    im,d=new(); titleBar(d,"HOTSPOT","on")
    ctr(d,"RAFIQ-SETUP",16,1,255,"ssid")
    ctr(d,"pass  password",28,1,255,"pass")
    ctr(d,"192.168.4.1",40,1,255,"ip")
    ctr(d,"restart from the page",54,1,255,"out")
    return im

shots=[(hotspot(),"hotspot: the whole screen")]
PAD=10;LBL=16
sheet=Image.new("L",(W*S+PAD*2,H*S+LBL+PAD*2),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD; cy=PAD
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_hotspot.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered -> out_hotspot.png")
