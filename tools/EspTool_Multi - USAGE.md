# EspTool_Multi — CLI Usage Guide

Parallel ESP32 flash tool. Flashes identical firmware to one or more ESP32 boards simultaneously over serial, with MD5 verification, automatic retry, and interleaved round-robin writes. Also supports parallel full-chip erase and selective region erase.

## Quick Start

```bash
# Flash three boards at once
EspTool_Multi -p COM8,COM9,COM10 write_flash 0x0 bootloader.bin 0x10000 firmware.bin

# Erase three boards at once
EspTool_Multi -p COM8,COM9,COM10 erase_flash
```

## Syntax

```
EspTool_Multi [OPTIONS] <COMMAND>
```

Options can appear in any order before the command.

### Commands

| Command | Description |
|---------|-------------|
| `write_flash <offset> <file> [...]` | Flash one or more firmware segments to all target devices. |
| `erase_flash` | Erase the entire flash memory on all target devices. |
| `erase_region <offset> <size>` | Erase a specific region of flash memory. |

---

## Options Reference

| Short | Long            | Argument         | Default  | Description                          |
|-------|-----------------|------------------|----------|--------------------------------------|
| `-p`  | `--port`        | `<PORTS>`        | _(required)_ | Serial port(s), comma-separated  |
| `-b`  | `--baud`        | `<RATE>`         | `460800` | Baud rate for flash transfer         |
| `-n`  | `--no-verify`   | _(none)_         | verify on | Skip MD5 verification after flash   |
| `-h`  | `--help`        | _(none)_         |          | Show help and exit                   |
| `-V`  | `--version`     | _(none)_         |          | Show version and exit                |

### `-p, --port <PORTS>`

One or more serial ports, separated by commas. No spaces around commas.

```bash
# Single port
EspTool_Multi -p COM8 write_flash ...

# Multiple ports (parallel flash)
EspTool_Multi -p COM8,COM9,COM10 write_flash ...

# Linux
EspTool_Multi -p /dev/ttyUSB0,/dev/ttyACM1 write_flash ...

# Long form
EspTool_Multi --port COM8,COM9 write_flash ...
```

### `-b, --baud <RATE>`

Baud rate used for flash data transfer. The initial bootloader sync always happens at 115200; after the stub is uploaded, the connection switches to this rate.

```bash
# Default (460800)
EspTool_Multi -p COM8 write_flash ...

# Slower, more reliable over long cables
EspTool_Multi -p COM8 -b 115200 write_flash ...

# Maximum speed
EspTool_Multi -p COM8 -b 921600 write_flash ...

# Common values: 115200, 230400, 460800, 921600
```

### `-n, --no-verify`

Skip the MD5 verification pass after each flash segment. By default, every segment is verified by reading back its MD5 hash from the chip and comparing against the source file. Only applies to `write_flash`.

```bash
# With verification (default)
EspTool_Multi -p COM8 write_flash 0x10000 firmware.bin

# Without verification (faster, less safe)
EspTool_Multi -p COM8 -n write_flash 0x10000 firmware.bin
EspTool_Multi -p COM8 --no-verify write_flash 0x10000 firmware.bin
```

---

## Commands

### `erase_flash`

Erases the entire flash memory on all target devices in parallel. No firmware files are needed. After erasing, devices are reset.

```bash
# Erase a single board
EspTool_Multi -p COM8 erase_flash

# Erase three boards in parallel
EspTool_Multi -p COM8,COM9,COM10 erase_flash

# Erase at a specific baud rate
EspTool_Multi -p COM8,COM9 -b 115200 erase_flash
```

#### Example output

```
[COM8 ] BOOT        syncing...
[COM9 ] BOOT        syncing...
[COM10] BOOT        syncing...
[COM8 ] BOOT   100% synced
[COM8 ] CHIP   100% ESP32c6
[COM8 ] STUB        uploading...
[COM8 ] STUB   100% complete
[COM8 ] BAUD   100% 460800
...
[COM8 ] ERASE       erasing...
[COM9 ] ERASE       erasing...
[COM10] ERASE       erasing...
[COM8 ] ERASE  100% complete
[COM9 ] ERASE  100% complete
[COM10] ERASE  100% complete
[COM8 ] RESET  100% complete
[COM9 ] RESET  100% complete
[COM10] RESET  100% complete

All ports erased successfully.
```

### `erase_region`

Erases a specific region of flash memory on all target devices in parallel. Both `<offset>` and `<size>` must be aligned to the flash sector size (typically 4096 / 0x1000 bytes).

```bash
# Erase the filesystem partition (768KB at 0x300000)
EspTool_Multi -p COM8,COM9,COM10 erase_region 0x300000 0xC0000

# Erase the app partition (3MB at 0x10000)
EspTool_Multi -p COM8 erase_region 0x10000 0x2F0000

# Erase NVS (128KB at 0x3C0000)
EspTool_Multi -p COM8,COM9 erase_region 0x3C0000 0x20000
```

