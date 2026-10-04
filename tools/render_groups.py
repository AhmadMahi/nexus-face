"""The settings, grouped, at real pixel size."""
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
def at(d,x,y,s,size=1,c=255,tag=""):
    w=len(s)*6*size
    if x<0 or x+w>W+1: OVER.append(f"{tag}: {s!r} x={x} right={x+w}")
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,s,y,size=1,c=255,tag=""): at(d,(W-len(s)*6*size)//2,y,s,size,c,tag)
def bar(d,t): rect(d,0,0,W,11); ctr(d,t,2,1,0,"bar")

NAMES=re.findall(r'"([^"]*)"',re.search(r"const char\* C_NAME\[C_COUNT\] =\s*\{(.*?)\};",src,re.S).group(1))
ORDER=[x.strip().split(" ")[0] for x in re.split(r",",re.search(r"enum \{ (C_BRIGHT = 0,.*?)C_COUNT \};",src,re.S).group(1)) if x.strip().startswith("C_")]
GN=re.findall(r'"([^"]*)"',re.search(r"SG_NAME\[SG_COUNT\] = \{(.*?)\};",src).group(1))
GR=[[t for t in re.findall(r"C_\w+|SG_END",r) if t!="SG_END"]
    for r in re.search(r"SG_ROWS\[SG_COUNT\]\[SG_MAX\] = \{(.*?)\n\};",src,re.S).group(1).strip().split("\n")]
VALS={"Brightness":"100%","Watch face":"regulator","Sleep after":"never","Page turn":"touch",
 "Popup time":"off","Eye style":"sleepy","Prayer times":"saved","Hijri shift":"+2 d",
 "Network":"no signal","Vehicle":"on","Hotspot":"hold","Accelerometer":"hold","Knocks":"off",
 "Tap strength":"off","Go back by":"shake","Power down":"7 min","Battery full":"4.25V",
 "Pair a Mac":"paired","Check update":"hold","Auto update":"on","Reset settings":"hold",
 "Reboot":"hold","About":"hold","Wake on hold":"3s"}

def groups(sel):
    im,d=new(); bar(d,"SETTINGS")
    for g,n in enumerate(GN):
        y=14+g*12; on=(g==sel)
        if on: rect(d,0,y-2,W,12)
        c=0 if on else 255
        at(d,3,y,n,1,c,"grp"); cnt=str(len(GR[g]))
        at(d,W-3-len(cnt)*6,y,cnt,1,c,"grp")
    return im

def rows(g, sel):
    im,d=new(); bar(d,GN[g])
    n=len(GR[g]); first=max(0,min(sel-3 if sel>3 else 0, n-4)); first=max(first,0)
    for r in range(min(4,n-first)):
        i=first+r; y=14+r*12; on=(i==sel)
        if on: rect(d,0,y-2,W,12)
        c=0 if on else 255
        nm=NAMES[ORDER.index(GR[g][i])]
        at(d,3,y,nm,1,c,"row")
        v=VALS.get(nm,"hold"); at(d,W-3-len(v)*6,y,v,1,c,"row")
    return im

shots=[(groups(0),"the four"),(groups(1),"Wireless"),
       (rows(0,0),"Display, top"),(rows(0,7),"Display, end"),
       (rows(1,0),"Wireless"),(rows(3,0),"System")]
PAD=10;LBL=16;COLS=3;rws=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL)*rws+PAD*(rws+1)),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",20)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_groups.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print(f"{len(GN)} groups, {sum(len(g) for g in GR)} rows -> out_groups.png")
