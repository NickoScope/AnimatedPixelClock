#!/usr/bin/env python3
"""fake_vmnet.py - a stand-in for socket_vmnet (bridged) and the home router behind it, for
testing esp32sim --net bridge:PATH and twin.py --lan without root and without the LAN.
Framing as socket_vmnet v1.2.2 main.c: a 4-byte big-endian length and the Ethernet frame,
read with ONE recv() for the header and ONE for the body (the real daemon asserts that,
main.c:553-575). It plays the LAN's router: DHCP OFFER/ACK, ARP replies for
addresses other than the lease, ICMP echo for the router. Every ~100 ms it also puts two frames
on the "LAN" the station must not see (unicast to another MAC) and one mDNS multicast it must.

usage: [FLOOD=N] [BURST_AT=S] fake_vmnet.py SOCKET_PATH [LOG]
  FLOOD: extra broadcast frames every 100 ms; BURST_AT: 1000 broadcast frames at once, S seconds
  after the client connects. A socket path must fit sun_path, 104 bytes on macOS (sys/un.h): run it
  from a short directory and give a relative path, e.g. fake.sock.
Then: TWIN_LAN_SOCKET=fake.sock twin.py run --lan ...   or   lanprobe.py --socket fake.sock
"""
import os, socket, struct, sys, threading, time

PATH = sys.argv[1]
LOG = open(sys.argv[2], "w", buffering=1) if len(sys.argv) > 2 else sys.stderr
ROUTER_IP = bytes([192, 168, 77, 1])
LEASE_IP = bytes([192, 168, 77, 23])
MASK = bytes([255, 255, 255, 0])
ROUTER_MAC = bytes.fromhex("024757000001")
OTHER_MAC = bytes.fromhex("02aabbccddee")
lock = threading.Lock()
stats = {"frames_in": 0, "frames_out": 0, "dhcp": [], "arp_answered": 0, "icmp": 0}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=LOG)


def csum(b):
    if len(b) % 2: b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack("!H", csum(h)) + h[12:] + payload


def eth(dst, src, et, payload):
    return dst + src + struct.pack("!H", et) + payload


def send(conn, frame):
    # socket_vmnet writes header and frame with one writev (main.c:175-186)
    with lock:
        conn.sendall(struct.pack("!I", len(frame)) + frame)
        stats["frames_out"] += 1


def dhcp_reply(conn, bootp, src_mac):
    opts, i = {}, 240
    while i < len(bootp) and bootp[i] != 255:
        if bootp[i] == 0: i += 1; continue
        opts[bootp[i]] = bootp[i + 2:i + 2 + bootp[i + 1]]; i += 2 + bootp[i + 1]
    mtype = opts.get(53, b"\0")[0]
    flags = struct.unpack("!H", bootp[10:12])[0]
    chaddr = bootp[28:34]
    name = {1: "DISCOVER", 3: "REQUEST", 4: "DECLINE", 7: "RELEASE", 8: "INFORM"}.get(mtype, mtype)
    info = dict(type=name, chaddr=chaddr.hex(":"), eth_src=src_mac.hex(":"), broadcast_flag=bool(flags & 0x8000),
                opt61=opts[61].hex(":") if 61 in opts else None, opt12=opts[12].decode(errors="replace") if 12 in opts else None,
                opt55=list(opts.get(55, b"")), opt50=".".join(map(str, opts[50])) if 50 in opts else None)
    stats["dhcp"].append(info)
    log("DHCP", info)
    reply = {1: 2, 3: 5}.get(mtype)
    if reply is None: return
    b = bytearray(240)
    b[0], b[1], b[2] = 2, 1, 6
    b[4:8] = bootp[4:8]; b[10:12] = bootp[10:12]
    b[16:20] = LEASE_IP; b[20:24] = ROUTER_IP; b[28:34] = chaddr
    b[236:240] = bytes([0x63, 0x82, 0x53, 0x63])
    b += bytes([53, 1, reply, 54, 4]) + ROUTER_IP + bytes([51, 4]) + struct.pack("!I", 3600)
    b += bytes([1, 4]) + MASK + bytes([3, 4]) + ROUTER_IP + bytes([6, 4]) + ROUTER_IP + bytes([255])
    b += bytes(300 - len(b)) if len(b) < 300 else b""
    udp = struct.pack("!HHHH", 67, 68, 8 + len(b), 0) + bytes(b)
    bcast = bool(flags & 0x8000)
    dst_ip = b"\xff\xff\xff\xff" if bcast else LEASE_IP
    dst_mac = b"\xff" * 6 if bcast else chaddr
    send(conn, eth(dst_mac, ROUTER_MAC, 0x0800, ipv4(17, ROUTER_IP, dst_ip, udp)))
    log("DHCP ->", "OFFER" if reply == 2 else "ACK", ".".join(map(str, LEASE_IP)), "broadcast" if bcast else "unicast")


