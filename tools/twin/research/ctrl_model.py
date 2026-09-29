# Transcription of src/control/control.cpp sampleTick() (rotation + switch), 1 kHz.
TAB=[0,-1,0,0, 1,0,0,0, 0,0,0,1, 0,0,-1,0]
class Ctrl:
    def __init__(s, phys_ab_idle=(0,0), active_high=True, stable=2, lockout=10, debounce=40, half=-1):
        s.ah=active_high; s.stable=stable; s.lockout=lockout; s.deb=debounce; s.half=half
        ab=s.encAB(*phys_ab_idle)
        s.shown=s.cand=ab; s.run=0; s.prev=ab; s.since=0; s.last=ab*5; s.dir=0; s.lastStep=-10**9
        s.swRaw=False; s.swRawSince=0; s.swDown=False; s.swDownAt=0; s.longSent=False; s.ev=[]
    def encAB(s,A,B):
        a=(1-A) if s.ah else A; b=(1-B) if s.ah else B
        return (b<<1)|a
    def filt(s,raw):
        if raw==s.shown: s.cand=raw; s.run=0; return s.shown
        if raw!=s.cand: s.cand=raw; s.run=1
        elif s.run<255: s.run+=1
        if s.run>=s.stable: s.shown=raw; s.run=0
        return s.shown
    def tick(s,now,A,B,gpio0):
        ab=s.filt(s.encAB(A,B))
        if ab!=s.prev: s.prev=ab; s.since=now
        if s.half<0 and ab==0 and now-s.since>=250: s.half=1
        if now-s.lastStep<s.lockout: s.dir=0; s.last=ab*5
        else:
            s.last=(s.last>>2)|(ab<<2); s.dir+=TAB[s.last&15]
        atDet=(ab==3) or (s.half==1 and ab==0)
        if s.dir!=0 and atDet:
            s.lastStep=now; fwd=s.dir>0; s.dir=0; s.last=ab*5; s.ev.append((now,'CW' if fwd else 'CCW'))
        raw=(gpio0==0)
        if raw!=s.swRaw: s.swRaw=raw; s.swRawSince=now
        if raw!=s.swDown and now-s.swRawSince>=s.deb:
            if raw: s.swDownAt=now; s.longSent=False
            elif (not s.longSent) and now-s.swDownAt<500: s.ev.append((now,'PRESS'))
            s.swDown=raw
        if s.swDown and not s.longSent and now-s.swDownAt>=1000:
            s.longSent=True; s.ev.append((now,'LONG'))

def run(wave_ab, wave_sw, T, **kw):
    """wave_*: list of (t_ms, level...) breakpoints; sample at integer ms (+0.5 phase)."""
    c=Ctrl(**kw)
    def lvl(w,t):
        v=w[0][1:]
        for p in w:
            if p[0]<=t: v=p[1:]
        return v
    for ms in range(1,T):
        t=ms+0.5
        A,B=lvl(wave_ab,t); (g,)=lvl(wave_sw,t)
        c.tick(ms,A,B,g)
    return c.ev, c.half

# 1) Board wiring (physical idle LOW LOW, closed = HIGH): full cycle per detent, A closes first.
st=3
def cycle(t0,cw=True):
    seq=[(1,0),(1,1),(0,1),(0,0)] if cw else [(0,1),(1,1),(1,0),(0,0)]
    return [(t0+i*st,)+p for i,p in enumerate(seq)]
w=[(0,0,0)]
t=100
for k in range(3): w+=cycle(t,True); t+=st*4+10
for k in range(2): w+=cycle(t,False); t+=st*4+10
print('board wiring, 3 CW + 2 CCW full cycles, 3 ms/state, 10 ms gap:', run(w,[(0,1)],t+300))
# 2) state 1 ms only -> filtered?
st=1; w=[(0,0,0)]+cycle(100,True)
print('1 ms per state:', run(w,[(0,1)],400))
st=2; w=[(0,0,0)]+cycle(100,True)
print('2 ms per state:', run(w,[(0,1)],400))
# 3) gap too short after a step (lockout 10 ms)
st=3; w=[(0,0,0)]+cycle(100,True)+cycle(112,True)
print('two detents, second starts 0 ms after first ends:', run(w,[(0,1)],400))
# 4) half-cycle knob (rests at 11 and 00 in logical terms)
st=3; w=[(0,0,0),(100,1,0),(103,1,1),(600,0,1),(603,0,0),(1100,1,0),(1103,1,1)]
print('half-detent knob, 3 clicks 500ms apart:', run(w,[(0,1)],1500))
# 5) Wokwi KY-040 style: pull-ups, idle HIGH HIGH, CW = CLK low then DT low, both back high
w=[(0,1,1),(100,0,1),(103,0,0),(106,1,0),(109,1,1)]
print('KY-040 polarity on unmodified firmware (CLK->A):', run(w,[(0,1)],800))
# 6) Switch: NEC data frame on GPIO0 (marks low) must not click; 250 ms low -> PRESS; 1100 ms -> LONG
def nec(t0,val,repeat_n=0):
    ev=[]; t=t0
    def mark(d):
        nonlocal t; ev.append((t,0)); t+=d
    def space(d):
        nonlocal t; ev.append((t,1)); t+=d
    start=t; mark(9.0); space(4.5)
    for i in range(31,-1,-1):
        mark(0.56); space(1.69 if (val>>i)&1 else 0.56)
    mark(0.56); space(max(0,108-(t-start)))
    for r in range(repeat_n):
        start=t; mark(9.0); space(2.25); mark(0.56); space(108-(t-start))
    return ev
sw=[(0,1)]+nec(100,0x00FF708F,repeat_n=10)
print('NEC frame + 10 repeats on GPIO0 -> knob switch events:', run([(0,0,0)],sw,2000))
print('GPIO0 low 250 ms:', run([(0,0,0)],[(0,1),(100,0),(350,1)],900))
print('GPIO0 low 30 ms:', run([(0,0,0)],[(0,1),(100,0),(130,1)],900))
print('GPIO0 low 1100 ms:', run([(0,0,0)],[(0,1),(100,0),(1200,1)],1800))
print('GPIO0 stuck low from boot (QEMU stub GPIO_IN=0):', run([(0,0,0)],[(0,0)],5000))
