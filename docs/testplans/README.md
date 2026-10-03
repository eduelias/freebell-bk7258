# Test plans and results

These are the author's working test plans, one per feature branch, each ending with the results
measured on the real device on the date given. They are kept as a record of *how* each feature was
verified and what was found; they are not a tutorial. Expect bench detail, dead ends and timings.

Conventions in these files: `<device-ip>` / `<ha-ip>` were, on the author's bench, `192.168.2.89`
(the doorbell) and `192.168.2.80` (Home Assistant); the "bench host" is a Linux machine on the same
LAN used for TCP peers and long soaks (see [`../notes/bench-setup.md`](../notes/bench-setup.md)).
Device commands are `python3 tools/dbcmd.py <cmd>` unless marked UART.

| File | Feature | Firmware |
|---|---|---|
| [TESTPLAN-polish.md](TESTPLAN-polish.md) | night light arbiter, blue ring indicator, amplifier pop, `sys info` | 0.19.x |
| [TESTPLAN-motion.md](TESTPLAN-motion.md) | PIR control (`pir`), motion/ring snapshot pushes in Home Assistant | 0.20.x |
| [TESTPLAN-power.md](TESTPLAN-power.md) | battery, charger, deep sleep and wake sources (`pwr`), HA sensors | 0.21.x |
| [TESTPLAN-rtsp.md](TESTPLAN-rtsp.md) | H.264 RTSP server, camera mode policy, HA camera entry options | 0.22.x |
| [TESTPLAN-talk.md](TESTPLAN-talk.md) | two-way audio over HTTP (`/speak`, `/mic.wav`, `/talk`), HA talk scripts | 0.23.x |
| [TESTPLAN-sd.md](TESTPLAN-sd.md) | optional microSD storage (`sd`), SNTP time | 0.24.0 |