def handle(conn, f):
    dst, src, et = f[0:6], f[6:12], struct.unpack("!H", f[12:14])[0]
    if et == 0x0806 and len(f) >= 42 and f[20:22] == b"\x00\x01":
        sip, tip = f[28:32], f[38:42]
        if tip == LEASE_IP or tip == sip:    # a probe or announcement for the lease: nobody else has it
            log("ARP probe/announce for", ".".join(map(str, tip)), "from", src.hex(":"), "- no answer")
            return
        arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + ROUTER_MAC + tip + src + sip
        send(conn, eth(src, ROUTER_MAC, 0x0806, arp))
        stats["arp_answered"] += 1
        log("ARP who-has", ".".join(map(str, tip)), "->", ROUTER_MAC.hex(":"))
        return
    if et != 0x0800: return
    ip = f[14:]
    ihl = (ip[0] & 15) * 4
    proto, sip, dip = ip[9], ip[12:16], ip[16:20]
    body = ip[ihl:struct.unpack("!H", ip[2:4])[0]]
    if proto == 17 and struct.unpack("!H", body[2:4])[0] == 67:
        dhcp_reply(conn, body[8:], src)
    elif proto == 1 and body[0] == 8 and dip == ROUTER_IP:
        icmp = bytearray(body); icmp[0] = 0; icmp[2:4] = b"\0\0"; icmp[2:4] = struct.pack("!H", csum(bytes(icmp)))
        send(conn, eth(src, ROUTER_MAC, 0x0800, ipv4(1, ROUTER_IP, sip, bytes(icmp))))
        stats["icmp"] += 1


FLOOD = int(os.environ.get("FLOOD", "0"))        # extra broadcast frames per 100 ms
BURST_AT = float(os.environ.get("BURST_AT", "0"))  # seconds after connect: 1000 broadcast frames at once


def flood_frame(i):
    udp = struct.pack("!HHHH", 40000, 9, 8 + 18, 0) + i.to_bytes(4, "big") + bytes(14)
    return eth(b"\xff" * 6, bytes.fromhex("02f100d00001"), 0x0800, ipv4(17, bytes([192, 168, 77, 99]), b"\xff" * 4, udp))


def noise(conn, stop):
    # the LAN's chatter: frames for others (to be filtered) and an mDNS multicast (to pass)
    t0, burst_done, n = time.time(), False, 0
    while not stop.is_set():
        try:
            for _ in range(FLOOD):
                n += 1; send(conn, flood_frame(n))
            if BURST_AT and not burst_done and time.time() - t0 >= BURST_AT:
                for _ in range(1000):
                    n += 1; send(conn, flood_frame(n))
                burst_done = True; log("sent a burst of 1000 broadcast frames")
            send(conn, eth(OTHER_MAC, ROUTER_MAC, 0x0800, ipv4(17, ROUTER_IP, bytes([192, 168, 77, 50]), b"\0" * 16)))
            send(conn, eth(bytes.fromhex("02deadbeef01"), ROUTER_MAC, 0x0800, b"\x45" + b"\0" * 40))
            send(conn, eth(bytes.fromhex("01005e0000fb"), ROUTER_MAC, 0x0800, ipv4(17, ROUTER_IP, bytes([224, 0, 0, 251]), struct.pack("!HHHH", 5353, 5353, 8, 0))))
        except OSError:
            return
        time.sleep(0.1)


def serve(conn):
    stop = threading.Event()
    threading.Thread(target=noise, args=(conn, stop), daemon=True).start()
    try:
        while True:
            hdr = conn.recv(4)                  # ONE read, as main.c:553
            if not hdr: break
            assert len(hdr) == 4, f"header came in pieces: {len(hdr)}"
            n = struct.unpack("!I", hdr)[0]
            assert n <= 64 * 1024, f"main.c:564 would abort on {n}"
            body = conn.recv(n)                 # ONE read, as main.c:565; main.c:575 asserts it is whole
            assert len(body) == n, f"body came in pieces: {len(body)} of {n}"
            stats["frames_in"] += 1
            handle(conn, body)
    finally:
        stop.set()
        log("client gone; frames in", stats["frames_in"], "out", stats["frames_out"], "ARP answered", stats["arp_answered"], "ICMP", stats["icmp"])


if os.path.exists(PATH): os.unlink(PATH)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(PATH); srv.listen(1)
log("listening on", PATH)
while True:
    c, _ = srv.accept()
    log("client connected")
    threading.Thread(target=serve, args=(c,), daemon=True).start()
