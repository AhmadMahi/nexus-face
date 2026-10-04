import math, re
from PIL import Image, ImageDraw, ImageFont
SRC=open("nexus-repo/nexus_face/nexus_face.ino").read()
W,H,S=128,64,6
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
    for a,b in ((x,y),(x+w-1,y),(x,y+h-1),(x+w-1,y+h-1)): px(d,a,b,0)
def hline(d,x,y,w,c=255): rect(d,x,y,w,1,c)
def vline(d,x,y,h,c=255): rect(d,x,y,1,h,c)
def circ(d,cx,cy,r,c=255,fill=False):
    for yy in range(cy-r-1,cy+r+2):
        for xx in range(cx-r-1,cx+r+2):
            dd=math.hypot(xx-cx,yy-cy)
            if (dd<=r) if fill else (abs(dd-r)<0.7): px(d,xx,yy,c)
def line(d,x0,y0,x1,y1):
    n=max(abs(x1-x0),abs(y1-y0),1)
    for i in range(n+1): px(d,round(x0+(x1-x0)*i/n),round(y0+(y1-y0)*i/n))
def at(d,x,y,s,size=1):
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=255,font=f)
def ctr(d,y,s,size=1): at(d,max(0,(W-len(s)*6*size)//2),y,s,size)
def bar(d,t): rect(d,0,0,W,11); 
def barT(d,t):
    rect(d,0,0,W,11)
    f=fnt(1); x=max(0,(W-len(t)*6)//2)
    for i,ch in enumerate(t): d.text(((x+i*6)*S,2*S),ch,fill=0,font=f)
def bikeIcon(d,x,y):
    circ(d,x+5,y+11,4); circ(d,x+27,y+11,4)
    px(d,x+5,y+11); px(d,x+27,y+11)
    line(d,x+5,y+11,x+13,y+4); line(d,x+13,y+4,x+22,y+5)
    line(d,x+22,y+5,x+27,y+11); line(d,x+13,y+4,x+16,y+11)
    line(d,x+16,y+11,x+27,y+11)
    rect(d,x+10,y+2,7,2); rect(d,x+20,y+1,2,4); rect(d,x+17,y+6,5,3)

PLATE=re.search(r'char bikePlate\[\d+\] = "([^"]*)"',SRC).group(1)
MODEL=re.search(r'char bikeModel\[\d+\] = "([^"]*)"',SRC).group(1)
OWNER=re.search(r'char bikeOwner\[\d+\] = "([^"]*)"',SRC).group(1)
shots=[];labels=[]

# tpl 0: big plate, split on the second space
im,d=new()
p=PLATE.split(" ")
top=" ".join(p[:2]); bot=" ".join(p[2:])
plateBox(d,6,4,W-12,38 if bot else 24)
if bot: ctr(d,9,top,2); ctr(d,26,bot,2)
else:   ctr(d,11,top,2)
ctr(d,50,f"{MODEL}  {OWNER}")
shots.append(im); labels.append("0  plate (default)")

# tpl 1: badge
im,d=new()
bikeIcon(d,3,6); vline(d,42,4,56)
at(d,48,6,PLATE); hline(d,48,16,W-52)
at(d,48,20,MODEL); at(d,48,31,OWNER); at(d,48,45,"India")
shots.append(im); labels.append("1  badge")

# tpl 2: ticket
im,d=new()
barT(d,"VEHICLE")
for i,(k,v) in enumerate((("REG",PLATE),("MODEL",MODEL),("OWNER",OWNER))):
    y=16+i*13; at(d,4,y,k); at(d,W-4-len(v)*6,y,v)
    if i<2:
        for x in range(4,W-4,3): px(d,x,y+9)
shots.append(im); labels.append("2  ticket")

# tpl 3: dial
im,d=new()
CX,CY,R=64,46,34
for a in range(180,361,2): px(d,CX+int(R*math.cos(math.radians(a))),CY+int(R*math.sin(math.radians(a))))
for i in range(7):
    a=math.radians(180+30*i)
    for r in range(R-4,R+1): px(d,CX+int(r*math.cos(a)),CY+int(r*math.sin(a)))
a=math.radians(180+30*4.2); line(d,CX,CY,CX+int(27*math.cos(a)),CY+int(27*math.sin(a)))
circ(d,CX,CY,2,fill=True)
ctr(d,2,MODEL); ctr(d,50,PLATE)
shots.append(im); labels.append("3  dial")

# offline home
im,d=new()
circ(d,64,24,11); circ(d,60,22,2,fill=True); circ(d,68,22,2,fill=True)
for a in range(20,161,6): px(d,64+int(5*math.cos(math.radians(a))),26+int(5*math.sin(math.radians(a))))
ctr(d,40,"Salam, Ahmed"); ctr(d,54,"touch to begin")
at(d,2,2,"14:32"); at(d,W-2-3*6,2,"78%")
shots.append(im); labels.append("offline home")

cols=3; rows=(len(shots)+cols-1)//cols
sheet=Image.new("L",(cols*(W*S+10)+10,rows*(H*S+26)+10),70)
dd=ImageDraw.Draw(sheet)
for i,(im,lb) in enumerate(zip(shots,labels)):
    x=10+(i%cols)*(W*S+10); y=10+(i//cols)*(H*S+26)
    sheet.paste(im,(x,y)); dd.text((x+4,y+H*S+5),lb,fill=255)
sheet.save("/tmp/v5.png"); print("plate:",PLATE)
