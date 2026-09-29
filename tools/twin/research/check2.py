from model import *
import re
src=open("/Users/apple/AnimatedPixelClock-netbroker/.pio/libdeps/matrix-waveshare-rgb/ESP32 HUB75 LED MATRIX PANEL DMA Display/src/cie_luts.h").read()
lut=[int(x) for x in re.search(r"lumConvTab_8bit\[256\] = \{(.*?)\};",src,re.S).group(1).replace("\n"," ").split(",") if x.strip()]
fb=clear_fb(); set_brightness(fb,255)
paint(fb,10,10,255,0,0,lut); paint(fb,20,42,0,255,0,lut); paint(fb,0,0,0,0,255,lut); paint(fb,127,63,255,255,255,lut)
paint(fb,5,31,255,0,0,lut); paint(fb,6,32,0,0,255,lut)
acc,total=decode(fb)
lit=[(y,x,acc[y][x]) for y in range(H) for x in range(W) if any(acc[y][x])]
print("frame words",total); print(lit)
# full-stream gradient: input codes 186..200 red on row 3
fb=clear_fb(); set_brightness(fb,255)
for i,v in enumerate(range(186,201)): paint(fb,i,3,v,0,0,lut)
acc,_=decode(fb)
print([(v,lut[v],acc[3][i][0]) for i,v in enumerate(range(186,201))])
