import re
from PIL import Image, ImageDraw, ImageFont
SRC = open("nexus-repo/nexus_face/nexus_face.ino").read()
LY = [int(x) for x in re.search(r"const int LY\[5\] = \{([^}]*)\}", SRC).group(1).split(",")]
LB = re.findall(r'"([^"]*)"', re.search(r"const char\* LB\[5\] = \{([^}]*)\}", SRC).group(1))
IPY = int(re.search(r"ctr\(ip\.c_str\(\), (\d+), 1\);", SRC).group(1))
FW = re.search(r'#define FW_VERSION "([^"]+)"', SRC).group(1)
W,H,S = 128,64,8
F = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 7)
def mk(vals, ip):
    im = Image.new("1",(W,H),0); d = ImageDraw.Draw(im)
    d.rectangle([0,0,W-1,10], fill=1)
    d.text((3,2),"SYSTEM",fill=0,font=F)
    d.text((W-2-len(FW)*6,2),FW,fill=0,font=F)
    for y,lb,v in zip(LY,LB,vals):
        d.text((4,y),lb,fill=1,font=F)
        d.text((W-4-len(v)*6,y),v,fill=1,font=F)
    d.text((max(0,(W-len(ip)*6)//2),IPY),ip,fill=1,font=F)
    return im
shots = [
  mk(["3h12m","-52 dBm","241 kB","-- 830 +k","78% 3.91V"], "192.168.1.31"),
  mk(["12d4h","offline","198 kB","ON 999M","none"],         "no address"),
  mk(["45m","hotspot","240 kB","-- 0","100% 4.20V"],        "192.168.4.1"),
]
sheet = Image.new("L",(W*S*len(shots)+10*(len(shots)+1), H*S+20),70)
for i,im in enumerate(shots):
    sheet.paste(im.convert("L").resize((W*S,H*S),Image.NEAREST),(10+i*(W*S+10),10))
sheet.save("/tmp/sysrow.png")
print("rows",LY,"ip",IPY)
