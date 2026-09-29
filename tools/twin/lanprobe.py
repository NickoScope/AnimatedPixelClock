#!/usr/bin/env python3
"""lanprobe.py - does the router answer DHCP through the bridge? (ADR-TWIN-02, check A1)

Sends ONE DHCPDISCOVER through the socket_vmnet socket as the twin would: chaddr = the twin's
MAC, option 61 (client id 01:<MAC>), option 12 (a TWIN- host name), the broadcast flag. Prints
the OFFER: yiaddr, mask, router, server id, lease. Sends no REQUEST, so no lease is taken.

Framing (socket_vmnet v1.2.2 main.c:553-575): a 4-byte big-endian length and the Ethernet frame,
written in one send; the daemon floods everything, so this reads until the OFFER for our xid.

  lanprobe.py [--socket PATH] [--mac 02:54:57:49:4E:01] [--name TWIN-...] [--timeout S]
"""
import argparse, os, socket, struct, sys, time

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--socket", default=os.environ.get("TWIN_LAN_SOCKET", "/var/run/socket_vmnet.bridged.en0"))
ap.add_argument("--mac", default="02:54:57:49:4E:01")
ap.add_argument("--name", default="TWIN-NickoScopeMatrix-64x128-01")
ap.add_argument("--timeout", type=float, default=5)
a = ap.parse_args()
mac = bytes(int(x, 16) for x in a.mac.split(":"))
if not a.name.startswith("TWIN-"):
    sys.exit("--name must start with TWIN-: the router's list must not show a second panel")
xid = os.urandom(4)

bootp = bytearray(240)
bootp[0:4] = bytes([1, 1, 6, 0])                     # BOOTREQUEST, Ethernet, 6-byte address
bootp[4:8] = xid
bootp[10:12] = struct.pack("!H", 0x8000)             # broadcast flag: answer to ff:ff:ff:ff:ff:ff
bootp[28:34] = mac
bootp[236:240] = bytes([0x63, 0x82, 0x53, 0x63])
name = a.name.encode()
bootp += bytes([53, 1, 1, 61, 7, 1]) + mac + bytes([12, len(name)]) + name + bytes([55, 6, 1, 3, 6, 28, 51, 54, 255])
udp = struct.pack("!HHHH", 68, 67, 8 + len(bootp), 0) + bootp                   # UDP checksum 0: optional in IPv4
ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, bytes(4), b"\xff" * 4)
s = sum(struct.unpack("!10H", ip))
s = (s & 0xFFFF) + (s >> 16); s = (s & 0xFFFF) + (s >> 16)
ip = ip[:10] + struct.pack("!H", ~s & 0xFFFF) + ip[12:]
frame = b"\xff" * 6 + mac + b"\x08\x00" + ip + udp

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(a.socket)
sock.sendall(struct.pack("!I", len(frame)) + frame)   # header and frame in one send
print(f"DHCPDISCOVER sent: chaddr {a.mac}, client id 01:{a.mac.lower()}, host name {a.name}, xid {xid.hex()}")


def recv_exact(n):
    b = b""
    while len(b) < n:
        c = sock.recv(n - len(b))
        if not c: sys.exit("the daemon closed the connection")
        b += c
    return b


end = time.time() + a.timeout
while time.time() < end:
    sock.settimeout(max(0.05, end - time.time()))
    try:
        f = recv_exact(struct.unpack("!I", recv_exact(4))[0])
    except socket.timeout:
        break
    if len(f) < 42 or f[12:14] != b"\x08\x00" or f[23] != 17: continue
    ihl = (f[14] & 15) * 4
    u = f[14 + ihl:]
    if u[0:4] != struct.pack("!HH", 67, 68): continue
    d = u[8:]
    if len(d) < 240 or d[0] != 2 or d[4:8] != xid or d[28:34] != mac: continue
    opts, i = {}, 240
    while i + 1 < len(d) and d[i] != 255:
        if d[i] == 0: i += 1; continue
        opts[d[i]] = d[i + 2:i + 2 + d[i + 1]]; i += 2 + d[i + 1]
    if opts.get(53) != b"\x02": continue
    q = lambda b: ".".join(map(str, b)) if b else "-"
    print(f"OFFER from {q(f[26:30])}: yiaddr {q(d[16:20])}, mask {q(opts.get(1))}, router {q(opts.get(3, b'')[:4])}, "
          f"server id {q(opts.get(54))}, DNS {q(opts.get(6, b'')[:4])}, lease {struct.unpack('!I', opts[51])[0] if 51 in opts else '-'} s")
    print("no REQUEST sent: the router holds no lease for the twin")
    sys.exit(0)
sys.exit(f"no OFFER within {a.timeout} s")
