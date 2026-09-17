#!/usr/bin/env python3
"""Capture the console (USB-Serial-JTAG) for N seconds.
  tools/serial_log.py PORT [seconds] [--reset]
--reset pulses RTS like esptool's hard_reset so the boot log is captured too.
Needs pyserial (available inside the ESP-IDF container)."""
import sys, time, serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 15
p = serial.Serial(port, 115200, timeout=0.2)
if "--reset" in sys.argv:
    p.dtr = False
    p.rts = True
    time.sleep(0.1)
    p.rts = False
    p.reset_input_buffer()
end = time.time() + secs
while time.time() < end:
    d = p.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()