### `write_flash`

Flash one or more firmware segments to all target devices. Provide `<offset> <file>` pairs after the command. Offsets can be hex (`0x1000`) or decimal (`4096`).

```bash
# Single segment
EspTool_Multi -p COM8 write_flash 0x10000 firmware.bin

# Multiple segments (typical ESP32 layout)
EspTool_Multi -p COM8 write_flash \
    0x0     bootloader.bin \
    0x8000  partitions.bin \
    0x10000 firmware.bin \
    0x300000 littlefs.bin

# Decimal offsets work too
EspTool_Multi -p COM8 write_flash 0 bootloader.bin 32768 partitions.bin 65536 firmware.bin
```

---

## Common Flash Layouts

**ESP-IDF / PlatformIO (4 MB flash):**

| Offset     | File              | Description         |
|------------|-------------------|---------------------|
| `0x0`      | `bootloader.bin`  | Second-stage bootloader |
| `0x8000`   | `partitions.bin`  | Partition table     |
| `0x10000`  | `firmware.bin`    | Application         |
| `0x300000` | `littlefs.bin`    | Filesystem image (optional) |

```bash
EspTool_Multi -p COM8 write_flash \
    0x0      build/bootloader.bin \
    0x8000   build/partitions.bin \
    0x10000  build/firmware.bin \
    0x300000 build/littlefs.bin
```

**Arduino (default partitions):**

| Offset     | File              | Description         |
|------------|-------------------|---------------------|
| `0x1000`   | `bootloader.bin`  | Bootloader          |
| `0x8000`   | `partitions.bin`  | Partition table     |
| `0xE000`   | `boot_app0.bin`   | OTA data            |
| `0x10000`  | `sketch.bin`      | Application         |

```bash
EspTool_Multi -p COM3 write_flash \
    0x1000  bootloader.bin \
    0x8000  partitions.bin \
    0xE000  boot_app0.bin \
    0x10000 sketch.bin
```

---

## Parallel Flashing Examples

### Erase and reprogram a batch

```bash
# Wipe everything first
EspTool_Multi -p COM8,COM9,COM10 erase_flash

# Then flash fresh firmware
EspTool_Multi -p COM8,COM9,COM10 write_flash \
    0x0      bootloader.bin \
    0x8000   partitions.bin \
    0x10000  firmware.bin \
    0x300000 littlefs.bin
```

### Erase only the filesystem, then re-flash it

```bash
EspTool_Multi -p COM8,COM9,COM10 erase_region 0x300000 0xC0000
EspTool_Multi -p COM8,COM9,COM10 write_flash 0x300000 littlefs.bin
```

### Two boards

```bash
EspTool_Multi -p COM8,COM9 write_flash 0x0 bootloader.bin 0x10000 firmware.bin
```

### Production batch (5 boards)

```bash
EspTool_Multi -p COM3,COM4,COM5,COM6,COM7 write_flash \
    0x0      bootloader.bin \
    0x8000   partitions.bin \
    0x10000  firmware.bin \
    0x300000 filesystem.bin
```

### Fast batch, no verify

```bash
EspTool_Multi -p COM3,COM4,COM5,COM6,COM7 -b 921600 -n write_flash \
    0x0      bootloader.bin \
    0x10000  firmware.bin
```

### Slow and safe (long USB cables, hubs)

```bash
EspTool_Multi -p COM8,COM9,COM10 -b 115200 write_flash \
    0x0      bootloader.bin \
    0x8000   partitions.bin \
    0x10000  firmware.bin
```

---

## Console Output

During operation, real-time progress is printed per port:

```
[COM8 ] BOOT        syncing...
[COM9 ] BOOT        syncing...
[COM10] BOOT        syncing...
[COM8 ] BOOT   100% synced
[COM8 ] CHIP   100% ESP32c6
[COM8 ] STUB        uploading...
[COM8 ] STUB   100% complete
[COM8 ] BAUD   100% 460800
[COM9 ] BOOT   100% synced
...
[COM8 ] FLASH    0% 0x0
[COM9 ] FLASH    0% 0x0
[COM10] FLASH    0% 0x0
[COM8 ] FLASH   50% 0x0
[COM9 ] FLASH   50% 0x0
[COM10] FLASH   50% 0x0
[COM8 ] FLASH  100% 0x0
[COM9 ] FLASH  100% 0x0
[COM10] FLASH  100% 0x0
[COM8 ] VERIFY      0x0
[COM8 ] VERIFY 100% 0x0 OK
...
[COM8 ] RESET  100% complete
[COM9 ] RESET  100% complete
[COM10] RESET  100% complete

All ports flashed successfully.
```

### Progress Phases

