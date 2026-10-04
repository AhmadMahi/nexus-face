import math
from PIL import Image, ImageDraw, ImageFont
W,H,S = 128,64,6
FNT = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(7*S))
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
def hline(d,x,y,w,c=255): rect(d,x,y,w,1,c)
def circ(d,cx,cy,r,c=255):
    for a in range(0,360,3):
        px(d,cx+int(r*math.cos(math.radians(a))),cy+int(r*math.sin(math.radians(a))),c)
def at(d,x,y,s,c=255,size=1):
    f=fnt(size)
    for i,ch in enumerate(s): d.text(((x+i*6*size)*S,y*S),ch,fill=c,font=f)
def ctr(d,y,s,size=1,c=255): at(d,max(0,(W-len(s)*6*size)//2),y,s,c,size)
def bar(d,t): rect(d,0,0,W,11); at(d,max(0,(W-len(t)*6)//2),2,t,0)

shots=[]; labels=[]

# --- reminders summary ---
im,d=new(); bar(d,"REMINDERS")
ctr(d,20,"3 waiting",2); ctr(d,44,"next at 09:00"); ctr(d,55,"hold to read")
shots.append(im); labels.append("summary")

# --- a reminder, watch style ---
im,d=new()
rect(d,0,0,W,11); at(d,3,2,"09:00  Thu 02 Oct",0)
txt="Call the clinic about the appointment and move it"
CW=21; pos=0; line=0
while pos<len(txt) and line<4:
    take=min(CW,len(txt)-pos)
    if pos+take<len(txt):
        sp=take
        while sp>0 and txt[pos+sp]!=' ': sp-=1
        if sp>4: take=sp
    at(d,3,16+line*11,txt[pos:pos+take]); pos+=take
    while pos<len(txt) and txt[pos]==' ': pos+=1
    line+=1
shots.append(im); labels.append("a reminder")

# --- clear them all ---
im,d=new(); bar(d,"CLEAR THEM ALL")
ctr(d,20,"Throw away every"); ctr(d,31,"reminder?")
# yesNo
for i,(lb,on) in enumerate([("YES",False),("NO",True)]):
    x=28+i*40
    if on: rect(d,x-4,41,30,12); at(d,x+2,43,lb,0)
    else:  box(d,x-4,41,30,12); at(d,x+2,43,lb)
ctr(d,55,"1 moves  hold confirms")
shots.append(im); labels.append("clear all")

# --- the hold words ---
im,d=new(); bar(d,"SETTINGS")
for i,(r,v) in enumerate([("Brightness","75%"),("Watch face","dial"),("Knocks","off")]):
    y=14+i*12; at(d,3,y,r); at(d,W-3-len(v)*6,y,v)
w="KEEP HOLDING: SLEEP"; bw=len(w)*6+6; bx=(W-bw)//2
rect(d,bx,26,bw,13,0); box(d,bx,26,bw,13); at(d,bx+3,29,w)
shots.append(im); labels.append("held 10s")

# --- going to sleep ---
im,d=new()
circ(d,40,34,5); circ(d,88,34,5)
ctr(d,8,"going to sleep")
for z in range(3): at(d,100+z*5,24-z*7,"z")
ctr(d,54,"touch to wake me")
shots.append(im); labels.append("goodnight")

# --- the name card ---
im,d=new()
at(d,(W-5*12)//2,16,"RAFIQ",size=2)
hline(d,30,36,W-60)
ctr(d,41,"your companion"); ctr(d,54,"developed by Ahmed")
shots.append(im); labels.append("boot")

sheet=Image.new("L",(W*S*3+40,(H*S+26)*2+10),70)
dd=ImageDraw.Draw(sheet)
for i,(im,lb) in enumerate(zip(shots,labels)):
    x=10+(i%3)*(W*S+10); y=10+(i//3)*(H*S+26)
    sheet.paste(im,(x,y)); dd.text((x+4,y+H*S+4),lb,fill=255)
sheet.save("/tmp/v4.png")
print("ok")
