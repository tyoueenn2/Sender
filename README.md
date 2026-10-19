# FrameSender

Standalone Windows software for sending an exact, uncompressed center crop of a desktop display to the UDP Vision Receiver on another PC.

FrameSender uses the documented Windows DXGI Desktop Duplication API. The GPU copies only the selected center crop into a CPU-readable staging texture. Native BGRA8 pixels are packed without resizing, filtering, color conversion, or compression, then sent as UVF1 UDP fragments. CUDA is not required, and ordinary AMD Radeon, Intel, and NVIDIA Direct3D 11 display adapters are supported.

FrameSender does not inject into another process or attempt to bypass protected-content, capture-blocking, or anti-cheat mechanisms. Content masked by Windows remains masked.

## Requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- CMake 3.24 or newer.
- A wired LAN. 2.5 GbE or faster is recommended for crops larger than 320×320 at high refresh rates.
- An unrotated landscape display. Rotated Desktop Duplication surfaces are rejected rather than transformed.

## Build

Open a Developer PowerShell in this directory:

```powershell
.\build.ps1
```

Equivalent manual commands:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --target frame_sender
```

The executable is written to `build\Release\frame_sender.exe`.

The included GitHub Actions workflow performs the same Windows build, runs `--version` and the built-in `--self-test`, and publishes a ZIP artifact when this directory is placed in its own repository.

To create a standalone ZIP containing the executable and README:

```powershell
.\package.ps1
```

## Run

List available displays:

```powershell
.\build\Release\frame_sender.exe --list-outputs
```

Send a center crop matching Receiver's active model automatically:

```powershell
.\build\Release\frame_sender.exe --host 192.168.1.10 --output 0
```

FrameSender waits for Receiver to finish loading its model before sending pixels.
Changing the model or input size in Receiver updates the crop while both programs
keep running. In color-only mode it follows Receiver's **Capture size** setting.
Pixels remain an exact, unscaled BGRA center crop.

For a fixed 320×320 crop, or a Receiver build without resolution requests:

```powershell
.\build\Release\frame_sender.exe --host 192.168.1.10 --output 0 --no-sync-resolution --size 320
```

Useful options:

| Option | Meaning |
|---|---|
| `--sync-resolution` | Follow Receiver's session-bound UVN1 size request; enabled by default |
| `--no-sync-resolution` | Use manual dimensions; also supports older Receivers |
| `--size N` | Manual square center crop, 1–1024 pixels; default 320 |
| `--width N --height N` | Manual rectangular center crop |
| `--fps N` | Maximum send rate; zero sends every new desktop present |
| `--output N` | Display index reported by `--list-outputs` |
| `--port N` | Receiver UDP frame port; default 5000 |
| `--send-buffer-mib N` | UDP queue size, 1–16 MiB; default 2 |
| `--self-test` | Verify UVF1 wire encoding without opening capture or networking |
| `--version` | Print the FrameSender version |

Receiver must allow inbound UDP on the selected frame port and configure **Sender IPv4** to this PC's wired-LAN address.

## Latency behavior

- DXGI's presentation timestamp is sent as the frame capture time.
- Capture and packetization always favor the newest desktop image.
- Heartbeats and clock synchronization use an independent control thread.
- UDP datagrams stay at or below 1400 bytes to avoid IPv4 fragmentation with a normal 1500-byte MTU.
- A full socket never blocks capture behind stale pixels. The partial frame is abandoned and a short drain interval allows a later fresh frame to complete.
- `--sync-resolution` changes the exact crop dimensions; it never scales an image.
- A requested crop larger than the selected display reports an error. Choose a
  smaller Receiver input size or a larger display.

Raw BGRA payload rates before protocol and Ethernet overhead:

| Crop | 120 FPS | 240 FPS |
|---:|---:|---:|
| 160×160 | 98 Mbit/s | 197 Mbit/s |
| 320×320 | 393 Mbit/s | 786 Mbit/s |
| 640×640 | 1.57 Gbit/s | 3.15 Gbit/s |

If the requested rate exceeds the physical link, set `--fps` to a sustainable value. Successful UDP submission does not prove delivery; use Receiver's completed-frame and latency metrics for deployment measurements.

## Receiver compatibility

FrameSender uses these existing Receiver protocols:

- `UVH1`: sender ownership heartbeat every 50 ms.
- `UVC1` / `UVS1`: monotonic clock synchronization.
- `UVF1`: raw BGRA frame fragments.
- `UVN1`: Receiver-requested center-crop dimensions, followed by default.

Receiver repeats UVN1 requests every 250 ms and immediately after the active input
size changes. Requests are bound to the sender's current session and connected
Receiver endpoint. Malformed requests, unsupported formats, and stale sessions
are ignored. Unknown control datagrams are ignored.

Both programs need builds with UVN1 support for automatic sizing. If the sender
keeps waiting, start Receiver's UDP session and check its **Sender IPv4**, UDP port,
and firewall rules. Use `--no-sync-resolution` with Receiver builds that predate
UVN1. Manual senders continue to work with the updated Receiver.

The complete byte layouts are documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Portable UVN1 tests can also run without Windows capture:

```bash
cmake -S . -B build/protocol-tests -DFRAME_SENDER_TESTS_ONLY=ON
cmake --build build/protocol-tests
ctest --test-dir build/protocol-tests --output-on-failure
```

## Current validation boundary

The wire layout has been checked against Receiver's protocol implementation. This workspace does not currently expose an MSVC compiler or Windows SDK, so the executable still requires its first Release build and hardware benchmark on the deployment PCs. Verify exact crop pixels, display-mode recovery, sustained throughput, packet loss, and capture-to-reassembly latency before relying on production measurements.
