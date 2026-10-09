from gfx import OLED, W, H
from PIL import Image
shots=[]
def shot(n,o): o.png(f'61_{n}.png'); shots.append((n,o))
HOLD=700; MAX=max(HOLD+1500,2000); BACK=MAX+1000; SS=BACK+2000; SL=SS+3000
def holdbar(held,name):
    o=OLED(); tick=-1
    if held>=SS: lab,hint,f="SWITCH OFF","keep holding",(held-SS)/(SL-SS)
    else:
        f=(held-HOLD)/(BACK-HOLD); tick=6+int(116*(MAX-HOLD)/(BACK-HOLD))
        if held<MAX: lab,hint="OPEN","let go to open"
        elif held<BACK: lab,hint="CANCEL","let go: nothing"
        else: lab,hint,f="BACK","let go to go back",1
    f=max(0,min(1,f)); o.titleBarC(lab)
    o.rrect(4,24,120,14,4,1); o.rrect(5,25,118,12,3,0)
    w=int(116*f)
    if w>2: o.rrect(6,26,w,10,3,1)
    if tick>0: o.rect(tick,20,1,3); o.rect(tick,39,1,3)
    o.ctr(hint,50,1,name); shot(name,o)
holdbar(1300,'hold_open'); holdbar(2600,'hold_cancel'); holdbar(3500,'hold_back'); holdbar(6500,'hold_switchoff')
# popup
o=OLED(); o.titleBar('WhatsApp','3'); o.ctr('Sara',15,1,'pop'); o.text(0,27,'Are you coming to'); o.text(0,35,'dinner tonight?')
o.rect(0,54,W,1); o.ctr('tap close  hold open',56,1,'pop'); shot('popup',o)
# notifications list
o=OLED(); o.titleBar('NOTIFICATIONS','4')
rows=['*WhatsApp: Sara','*Mail: Invoice 4432','Messages: Mom','Calendar: Standup']
for r,t in enumerate(rows):
    y=14+r*12
    if r==0: o.rect(0,y-2,W,12,1); o.col=0
    o.text(3,y,t[:20],1,'list'); o.col=1
o.rect(126,14,1,48); o.rect(125,14,3,9); shot('notif_list',o)
o=OLED(); o.titleBar('NOTIFICATIONS','5')
y=14+3*12; o.rect(0,y-2,W,12,1); o.col=0; o.text(3,y,'Clear all'); o.col=1
for r,t in enumerate(['Mail: Invoice 4432','Messages: Mom','Calendar: Standup']): o.text(3,14+r*12,(' '+t)[:20])
shot('notif_clear',o)
# settings
def group(title,rows,sel,name):
    o=OLED(); o.titleBar(title,'2:30')
    for r,(n,v) in enumerate(rows):
        y=14+r*12
        if r==sel: o.rect(0,y-2,W,12,1); o.col=0
        o.text(3,y,n,1,name); o.text(W-3-len(v)*6,y,v,1,name); o.col=1
    shot(name,o)
group('Controls',[('Hold time','0.7 s'),('Knocks','on'),('Tap strength','medium'),('Go back by','knock')],0,'set_controls')
group('System',[('Clock','12 hour'),('Power down','2 min'),('Battery full','4.10V'),('Tamper alarm','hold')],0,'set_system')
group('System',[('Network','bt -62 dBm'),('Uptime','3h 12m'),('Memory','118 kB'),('Battery','82%')],0,'sys_signal')
over=[x for _,o in shots for x in o.over]; print('OVERFLOWS:', over or 'none')
tiles=[Image.open(f'61_{n}.png') for n,_ in shots]; cols=3; tw,th=tiles[0].size; rows_=(len(tiles)+cols-1)//cols
sheet=Image.new('RGB',(cols*tw+(cols+1)*10, rows_*th+(rows_+1)*10),(20,20,20))
for i,t in enumerate(tiles): sheet.paste(t,(10+(i%cols)*(tw+10),10+(i//cols)*(th+10)))
sheet.save('sheet61.png'); print(len(tiles),'screens')
