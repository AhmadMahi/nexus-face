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

def bell(d,cx,cy,h):
    w=h*3//4
    for i in range(h):
        half=w*(i+2)//(h+2)
        rect(d,cx-half,cy-h//2+i,half*2+1,1)
    rect(d,cx-w-2,cy+h//2,w*2+5,1)
    px(d,cx,cy-h//2-1)
    circ(d,cx,cy+h//2+2,1,255,True)

def titleBar(d,title,right):
    rect(d,0,0,W,11)
    at(d,3,2,title,1,0,"ribbon-l")
    at(d,W-3-len(right)*6,2,right,1,0,"ribbon-r")

def corner(n,un,hint):
    im,d=new(); titleBar(d,"NOTICES","09:41")
    bell(d,W//2,31,11)
    if not n: lab="Nothing yet"
    elif un:  lab=f"{un} new of {n}"
    else:     lab=f"{n} kept"
    ctr(d,lab,45,1,255,"count"); ctr(d,hint,56,1,255,"hint")
    return im

def reader(app,when,title,msg,pos,total):
    im,d=new(); titleBar(d,app,when)
    ctr(d,title,15,1,255,"title")
    # fitText wraps at 21 chars a line
    lines=[]; cur=""
    for word in msg.split():
        if len(cur)+len(word)+1>21: lines.append(cur); cur=word
        else: cur=(cur+" "+word).strip()
    if cur: lines.append(cur)
    for i,l in enumerate(lines[:3]): ctr(d,l,27+i*9,1,255,"body")
    ctr(d,f"{pos}/{total}  hold clears",56,1,255,"foot")
    return im

def call(kind,who,pos,total):
    im,d=new(); titleBar(d,"Phone","09:41")
    ctr(d,kind,20,1,255,"kind"); ctr(d,who,34,1,255,"who")
    ctr(d,f"{pos}/{total}  hold clears",56,1,255,"foot")
    return im

def tail():
    im,d=new(); titleBar(d,"NOTICES","09:41")
    ctr(d,"That is all",26,1,255,"t"); ctr(d,"hold to clear them",44,1,255,"t")
    return im

def confirm(yes):
    im,d=new(); titleBar(d,"NOTICES","09:41")
    ctr(d,"Clear them all?",26,1,255,"c")
    ctr(d,"> YES    no" if yes else "  yes  > NO",44,1,255,"c")
    return im

shots=[(corner(0,0,"from your phone"),"nothing yet"),
       (corner(7,3,"hold to read"),"three unread"),
       (reader("Messages","09:41","Ammi","Are you coming home for dinner tonight?",1,7),"reading one"),
       (call("Calling","+91 98765 43210",2,7),"a call, the number"),
       (call("Missed call","Alok Kumar",3,7),"a missed call"),
       (tail(),"past the last"),
       (confirm(False),"clear them all")]
PAD=10;LBL=16;COLS=4
rows=(len(shots)+COLS-1)//COLS
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),(H*S+LBL+PAD)*rows+PAD),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+(i%COLS)*(W*S+PAD); cy=PAD+(i//COLS)*(H*S+LBL+PAD)
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_notes.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered -> out_notes.png")
