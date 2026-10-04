"""The settings list and the boot card, at real pixel size, with the
names and the longest value each row can show taken from the source."""
import re, sys
from PIL import Image, ImageDraw, ImageFont
src=open("nexus-repo/nexus_face/nexus_face.ino").read()
W,H,S=128,64,6; OVER=[]
def fnt(s): return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S*s))
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def rect(d,x,y,w,h,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w): px(d,xx,yy,c)
def hline(d,x,y,w,c=255): rect(d,x,y,w,1,c)
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def titleBar(d,t,r):
    rect(d,0,0,W,11); ctr(d,t,2,1,0,"tb")
    if r: at(d,W-2-len(r)*6,2,r,1,0,"tb")

NAMES = re.findall(r'"([^"]*)"',
         re.search(r"const char\* C_NAME\[C_COUNT\] =\s*\{(.*?)\};", src, re.S).group(1))
VALS = {"Brightness":"100%","Watch face":"regulator","Sleep after":"never",
        "Page turn":"touch","Popup time":"off","Eye style":"sleepy",
        "Prayer times":"saved","Hijri shift":"+2 d","Network":"no signal",
        "Vehicle":"on","Hotspot":"hold","Accelerometer":"hold","Knocks":"off",
        "Tap strength":"off","Go back by":"shake","Power down":"7 min",
        "Battery full":"4.25V","Pair a Mac":"paired","Check update":"hold",
        "Auto update":"on","Reset settings":"hold","Reboot":"hold","About":"hold"}

def page(first, sel):
    im,d=new(); titleBar(d,"SETTINGS",f"{sel+1}/{len(NAMES)}")
    for k in range(4):
        i=first+k
        if i>=len(NAMES): break
        y=14+k*12; on=(i==sel)
        if on: rect(d,0,y-2,W,12)
        c=0 if on else 255
        at(d,3,y,NAMES[i],1,c,"row")
        v=VALS.get(NAMES[i],"hold")
        at(d,W-3-len(v)*6,y,v,1,c,"row")
    return im

def bootcard():
    im,d=new(); rect(d,0,0,W,11); ctr(d,"HOW TO USE ME",2,1,0,"boot")
    at(d,8,16,"1  next",1,255,"boot"); at(d,8,28,"2  back",1,255,"boot")
    at(d,66,16,"hold  open",1,255,"boot"); at(d,66,28,"5s  home",1,255,"boot")
    hline(d,8,40,112); ctr(d,"Touch the pad",48,1,255,"boot")
    return im

shots=[(page(0,0),"settings 1"),(page(4,6),"settings 2"),
       (page(12,14),"Go back by"),(page(19,22),"settings end"),
       (bootcard(),"the card on a cold boot")]
PAD=10;LBL=16;COLS=3;rows=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL)*rows+PAD*(rows+1)),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",20)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_set.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print(f"{len(NAMES)} rows rendered, nothing off the edge -> out_set.png")
