import re, math
from PIL import Image, ImageDraw, ImageFont
SRC = open("nexus-repo/nexus_face/nexus_face.ino").read()
GL  = open("nexus-repo/nexus_face/arabic_glyphs.h").read()
W,H,S = 128,64,6
F = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S))
def fnt(s): return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S*s))
def new(): im=Image.new("L",(W*S,H*S),0); return im, ImageDraw.Draw(im)
def px(d,x,y,c=255):
    if 0<=x<W and 0<=y<H: d.rectangle([x*S,y*S,(x+1)*S-1,(y+1)*S-1],fill=c)
def rect(d,x,y,w,h,c=255):
    for yy in range(y,y+h):
        for xx in range(x,x+w): px(d,xx,yy,c)
def circf(d,cx,cy,r,c=255):
    for yy in range(cy-r,cy+r+1):
        for xx in range(cx-r,cx+r+1):
            if (xx-cx)**2+(yy-cy)**2 <= r*r: px(d,xx,yy,c)
def at(d,x,y,s,size=1,c=255):
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,y,s,size=1): at(d,max(0,(W-len(s)*6*size)//2),y,s,size)

def glyphs(name):
    w=int(re.search(rf"#define {name}_W (\d+)",GL).group(1))
    h=int(re.search(rf"#define {name}_H (\d+)",GL).group(1))
    body=re.search(rf"const uint8_t {name}\[\d+\]\[\d+\] PROGMEM = \{{(.*?)\n\}};",GL,re.S).group(1)
    out=[]
    for r in re.findall(r"\{([^}]*)\}",body):
        data=[int(x,16) for x in re.findall(r"0x([0-9A-F]{2})",r)]
        out.append((w,h,data))
    return out
BIG, SML, MON = glyphs("AR_BIG"), glyphs("AR_SMALL"), glyphs("AR_MONTH")
def blit(d,x,y,g):
    w,h,data=g; stride=(w+7)//8
    for yy in range(h):
        for xx in range(w):
            if data[yy*stride+(xx>>3)] & (0x80>>(xx&7)): px(d,x+xx,y+yy)
def arNum(d,x,y,v,n,big):
    gs = BIG if big else SML; wdt = gs[0][0]
    for i in range(n-1,-1,-1): blit(d,x+i*wdt,y,gs[v%10]); v//=10
def slide(d,y,a,b,t):
    Hh,PER,SL = 9,2000,320
    tt = t % (PER*2); second = tt>=PER; into = tt-PER if second else tt
    now_, was = (b,a) if second else (a,b)
    off = int(into*Hh/SL) if into<SL else Hh
    if off<Hh: ctr(d,y-off,was); ctr(d,y+Hh-off,now_)
    else: ctr(d,y,now_)
    rect(d,0,y-Hh,W,Hh,0); rect(d,0,y+8,W,Hh,0)

Hh, Hm, Hd = 1448, 4, 19
shots=[]; labels=[]

# arabic
im,d=new()
DW=BIG[0][0]; GAP=7; w=DW*4+GAP; x=(W-w)//2; y=4
arNum(d,x,y,14,2,True); arNum(d,x+DW*2+GAP,y,32,2,True)
cx=x+DW*2+GAP//2
rect(d,cx-1,y+7,2,2); rect(d,cx-1,y+15,2,2)
arNum(d,3,30,Hd,2,False)
arNum(d,W-3-4*SML[0][0],30,Hh,4,False)
blit(d,(W-MON[0][0])//2,45,MON[Hm-1])
shots.append(im); labels.append("arabic")

# hijri, at three moments of the slide
LAT=["Muharram","Safar","Rabi I","Rabi II","Jumada I","Jumada II","Rajab",
     "Shaban","Ramadan","Shawwal","Dhul Qadah","Dhul Hijjah"]
for t,lb in ((300,"hijri (gregorian)"),(2150,"hijri (mid slide)"),(3000,"hijri (hijri)")):
    im,d=new()
    at(d,(W-5*18)//2,4,"14:32",3)
    slide(d,44,"02 Oct 2026",f"{Hd} {LAT[Hm-1]} {Hh}",t)
    shots.append(im); labels.append(lb)

# crescent
im,d=new()
circf(d,22,20,13); circf(d,28,18,13,0)
at(d,46,12,"14:32",2); at(d,46+5*12+2,19,"07")
arNum(d,3,39,Hd,2,False)
blit(d,W-2-MON[0][0],37,MON[Hm-1])
at(d,3,55,str(Hh)); at(d,W-3-11*6,55,"02 Sep 2026")
shots.append(im); labels.append("crescent")

cols=3; rows=(len(shots)+cols-1)//cols
sheet=Image.new("L",(cols*(W*S+10)+10,rows*(H*S+26)+10),70)
dd=ImageDraw.Draw(sheet)
for i,(im,lb) in enumerate(zip(shots,labels)):
    xx=10+(i%cols)*(W*S+10); yy=10+(i//cols)*(H*S+26)
    sheet.paste(im,(xx,yy)); dd.text((xx+4,yy+H*S+4),lb,fill=255)
sheet.save("/tmp/arfaces.png"); print("ok")
