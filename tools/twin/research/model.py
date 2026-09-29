# Faithful Python re-implementation of ESP32-HUB75-MatrixPanel-DMA 3.0.14 buffer build (S3 path),
# config: 64x64 x chain 2, depth 8, i2sspeed HZ_8M (8e6), latch_blanking 2, min_refresh 60, double_buff.
import sys
W=128; H=64; RPF=H//2; DEPTH=8; BLANK=2; I2S=8_000_000; MINREF=60
BIT_LAT=1<<6; BIT_OE=1<<7; ADDR=8
# Step 1: lsbMsbTransitionBit loop (cpp:149-176)
t=0
while True:
    ps=10**12//I2S; nsLatch=(W+0)*ps//1000; nsRow=DEPTH*nsLatch
    for i in range(t+1,DEPTH): nsRow+=(1<<(i-t-1))*nsLatch
    nsFrame=nsRow*RPF; rr=10**9//nsFrame
    print("t=%d -> %d Hz (nsRow=%d)"%(t,rr,nsRow))
    if rr>=MINREF: break
    if t<DEPTH-1: t+=1
    else: break
calc_rr=rr
# Step 4: descriptor order per row: all planes once, then plane i repeated 2^(i-t-1) for i>t
seq=list(range(DEPTH))
for i in range(t+1,DEPTH): seq+= [i]*(1<<(i-t-1))
print("t=",t,"segments/row",len(seq),"seq",seq)
# buffers: row -> plane -> W words
def clear_fb():
    fb=[[[0]*W for _ in range(DEPTH)] for _ in range(RPF)]
    for r in range(RPF):
        for p in range(DEPTH):
            a = r if p>0 else (RPF-1 if r==0 else r-1)
            for x in range(W): fb[r][p][x]=a<<ADDR
        for p in range(DEPTH):
            fb[r][p][W-1]|=BIT_LAT
            b=BLANK
            while b:
                b-=1
                fb[r][p][0+b]|=BIT_OE; fb[r][p][W-1]|=BIT_OE; fb[r][p][W-b-1]|=BIT_OE
    return fb
def set_brightness(fb,brt):
    for r in range(RPF):
        for c in range(DEPTH):
            bitplane=(2*DEPTH-c)%DEPTH
            bitshift=(DEPTH-t-1)>>1
            rs=max(bitplane-bitshift-2,0)
            mx=(W-BLANK)>>rs
            n=(mx*brt)>>8
            if brt>0 and n==0: n=1
            if n>mx-1: n=mx-1
            xmax=(W+n+1)>>1; xmin=(W-n)>>1
            for x in range(W):
                if xmin<=x<xmax: fb[r][c][x]&=~BIT_OE & 0xFFFF
                else: fb[r][c][x]|=BIT_OE
def oe_counts(fb):
    return [sum(1 for x in range(W) if not fb[0][c][x]&BIT_OE) for c in range(DEPTH)]
# generic HUB75 stream decoder
def decode(fb, frames=2):
    stream=[]
    for r in range(RPF):
        for p in seq: stream+=fb[r][p]
    acc=[[[0,0,0] for _ in range(W)] for _ in range(H)]
    shift=[0]*W; latched=[0]*W
    total=0
    for f in range(frames):
        for w in stream:
            # clock in 6 colour bits; first word clocked ends at the far end -> index 0 after W clocks
            shift.pop(0); shift.append(w&0x3F)
            if w&BIT_LAT: latched=shift[:]
            if f==frames-1:
                total+=1
                if not (w&BIT_OE):
                    a=(w>>ADDR)&0x1F
                    for x in range(W):
                        v=latched[x]
                        for ch in range(3):
                            if v>>ch&1: acc[a][x][ch]+=1
                            if v>>(3+ch)&1: acc[a+RPF][x][ch]+=1
    return acc,total
def paint(fb,x,y,r,g,b,lut):
    rv,gv,bv=lut[r],lut[g],lut[b]
    off=0; clr=0b1111111111111000
    if y>=RPF: off=3; clr=0b1111111111000111; y-=RPF
    for c in range(DEPTH):
        m=1<<c; bits=((bv&m)!=0)<<2 | ((gv&m)!=0)<<1 | ((rv&m)!=0)
        fb[y][c][x]=(fb[y][c][x]&clr)|(bits<<off)
if __name__=="__main__":
    import re
    src=open("/Users/apple/AnimatedPixelClock-netbroker/.pio/libdeps/matrix-waveshare-rgb/ESP32 HUB75 LED MATRIX PANEL DMA Display/src/cie_luts.h").read()
    lut=[int(x) for x in re.search(r"lumConvTab_8bit\[256\] = \{(.*?)\};",src,re.S).group(1).replace("\n"," ").split(",") if x.strip()]
    for brt in [255,128,64,32,16,8,4,1]:
        fb=clear_fb(); set_brightness(fb,brt)
        print("brt",brt,"OE-low words per plane-memory c=0..7:",oe_counts(fb))
    brt=int(sys.argv[1]) if len(sys.argv)>1 else 128
    fb=clear_fb(); set_brightness(fb,brt)
    # paint red channel test: pixel x = code value k (0..127) on row 5 (upper) and x=k with value k+128 on row 40 (lower)
    for x in range(W):
        paint(fb,x,5,x,0,0,lut)
        paint(fb,x,40,x+128,0,0,lut)
    paint(fb,0,0,255,255,255,lut)
    acc,total=decode(fb)
    print("words/frame",total,"calc_refresh(8MHz formula)",calc_rr,"real@10MHz %.2f Hz"%(10e6/total), "@8MHz %.2f"%(8e6/total))
    print("white pixel(0,0) on-clocks R,G,B",acc[0][0],"duty %.5f"%(acc[0][0][0]/total))
    # per-plane effective weight: measure single-bit codes
    wts=[]
    for p in range(DEPTH):
        fb2=clear_fb(); set_brightness(fb2,brt)
        for c in range(DEPTH):
            pass
        # set only plane p bit for red at (10,10) directly
        fb2[10][p][10]|=1
        a,_=decode(fb2)
        wts.append(a[10][10][0])
    print("brt",brt,"on-clocks per bit plane 0..7:",wts, " ideal-binary ratio vs plane7:",[round(w/wts[7]*128,2) for w in wts])
    # neighbour-row leakage check
    print("row 9/11 leakage at x=10 (should be 0):")
