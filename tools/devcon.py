#!/usr/bin/env python3
"""Drive the firmware's developer console (main/devcon.c) over the USB port.

  tools/devcon.py PORT key 0 1          click key 0 (evt 1 click, 4 hold, 5 release)
  tools/devcon.py PORT tap 120 160      touch at x,y
  tools/devcon.py PORT shot out.png     screenshot
  tools/devcon.py PORT heap
  tools/devcon.py PORT ls [/sdcard/dir]  list the ride files (default /sdcard/c606oss)
  tools/devcon.py PORT get /sdcard/c606oss/x.fit [out.fit]   copy a file to the host
  tools/devcon.py PORT put local.gpx /sdcard/c606oss/routes/x.gpx   copy a file to the card
  tools/devcon.py PORT mv /sdcard/a /sdcard/b   rename a file on the card
  tools/devcon.py PORT pos 55.03 82.92  centre the map page on a position ("pos" alone: back to GPS)
  tools/devcon.py PORT zoom 14          map zoom level
  tools/devcon.py PORT sim 55.03 82.92 45 25 30   simulated GPS: from lat,lon heading 45 deg at 25 km/h for 30 s
  tools/devcon.py PORT nmea off         back to the real receiver
  tools/devcon.py PORT script "key 0 1" "sleep 0.5" "shot a.png" ...

Needs pyserial. Log lines from the device are printed as they arrive."""
import os, re, struct, sys, time, zlib
import serial

def png_from_rgb565(w, h, rows, path):
    raw = bytearray()
    for r in rows:
        raw.append(0)   # filter none
        for i in range(0, len(r), 2):
            v = r[i] | (r[i + 1] << 8)
            raw += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 0x3f) * 255 // 63, (v & 0x1f) * 255 // 31))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))

def read_line(p, timeout):
    p.timeout = timeout
    return p.readline().decode("utf-8", "replace")

def shot(p, path):
    p.reset_input_buffer()
    p.write(b"shot\n")
    w = h = None
    rows = []
    end = time.time() + 20
    while time.time() < end:
        line = read_line(p, 2)
        if not line:
            continue
        m = re.search(r"SHOT (\d+) (\d+)", line)
        if m:
            w, h = int(m.group(1)), int(m.group(2))
            continue
        # a row is written atomically but log output from other tasks may
        # precede it on the same line
        m = re.search(r"R([0-9a-f]{%d})" % (w * 4), line) if w else None
        if m:
            rows.append(bytes.fromhex(m.group(1)))
        elif "SHOT_END" in line:
            break
        else:
            sys.stdout.write(line)
    if not w or len(rows) != h:
        sys.exit(f"incomplete screenshot ({len(rows)} rows)")
    png_from_rgb565(w, h, rows, path)
    print(f"saved {path} ({w}x{h})")

def ls(p, path):
    p.reset_input_buffer()
    p.write(f"ls {path}\n".encode() if path else b"ls\n")
    end = time.time() + 10
    while time.time() < end:
        line = read_line(p, 2)
        if not line:
            continue
        if "LS_END" in line:
            rest = line.split("LS_END", 1)[1].strip()
            if rest:
                print(rest)
            break
        sys.stdout.write(line)

def get(p, path, out):
    p.reset_input_buffer()
    p.write(f"get {path}\n".encode())
    size = None
    chunks = []
    got = 0
    end = time.time() + 600
    while time.time() < end:
        line = read_line(p, 5)
        if not line:
            continue
        m = re.search(r"FILE (\d+)", line)
        if m and size is None:
            size = int(m.group(1))
            continue
        m = re.search(r"F([0-9a-f]+)$", line.strip()) if size is not None else None
        if m:
            b = bytes.fromhex(m.group(1))
            chunks.append(b)
            got += len(b)
        elif "FILE_END" in line:
            rest = line.split("FILE_END", 1)[1].strip()
            if rest:
                sys.exit(rest)
            break
        else:
            sys.stdout.write(line)
    if size is None or got != size:
        sys.exit(f"incomplete transfer ({got} of {size} bytes)")
    with open(out, "wb") as f:
        f.write(b"".join(chunks))
    print(f"saved {out} ({size} bytes)")