| Phase    | Meaning                                        |
|----------|------------------------------------------------|
| `BOOT`   | Entering bootloader mode and syncing           |
| `CHIP`   | Detecting chip type                            |
| `STUB`   | Uploading stub flasher to RAM                  |
| `BAUD`   | Switching to target baud rate                  |
| `ERASE`  | Erasing entire flash memory                    |
| `FLASH`  | Writing compressed data to flash               |
| `VERIFY` | MD5 hash verification                          |
| `RESET`  | Hard-resetting the chip into normal boot       |

---

## Error Handling and Retry

If one or more ports fail during flashing, the tool:

1. **Continues** flashing all healthy ports to completion.
2. **Retries** failed ports once after the healthy ports finish.
3. **Prints a paste-ready retry command** for any ports that still fail.

### Example: partial failure

```
[COM8 ] FLASH  100% 0x10000
[COM9 ] FLASH   ERROR: Write failed at block 42
[COM10] FLASH  100% 0x10000
...
[COM9 ] RETRY       retrying...
[COM9 ] FLASH  100% 0x10000

All ports flashed successfully.
```

### Example: persistent failure

```
Some ports failed:
  COM9: Write failed at block 42

Retry with: EspTool_Multi -p COM9 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

You can copy-paste the retry command directly into your terminal.

---

## Exit Codes

| Code | Meaning                                               |
|------|-------------------------------------------------------|
| `0`  | All ports completed successfully.                     |
| `1`  | One or more ports failed, or invalid arguments.       |
| `2`  | Chip type mismatch — all ports must be the same chip. |
| `130` | Cancelled by user (Ctrl+C).                          |

### Using exit codes in scripts

```bash
# Bash
EspTool_Multi -p COM8,COM9 write_flash 0x10000 firmware.bin
if [ $? -eq 0 ]; then
    echo "Flash complete!"
elif [ $? -eq 2 ]; then
    echo "Mixed chip types detected — check your wiring."
fi

# PowerShell
& .\EspTool_Multi.exe -p COM8,COM9 write_flash 0x10000 firmware.bin
if ($LASTEXITCODE -eq 0) { Write-Host "Flash complete!" }
```

---

## Chip Support

All ports must have the **same chip type** (strict homogeneous validation). Supported chips:

- ESP32
- ESP32-C2
- ESP32-C3
- ESP32-C6
- ESP32-H2
- ESP32-S2
- ESP32-S3
- ESP8266

Detection uses `GET_SECURITY_INFO` (chip ID) for newer variants (C3/C6/S3/H2) and falls back to the magic register at `0x40001000` for classic chips (ESP32, ESP8266).

---

## Boot Sequence

The tool automatically tries two reset sequences to enter the bootloader:

1. **USB-Serial/JTAG** (tried first) — for chips with built-in USB (ESP32-C3, C6, S3, H2).
2. **Classic DTR/RTS** (fallback) — for boards with an external UART bridge (CP2102, CH340, etc.).

No user configuration is needed. Both sequences are tried automatically.

---

## Building from Source

```bash
# Debug build
dotnet build ESPTool.sln

# Self-contained single-file exe (13 MB, no .NET runtime needed)
dotnet publish EspTool_Multi/EspTool_Multi.csproj \
    -c Release -r win-x64 --self-contained true \
    -p:PublishSingleFile=true \
    -p:PublishTrimmed=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -o ./publish

# Linux
dotnet publish EspTool_Multi/EspTool_Multi.csproj \
    -c Release -r linux-x64 --self-contained true \
    -p:PublishSingleFile=true \
    -p:PublishTrimmed=true \
    -o ./publish
```

---

## Library Usage

`EspTool_Multi` is a thin CLI wrapper around the `ParallelFlasher` library class. For programmatic use:

```csharp
using EspDotNet;
using EspDotNet.Parallel;
using EspDotNet.Tools.Firmware;

var flasher = new ParallelFlasher(new ESPToolbox());
var ports = new[] { "COM8", "COM9", "COM10" };
var options = new ParallelFlashOptions { BaudRate = 460800, Verify = true };
var progress = new Progress<ParallelFlashProgress>(p =>
    Console.WriteLine($"[{p.Port}] {p.Phase} {p.Percent}% {p.Detail}"));

// Erase all boards
var eraseResult = await flasher.EraseAsync(ports, options, progress);

// Or erase just a region (offset, size)
var regionResult = await flasher.EraseRegionAsync(ports, 0x300000, 0xC0000, options, progress);

// Flash all boards
var firmware = new FirmwareProvider(
    entryPoint: 0x00000000,
    segments: new List<IFirmwareSegmentProvider>
    {
        new FirmwareSegmentProvider(0x0, File.ReadAllBytes("bootloader.bin")),
        new FirmwareSegmentProvider(0x10000, File.ReadAllBytes("firmware.bin")),
    });

var flashResult = await flasher.FlashAsync(ports, firmware, options, progress);

Console.WriteLine(flashResult.AllSucceeded ? "Done!" : flashResult.RetryCommand);
```
