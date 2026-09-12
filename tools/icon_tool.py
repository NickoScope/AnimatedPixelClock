#!/usr/bin/env python3
"""Make and publish 16x16 card icons.

  python3 tools/icon_tool.py from-png bell.png bell.i16
  python3 tools/icon_tool.py publish bell.i16 bell        # to the live broker

The wire format is what the firmware reads: 512 bytes, RGB565, big-endian,
row-major. Big-endian because that is the order a colour is written down.
"""
import os, pathlib, struct, sys

W = H = 16

def to565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def from_png(src, dst):
    from PIL import Image
    im = Image.open(src).convert("RGB").resize((W, H), Image.NEAREST)
    out = bytearray()
    for y in range(H):
        for x in range(W):
            out += struct.pack(">H", to565(*im.getpixel((x, y))))
    pathlib.Path(dst).write_bytes(out)
    print(f"{dst}: {len(out)} bytes")

def publish(src, name):
    import socket
    data = pathlib.Path(src).read_bytes()
    if len(data) != W * H * 2:
        sys.exit(f"{src} is {len(data)} bytes, need {W*H*2}")
    env = lambda k: os.popen(
        f"bash -c 'set -a; . ~/.config/nickoscope/bridge.env; set +a; echo ${k}'").read().strip()
    host, user, pw = env("MQ_HOST") or "192.168.4.35", env("MQ_USER"), env("MQ_PASS")
    # MQTT remaining length: seven bits per byte, continuation bit on the rest.
    def enc(n):
        out = b""
        while True:
            d = n % 128; n //= 128
            out += bytes([d | 0x80]) if n else bytes([d])
            if not n: return out
    s16 = lambda x: struct.pack("!H", len(x.encode())) + x.encode()
    sk = socket.create_connection((host, 1883), 5); sk.settimeout(8)
    p = s16("MQTT") + bytes([4, 0xC2 if user else 0x02]) + struct.pack("!H", 30) + s16("icontool")
    if user: p += s16(user) + s16(pw)
    sk.sendall(bytes([0x10]) + enc(len(p)) + p)
    if sk.recv(4)[3] != 0: sys.exit("broker refused the connection")
    topic = f"nickoscope_matrix/icon/{name}"
    pk = s16(topic) + data
    sk.sendall(bytes([0x31]) + enc(len(pk)) + pk)          # retained
    print(f"{topic}: {len(data)} bytes, retained")
    sk.close()

if len(sys.argv) < 2: sys.exit(__doc__)
if sys.argv[1] == "from-png": from_png(sys.argv[2], sys.argv[3])
elif sys.argv[1] == "publish": publish(sys.argv[2], sys.argv[3])
else: sys.exit(__doc__)
