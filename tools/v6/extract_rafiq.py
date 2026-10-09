import re,sys
src=open('../../nexus_face/nexus_face.ino').read()
def grab(name):
    m=re.search(r'\n(static [^\n;{]*?\b'+re.escape(name)+r'\([^)]*\)\s*\{)',src)
    assert m, name
    i=m.end()-1; d=0
    for j in range(i,len(src)):
        if src[j]=='{': d+=1
        elif src[j]=='}':
            d-=1
            if d==0: return src[m.start(1):j+1]
names=sys.argv[1:]
print('\n\n'.join(grab(n) for n in names))
