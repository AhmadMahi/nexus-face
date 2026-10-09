from gfx import OLED, W
from PIL import Image
shots=[]
def btns(o,a,b,n):
    o.rrect(0,53,62,11,3,1); o.rrect(66,53,62,11,3,1); o.col=0
    o.text((62-len(a)*6)//2,55,a,1,n); o.text(66+(62-len(b)*6)//2,55,b,1,n); o.col=1
def header(o,who,right,n):
    room=(W-6-len(right)*6-6)//6; o.titleBar(who[:room],right)
o=OLED(); header(o,'Sara Ahmed','WhatsApp','pop'); o.text(0,14,'Are you coming to'); o.text(0,24,'dinner tonight? We'); o.text(0,34,'are at 8.'); btns(o,'tap:close','hold:open','pop'); shots.append(('pop',o))
o=OLED(); header(o,'Muhammad Abdul Rahman','Mail','pop2'); o.text(0,14,'Invoice 4432 is due'); btns(o,'tap:close','hold:open','pop2'); shots.append(('pop2',o))
o=OLED(); header(o,'Sara Ahmed','2:41','read'); o.ctr('WhatsApp',14,1,'read'); o.text(0,24,'Are you coming to'); o.text(0,34,'dinner tonight?'); btns(o,'tap:2/4','hold:clear','read'); shots.append(('read',o))
print('OVERFLOWS:',[x for _,o in shots for x in o.over] or 'none')
for n,o in shots: o.png(f'63_{n}.png')
tiles=[Image.open(f'63_{n}.png') for n,_ in shots]; tw,th=tiles[0].size
sh=Image.new('RGB',(3*tw+40,th+20),(20,20,20))
for i,t in enumerate(tiles): sh.paste(t,(10+i*(tw+10),10))
sh.save('sheet63.png')
