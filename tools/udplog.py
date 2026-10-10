#!/usr/bin/env python3
"""Receive the sensor's UDP log (UDP_LOG_ENABLE in src/config.h) and write it
in the same format as the serial logger, one file per day:

  logs/pga_YYYY-MM-DD.csv   time,millis,ax,ay,az,pga,pga_raw
                            #STATE,time,state   #LOG,time,line

Samples arrive in batches every 500 ms; each sample's time is reconstructed
from its millis() relative to the newest sample in the packet.

Usage: python3 tools/udplog.py logs/
"""
import os, socket, sys
from datetime import datetime

PORT = 5514


def stamp(t):
    return datetime.fromtimestamp(t).isoformat(timespec="milliseconds")


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "logs"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", PORT))
    carry = {}  # sender -> partial last line
    day, f = None, None
    while True:
        data, addr = sock.recvfrom(4096)
        now = datetime.now().timestamp()
        today = datetime.fromtimestamp(now).strftime("%Y-%m-%d")
        if today != day:
            if f:
                f.close()
            day = today
            f = open(os.path.join(out_dir, f"pga_{day}.csv"), "a", buffering=1)
        text = carry.get(addr[0], "") + data.decode(errors="replace")
        lines = text.split("\n")
        carry[addr[0]] = lines.pop()  # incomplete line, finished by the next packet
        millis = [int(l.split(",")[1]) for l in lines if l.startswith("PGA,") and l.count(",") >= 6]
        newest = max(millis) if millis else 0
        for l in lines:
            l = l.rstrip("\r")
            if l.startswith("PGA,"):
                try:
                    ms = int(l.split(",")[1])
                except ValueError:
                    continue
                f.write(stamp(now - (newest - ms) / 1000.0) + "," + l[4:] + "\n")
            elif l.startswith("STATE:"):
                f.write(f"#STATE,{stamp(now)},{l[7:]}\n")
            elif l.startswith("[") or l.startswith("PGA Trigger") or l.startswith("PGA TRIGGER"):
                f.write(f"#LOG,{stamp(now)},{l}\n")


if __name__ == "__main__":
    main()
