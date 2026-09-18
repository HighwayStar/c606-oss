#!/bin/sh
# Builds the host map dump tool against the firmware's mapfile.c.
cd "$(dirname "$0")" && exec cc -O2 -Wall -Wextra -std=gnu11 -I ../../main -I stubs -o mapdump mapdump.c ../../main/mapfile.c -lm
