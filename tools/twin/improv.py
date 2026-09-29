#!/usr/bin/env python3
"""improv.py SSID PASSWORD -> the Improv-Serial "send Wi-Fi settings" packet as hex.
Format from the Improv WiFi Library the firmware links (ImprovWiFiLibrary.cpp parseImprovSerial /
parseImprovData): "IMPROV", version 1, type 3 (RPC), length, then command 1 (WIFI_SETTINGS),
data length, ssid length, ssid, password length, password; last the sum of all bytes before it."""
import sys
ssid, pw = sys.argv[1].encode(), sys.argv[2].encode()
rpc = bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw
data = bytes([0x01, len(rpc)]) + rpc
pkt = b"IMPROV" + bytes([1, 3, len(data)]) + data
print((pkt + bytes([sum(pkt) & 0xff])).hex())
