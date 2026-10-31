# GPS Host EPO assistance

## Source and format

The C606 vendor firmware contains host `rfs-fitness.rfsvr.net` and path
`/indoor/v1/app/config/loda_agnss/download`. A verified HTTPS GET returns a
binary attachment named `<download time>_<expiry>_agnss.bin`, without an
account token. Its Content-Type can be `text/html`: validate the archive,
not that header. The firmware uses the ESP-IDF certificate bundle and never
disables certificate verification.

TLS allocation uses `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`: with Wi-Fi, maps
and logging active, internal RAM may not have the contiguous record buffers
needed by mbedTLS (`-0x7F00`, allocation failed). PSRAM is already required by
the bounded archive buffer. Certificate validation and TLS record-size limits
remain enabled; reducing verification or switching to plaintext is not a fix.

The observed archive is 167912 bytes: a 224-byte header, one BDS file, and
five 3-day GPS EPO files. The parser only supports this verified six-member
layout. It checks names, sizes, version, magic, exact total length, every
GPS record's XOR checksum, its satellite ID and the set's common epoch.
Unexpected formats are rejected, not sent to the receiver.

`main/epo.c` selects the valid six-hour GPS set. Each set contains 32 records
of 72 bytes: bytes 0..2 are hours from the GPS epoch, byte 3 is satellite ID,
and the last 32-bit little-endian word is the XOR of the preceding 17 words.
The selector rejects expired/future sets and avoids the last two minutes of
a validity window. Archive names/timestamps alone are not trusted.

## Receiver protocol

The Airoha AG3352 family supports Host EPO via
`$PAIR471,<System_ID>,<SV_ID>,<W[0]>,...,<W[17]>*checksum`.
System ID is 0 (GPS); SV_ID and the 18 little-endian words are hexadecimal.
The GPS task serializes UART writes and waits for `$PAIR001,471,0` before
sending the next record. A rejection or timeout aborts the sequence.

This mode writes assistance to receiver **RAM**, not its firmware or flash.
There is no GNSS erase, firmware update, binary-flash transfer, cold reset,
simulated coordinate injection or constellation reconfiguration. Only GPS
EPO is injected; the BDS member is retained in the archive but not uploaded.

Reference: Quectel LC26G/LC76G/LC86G AGNSS Application Note v1.1,
sections 2.1.2, 2.3 and 3.1.3 (same AG3352 platform).

## User workflow

Settings -> Wi-Fi / GPS time has two actions:

- **Sync GPS time** sends fresh SNTP UTC only, as before.
- **Update A-GPS** gets UTC, downloads the vendor archive, validates it,
  sends reference time and all 32 GPS records, and stops Wi-Fi.

In Russian: **Настройки -> Wi-Fi / время GPS -> Обновить A-GPS**.
Success is displayed only when all 32 records have been acknowledged.
Acknowledgement means the receiver accepted the assistance, not that it has
a position fix. Satellite signals must still be received outdoors.

The last successfully applied archive is cached at `c606oss/agnss.bin` on
the eMMC. It can be reused if a subsequent download fails and its contents
are still valid. Fresh UTC is still needed. Host EPO must be supplied again
after receiver power loss; it is not automatically injected on boot in this
version. Repeat Update A-GPS after power-on when assistance is needed.
Temporary cache names are `agnss.download` and `agnss.previous`; none of
the vendor files, maps or routes are edited.

## Tests and developer console

```sh
sh tools/test_epo.sh
SANITIZE=1 sh tools/test_epo.sh
sh tools/test_l10n.sh
sh tools/test_gps_aid.sh
idf.py -DBOARD=c606 build
```

Tests cover validity intervals, archive bounds, corrupt checksums, duplicate
PRNs and hexadecimal command formatting. An optional argument to the EPO
test accepts the exact vendor sample captured on 2026-10-31; its fixed test
UTC makes the test repeatable rather than dependent on today's date.

`gps-epo <current Unix UTC seconds>` applies the cached archive for a live
receiver test from a time-synchronized host. It returns
`GPS_EPO <result> <accepted>/32 expires=<unix seconds>`.
Check the `agnss` and `gps` log tags to distinguish network/validation errors
from receiver command rejection. Do not interpret partial acceptance as a
successful update, or claim a TTFF improvement without an outdoor comparison.
