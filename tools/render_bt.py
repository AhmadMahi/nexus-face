"""The Bluetooth screens at real pixel size."""
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
def btIcon(d,cx,cy,h):
    w=(h+1)//2; m=h//2
    rect(d,cx,cy-h,1,h*2)
    line(d,(cx,cy-h),(cx+w,cy-m)); line(d,(cx+w,cy-m),(cx-w,cy+m))
    line(d,(cx,cy+h-1),(cx+w,cy+m)); line(d,(cx+w,cy+m),(cx-w,cy-m))
def offIcon(d,x,y):
    rect(d,x+3,y+1,1,6); rect(d,x+1,y+7,5,1); px(d,x+1,y+2); px(d,x+5,y+2)
    for i in range(8): px(d,x-1+i,y+i)
def robot(d,cx,cy):
    for xx in range(cx-19,cx+19):
        for yy in range(cy-15,cy+16):
            e=(abs(xx-cx)>13 and abs(yy-cy)>9)
            if not e and (abs(xx-cx)==18 or abs(yy-cy)==15): px(d,xx,yy)
    rect(d,cx,cy-21,1,6); circ(d,cx,cy-23,2,255,True)
    rect(d,cx-23,cy-2,4,1); rect(d,cx+19,cy-2,4,1)

def pairing(stage):
    im,d=new(); btIcon(d,W//2,20,7)
    ctr(d,"Allow the pairing" if stage=="linked" else "Pair me in Settings",36,1,255,"p")
    ctr(d,"Rafiq Ahmed",48,1,255,"p"); ctr(d,stage,57,1,255,"p")
    return im
def paired():
    im,d=new(); btIcon(d,6,5,3); at(d,15,2,"09:41",1,255,"h")
    at(d,W-2-3*6,2,"82%",1,255,"h")
    robot(d,W//2,27); ctr(d,"Salam, Ahmed",44,1,255,"h"); ctr(d,"touch to begin",55,1,255,"h")
    return im
def setting(v):
    im,d=new(); rect(d,0,0,W,11); ctr(d,"Wireless",2,1,0,"b")
    rows=[("Network",v),("Hotspot","hold"),("Pair a Mac","paired"),("Prayer times","saved")]
    for i,(n,val) in enumerate(rows):
        y=14+i*12
        if i==0: rect(d,0,y-2,W,12)
        c=0 if i==0 else 255
        at(d,3,y,n,1,c,"s"); at(d,W-3-len(val)*6,y,val,1,c,"s")
    return im

shots=[(pairing("pair me"),"not paired yet"),(pairing("linked"),"phone connecting"),
       (paired(),"paired, at home"),(setting("bluetooth"),"walking the modes"),
       (setting("pair me"),"the row while waiting")]
PAD=10;LBL=16;COLS=5
sheet=Image.new("L",(W*S*COLS+PAD*(COLS+1),H*S+LBL+PAD*2),40)
dd=ImageDraw.Draw(sheet);lf=ImageFont.truetype("/System/Library/Fonts/Menlo.ttc",19)
for i,(im,lab) in enumerate(shots):
    cx=PAD+i*(W*S+PAD); cy=PAD
    sheet.paste(im,(cx,cy)); dd.rectangle([cx-1,cy-1,cx+W*S,cy+H*S],outline=120)
    dd.text((cx,cy+H*S+2),lab,fill=230,font=lf)
sheet.save("out_bt.png")
if OVER: print("OVERFLOW:"); [print("  ",o) for o in OVER]; sys.exit(1)
print("rendered -> out_bt.png")