def put(p, local, path):
    data = open(local, "rb").read()
    p.reset_input_buffer()
    p.write(f"put {path} {len(data)}\n".encode())
    end = time.time() + 5
    while time.time() < end:
        line = read_line(p, 1)
        if "PUT_GO" in line:
            break
        if "PUT_ERR" in line:
            sys.exit(line.strip())
        if line:
            sys.stdout.write(line)
    else:
        sys.exit("device did not accept the transfer")
    for i in range(0, len(data), 128):
        p.write(b"F" + data[i:i + 128].hex().encode() + b"\n")
        p.flush()
        end = time.time() + 5
        while time.time() < end:
            line = read_line(p, 1)
            if "PUT_ACK" in line:
                break
            if line:
                sys.stdout.write(line)
        else:
            p.write(b"PUT_END\n")
            sys.exit(f"no ack after {i} bytes")
    p.write(b"PUT_END\n")
    end = time.time() + 30
    while time.time() < end:
        line = read_line(p, 2)
        if "PUT_OK" in line or "PUT_ERR" in line:
            print(line.strip())
            return
        if line:
            sys.stdout.write(line)
    sys.exit("no reply after the transfer")

def nmea(fields):
    body = ",".join(fields)
    cs = 0
    for c in body.encode():
        cs ^= c
    return f"${body}*{cs:02X}"

def sim(p, lat, lon, course, kmh, seconds):
    """Feeds one RMC + GGA pair per second, moving along `course`."""
    import math
    def dm(v, w):
        d = int(abs(v)); m = (abs(v) - d) * 60
        return f"{d:0{w}d}{m:07.4f}"
    step_m = kmh / 3.6
    for i in range(int(seconds)):
        t = time.gmtime()
        hms = time.strftime("%H%M%S", t) + ".00"
        rmc = nmea(["GPRMC", hms, "A", dm(lat, 2), "N" if lat >= 0 else "S", dm(lon, 3), "E" if lon >= 0 else "W",
                    f"{kmh / 1.852:.1f}", f"{course:.1f}", time.strftime("%d%m%y", t), "", "", "A"])
        gga = nmea(["GPGGA", hms, dm(lat, 2), "N" if lat >= 0 else "S", dm(lon, 3), "E" if lon >= 0 else "W",
                    "1", "08", "1.0", "150.0", "M", "0.0", "M", "", ""])
        simple(p, "nmea " + rmc)
        simple(p, "nmea " + gga)
        lat += step_m * math.cos(math.radians(course)) / 111320.0
        lon += step_m * math.sin(math.radians(course)) / (111320.0 * math.cos(math.radians(lat)))
        end = time.time() + 1.0
        while time.time() < end:
            line = read_line(p, 0.2)
            if line:
                sys.stdout.write(line)

def simple(p, cmd):
    p.reset_input_buffer()
    p.write((cmd + "\n").encode())
    end = time.time() + 1.0
    while time.time() < end:
        line = read_line(p, 0.3)
        if line:
            sys.stdout.write(line)
            if line.strip() in ("ok", "mv failed") or line.startswith(("heap ", "zoom ")):
                break

def run(p, args):
    if not args:
        return
    if args[0] == "shot":
        shot(p, args[1] if len(args) > 1 else "shot.png")
    elif args[0] == "sleep":   # keeps printing the device log meanwhile
        end = time.time() + float(args[1])
        while time.time() < end:
            line = read_line(p, min(0.3, max(0.01, end - time.time())))
            if line:
                sys.stdout.write(line)
    elif args[0] == "ls":
        ls(p, args[1] if len(args) > 1 else "")
    elif args[0] == "put":
        put(p, args[1], args[2])
    elif args[0] == "sim":
        sim(p, *[float(a) for a in args[1:6]])
    elif args[0] == "get":
        get(p, args[1], args[2] if len(args) > 2 else os.path.basename(args[1]))
    else:
        simple(p, " ".join(args))

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    p = serial.Serial(sys.argv[1], 115200, timeout=0.5)
    if sys.argv[2] == "script":
        for step in sys.argv[3:]:
            run(p, step.split())
    else:
        run(p, sys.argv[2:])

if __name__ == "__main__":
    main()
