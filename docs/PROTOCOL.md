# FrameSender wire protocol

All integers use network byte order. Every message occupies one UDP datagram. FrameSender connects one UDP socket to the configured Receiver endpoint, so heartbeat, frame, clock, and resolution traffic share one stable source IP and port. The protocol is unauthenticated and intended for a trusted wired LAN.

## Sender heartbeat: UVH1

Sent approximately every 50 ms, independently of desktop capture:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UVH1` |
| 4 | 4 | Reserved, zero |
| 8 | 8 | Nonzero random sender session |

Receiver binds sender ownership to the source endpoint and session.

## Raw frame fragment: UVF1

Each fragment contains a 48-byte header followed by up to 1352 bytes of uncompressed BGRA data:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UVF1` |
| 4 | 1 | Version 1 |
| 5 | 1 | Format 2, BGRA32 |
| 6 | 2 | Header length 48 |
| 8 | 8 | Sender session |
| 16 | 4 | Frame sequence modulo 2³² |
| 20 | 8 | Sender monotonic capture time in nanoseconds |
| 28 | 2 | Crop width |
| 30 | 2 | Crop height |
| 32 | 4 | Complete raw-frame byte length |
| 36 | 2 | Zero-based fragment index |
| 38 | 2 | Fragment count |
| 40 | 4 | Payload byte offset |
| 44 | 2 | Fragment stride, 1352 |
| 46 | 2 | Reserved, zero |
| 48 | variable | Tightly packed BGRA bytes |

Rows run top-to-bottom with bytes ordered B, G, R, A. Width and height are each 1–1024. The total byte length is `width × height × 4`, fragment count is `ceil(total / 1352)`, and the last fragment carries only the remaining bytes. There is no retransmission: fresh frames take priority over incomplete old frames.

## Clock synchronization: UVC1 and UVS1

Receiver sends:

`UVC1 | reserved:u32=0 | session:u64 | t0:u64`

FrameSender records `t1` immediately after receipt and `t2` immediately before replying:

`UVS1 | reserved:u32=0 | session:u64 | echoed_t0:u64 | t1:u64 | t2:u64`

All timestamps use the same QueryPerformanceCounter-derived monotonic nanosecond clock as UVF1 capture timestamps.

## Receiver resolution request: UVN1

Receiver sends this 24-byte request after a sender heartbeat and successful model
initialization, whenever the active input size changes, and every 250 ms while the
sender's heartbeat lease is fresh. It keeps requesting the previous size during a
model replacement and after a failed replacement. Color-only mode uses Receiver's
configured capture size.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UVN1` |
| 4 | 1 | Version 1 |
| 5 | 1 | Requested format 2, BGRA32 |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Sender session |
| 16 | 2 | Requested crop width |
| 18 | 2 | Requested crop height |
| 20 | 4 | Reserved, zero |

FrameSender follows UVN1 by default and waits for its first valid request before
sending frames. It requires a matching nonzero session, version 1, BGRA32 format 2,
zero reserved fields, and dimensions in 1–1024. Invalid requests are ignored. A
valid crop larger than the selected display stops sending with a clear error.
Applying UVN1 changes the exact crop dimensions between frames and never scales
pixels. The following UVF1 frames carry the applied width, height, and format; no
separate acknowledgement is needed. Periodic requests recover from UDP loss.

`--no-sync-resolution` ignores UVN1 and uses the manual dimensions. Older senders
may also ignore UVN1; Receiver continues to accept their valid RGB24/BGRA32 frames
at the sender's chosen dimensions.
