# FrameSender

Standalone Windows software for sending an exact, uncompressed center crop of a desktop display to the UDP Vision Receiver on another PC.

FrameSender offers three documented Windows capture methods: DXGI Desktop Duplication (the default), Windows Graphics Capture, and GDI BitBlt. Each sends an unscaled center crop as raw BGRA8 UVF1 UDP fragments. CUDA is not required. DXGI and WGC copy only the crop into a CPU-readable GPU staging texture; GDI copies the crop into a top-down bitmap.

FrameSender does not inject into another process or attempt to bypass protected-content, capture-blocking, or anti-cheat mechanisms. Content masked by Windows remains masked.

## Requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2022 with **Desktop development with C++** and Windows SDK 10.0.19041 or newer (including C++/WinRT headers).
- CMake 3.24 or newer.
- A wired LAN. 2.5 GbE or faster is recommended for crops larger than 320×320 at high refresh rates.
- DXGI requires an unrotated display. WGC and GDI use the display's physical desktop orientation.
- WGC requires Windows 10 version 2004 or newer and system support for Windows Graphics Capture.

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

The GitHub Actions workflow builds Windows Release and Debug, runs `--version`, the built-in `--self-test`, and the resolution/cursor protocol checks. Portable protocol checks also run on Linux in both configurations. Release builds upload a ZIP artifact, and each job retains diagnostics.

To create a standalone ZIP containing the executable and README:

```powershell
.\package.ps1
```

## Run

### Capture methods

Choose a method with `--capture dxgi`, `--capture wgc`, or `--capture gdi`.
Omitting the option keeps DXGI. Methods are selected explicitly; a failed method
reports an error instead of silently changing capture behavior.

| Method | Description | Behavior and limitations |
|---|---|---|
| `dxgi` (default) | DXGI Desktop Duplication reads the composed desktop through Direct3D 11 and copies just the center crop to staging memory. | Suited to high-refresh desktop capture. Sends new desktop presentations and preserves their QPC timestamp. Rejects rotated duplication surfaces. Hardware cursor shapes are not added to the image. |
| `wgc` | Windows Graphics Capture creates a system-managed monitor capture session, using a two-frame GPU pool and the same crop readback as DXGI. | Requires Windows 10 2004+; excludes the cursor and retains Windows' capture border. Sends available frames with system timestamps capped at acquisition time. Windows or display policy can deny capture. BGRA8 capture of HDR content can differ from DXGI; use SDR for consistent color comparisons. |
| `gdi` | GDI `BitBlt` copies the visible center crop, including layered windows, into a CPU-readable 32-bit top-down bitmap. | Compatibility option with higher CPU overhead and possible missing hardware overlays or exclusive fullscreen content. Takes repeated snapshots, including on a static desktop, at 60 FPS by default or the requested `--fps` rate. Timestamps mark completion of the copy, and the undefined bitmap alpha byte is set to 255. |

All methods use Windows APIs from FrameSender's own process, with no injection,
hooks, drivers, or changes to the captured application. Protected, excluded, or
unavailable content can remain black or absent. Choosing another method does not
grant access to protected content. Cursor position remains available separately
through the existing Receiver calibration protocol. Each method uses the same
`--output` indices, exact crop dimensions, resolution sync, and UDP framing.

List the methods and their descriptions without opening capture or networking:

```powershell
.\build\Release\frame_sender.exe --list-capture-methods
```

Use WGC with Receiver's automatic crop selection:

```powershell
.\build\Release\frame_sender.exe --host 192.168.1.10 --capture wgc --output 0
```

Use GDI for a fixed 320×320 crop at up to 30 snapshots per second:

```powershell
.\build\Release\frame_sender.exe --host 192.168.1.10 --capture gdi --output 0 --no-sync-resolution --size 320 --fps 30
```

The implementations follow Microsoft's [Windows Graphics Capture documentation](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture), [BitBlt API](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt), and [CreateDIBSection API](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection).

### Display and crop selection

List available displays:

```powershell
.\build\Release\frame_sender.exe --list-outputs
```

Send a center crop matching Receiver's active model automatically:

```powershell
.\build\Release\frame_sender.exe --host 192.168.1.10 --output 0
```

FrameSender waits for Receiver to finish loading its model before sending pixels.
WGC starts its capture session when the first crop is ready, avoiding queued
startup frames while Receiver loads.
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
| `--capture METHOD` | `dxgi` (default), `wgc`, or `gdi`; see the descriptions above |
| `--list-capture-methods` | Print capture methods and descriptions without starting capture |
| `--fps N` | Maximum send rate; zero sends available DXGI/WGC frames, while GDI polls at 60 FPS |
| `--output N` | Display index reported by `--list-outputs` |
| `--port N` | Receiver UDP frame port; default 5000 |
| `--send-buffer-mib N` | UDP queue size, 1–16 MiB; default 2 |
| `--self-test` | Verify UVF1 wire encoding without opening capture or networking |
| `--version` | Print the FrameSender version |

Receiver must allow inbound UDP on the selected frame port and configure **Sender IPv4** to this PC's wired-LAN address.

## Latency behavior

- DXGI uses its presentation timestamp, WGC uses its system-relative frame timestamp capped at acquisition time, and GDI uses the copy-completion timestamp. WGC's compositor can predict a future presentation; the cap prevents negative capture latency. All timestamps are expressed in monotonic nanoseconds for Receiver's clock synchronization.
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

CI builds all three capture implementations in Windows Release and Debug and tests method selection, CLI validation, exact crop geometry, and the existing wire protocols. Portable geometry and protocol tests also run on Linux. These checks do not exercise an interactive desktop. Verify exact crop pixels for each method, display-mode recovery, sustained throughput, packet loss, and capture-to-reassembly latency on the deployment PCs before relying on production measurements.

## Receiver calibration feedback

Updated Receiver builds can request `UVP1`/`UVG1` cursor feedback for automatic
desktop calibration and verification of move/click/return actions. FrameSender
reports cursor position, visibility, physical display/crop geometry and an
associated frame sequence. It does not move the mouse. Feedback continues on a
static desktop and before the first crop request. No additional command-line
option is required. See [the protocol](docs/PROTOCOL.md).
