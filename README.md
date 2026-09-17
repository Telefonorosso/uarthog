# Emu68 USB Logging Console - AI CODED

**A persistent USB debug console for Emu68 — early boot logs, runtime status, live debug control, crash dumps and reboot from a standard serial terminal.**

This experiment exposes Emu68 itself as a **USB CDC ACM serial device** over the Raspberry Pi 3A+ USB device port.

Connect PiStorm to a PC and Emu68 becomes a serial console without requiring the traditional Pi serial cable.

The same connection can be used for:

- Emu68 boot logging;
- early-boot `kprintf()` output;
- runtime status;
- enabling/disabling Emu68 debug output;
- restricting debug output to an address range;
- selected 68k exception crash dumps;
- rebooting the complete PiStorm/Emu68 system.

---

## What it looks like

On the host, Emu68 enumerates as a normal USB serial / CDC ACM device.

The current firmware identifies itself with the banner:

```text
EMU68-USB-CDC-POC14-DEBUG-STATUS-RANGE-REBOOT-EARLYRING-CRLF READY
```

After opening the serial port in PuTTY, Tera Term, `screen`, `minicom` or another serial terminal, Emu68 logs appear directly in the terminal.

No AmigaOS program is required.

---

## Why this is useful

Low-level Emu68 development often happens before AmigaOS has started, and many problems can make the normal Amiga environment unusable.

Traditionally that means depending on a dedicated serial/debug connection.

This POC makes the Raspberry Pi USB port itself the diagnostic connection:

```text
Emu68 / PiStorm
      |
      v
 BCM2837 DWC2
      |
      v
 USB CDC ACM
      |
      v
 PC serial terminal
```

This is especially convenient for machines where a permanent serial setup is undesirable.

---

## Early boot logging

One of the most useful parts of the implementation is that logging begins **before USB has finished enumerating**.

`kprintf()` output is placed into an in-memory ring from the earliest boot messages.

Once the PC has configured the CDC ACM device, Emu68 drains the accumulated backlog to the terminal.

Conceptually:

```text
Emu68 starts
    |
    v
early kprintf()
    |
    v
USB log ring
    |
    | USB not ready yet
    |
    v
CDC ACM enumerates
    |
    v
backlog is transmitted
    |
    v
live log continues
```

This avoids losing precisely the boot messages that are often most useful during debugging.

If the ring fills, old data is overwritten rather than allowing diagnostic output to block Emu68.

---

## Runtime ownership

The USB console is intentionally kept away from the normal 68k execution path.

After initial enumeration, the PiStorm Classic CPU2 housekeeper becomes the sole runtime owner of the DWC2 polling path.

`kprintf()` itself does **not** access DWC2 registers.

It only places characters in the software logging ring.

This avoids concurrent USB controller access from multiple cores.

---

# Commands

The CDC console also accepts a small set of commands.

Commands are ASCII and case-insensitive.

---

## `PING`

Simple connectivity test:

```text
PING
```

Response:

```text
PONG
```

This is the quickest way to verify that USB RX and TX are both working.

---

## `STATUS`

Displays a compact Emu68 runtime status report:

```text
STATUS
```

The report includes:

```text
Temperature
Core voltage
JIT cache usage
JIT unit count
JIT cache misses
USB CDC configuration state
USB speed
USB maximum packet size
pending USB log backlog
```

Example format:

```text
EMU68 STATUS
Temperature: 52.3 C
Core voltage: 1200 mV
JIT cache: 18.4% used (...)
JIT units: ...
Cache misses: ... total
USB CDC: configured, high-speed, MPS 512
USB log backlog: 0 bytes
```

The JIT information is read directly from Emu68's internal state. It does not execute diagnostic instructions on the emulated 68k.

The status snapshot is deliberately lightweight and read-only.

---

## `DEBUG`

Shows the current Emu68 debug state and address-range filter:

```text
DEBUG
```

Typical result:

```text
DEBUG OFF
RANGE OFF
```

or:

```text
DEBUG ON
RANGE 00F80000-00FFFFFF
```

---

## `DEBUG ON`

Enables Emu68's normal debug output:

```text
DEBUG ON
```

Response:

```text
OK DEBUG ON
```

This controls the same internal Emu68 `debug` state used by the existing debug facilities.

---

## `DEBUG OFF`

Disable debug logging:

```text
DEBUG OFF
```

Response:

```text
OK DEBUG OFF
```

---

## `DEBUG RANGE`

Debug output can be restricted to translated code within a specific 68k address range.

Syntax:

```text
DEBUG RANGE <start> <end>
```

For example:

```text
DEBUG RANGE 00F80000 00FFFFFF
```

Hexadecimal values may also use the `0x` prefix:

```text
DEBUG RANGE 0x00F80000 0x00FFFFFF
```

The current range can be inspected with:

```text
DEBUG
```

---

## `DEBUG RANGE OFF`

Restores the normal unrestricted debug address range:

```text
DEBUG RANGE OFF
```

Response:

```text
OK DEBUG RANGE OFF
```

This does not necessarily disable debug itself; it removes only the address filter.

Use:

```text
DEBUG OFF
```

to disable debug output completely.

---

## `REBOOT`

Performs a complete PiStorm/Emu68 reboot:

```text
REBOOT
```

The console responds:

```text
rebooting in 3 seconds
```

The delay gives the reply time to reach the host before the machine resets.

The implementation calls the PiStorm Classic full-reboot path rather than merely resetting the emulated 68k.

If a reboot is already scheduled:

```text
reboot already pending
```

is returned.

---

# 68k crash dumps

The milestone also adds a diagnostic path for selected crash-like 68k exceptions.

When one of the selected exceptions occurs, Emu68 emits a native SVC diagnostic before constructing the normal guest exception frame.

Examples include:

- illegal instruction;
- divide by zero;
- CHK;
- Line-A;
- Line-F;
- format error;
- selected floating-point exceptions.

The AArch64 exception-vector code captures the live translated 68k context and prints a diagnostic dump through the normal Emu68 logging path.

Because the USB console receives `kprintf()` output, the crash information appears directly in the host terminal.

The normal guest exception mechanism remains in place after the diagnostic.

This makes the USB console particularly useful for faults where AmigaOS itself can no longer provide meaningful debugging information.

---

# Windows usage

After booting the modified Emu68 firmware, connect the Raspberry Pi USB device port to the PC.

Windows should expose a new serial COM port through its standard USB CDC/serial support.

Check:

```text
Device Manager
→ Ports (COM & LPT)
```

and identify the Emu68 USB serial device.

Open the corresponding COM port with a terminal application such as:

- PuTTY;
- Tera Term;
- Windows Terminal with an appropriate serial tool;
- any other CDC ACM-compatible terminal.

The baud rate setting is largely nominal for USB CDC ACM because this is USB rather than a physical UART.

A conventional value such as:

```text
115200 8N1
```

is fine.

The firmware translates line endings to CRLF on the USB side for normal Windows terminal display.

---

# Linux usage

Linux normally binds CDC ACM devices to:

```text
/dev/ttyACM0
```

or another `ttyACM` number.

For example:

```bash
screen /dev/ttyACM0 115200
```

or:

```bash
minicom -D /dev/ttyACM0
```

The exact device number depends on other USB serial devices connected to the host.

---

## USB implementation

The current implementation uses the Raspberry Pi 3A+ **BCM2837 DWC2** controller directly in device mode.

It implements a minimal CDC ACM device specifically for the Emu68 console.

The current POC uses:

```text
VID:PID 0525:A4A7
```

and supports High-Speed operation when negotiated.

The runtime implementation is polling-based rather than interrupt-driven.

That is intentional for this diagnostic POC: simplicity and predictable ownership are more important than throughput.

A logging console requires very little bandwidth.

---

## Log ring behaviour

Logging must never stop the emulator.

The USB logging ring therefore follows a deliberately lossy diagnostic policy:

```text
normal case:
    append log
    CPU2 sends it over USB

host slow/disconnected:
    ring accumulates data

ring full:
    oldest diagnostic data may be discarded
```

Losing an old debug message is preferable to blocking the Emu68 execution path.

---

## Source layout

The POC modifies five Emu68 source files:

```text
src/
├── M68k_Exception.c
├── aarch64/
│   └── vectors.c
├── pistorm/
│   └── ps_classic_protocol.c
└── raspi/
    ├── start_rpi64.c
    └── support_rpi.c
```

### `src/raspi/start_rpi64.c`

Contains the USB CDC ACM implementation, early-log ring, console command parser, status reporting, debug-range controls and USB runtime polling.

### `src/raspi/support_rpi.c`

Routes normal PiStorm Classic `kprintf()` output into the USB logging path and provides the Raspberry Pi firmware helpers used by `STATUS`.

### `src/pistorm/ps_classic_protocol.c`

Provides the CPU2 housekeeper integration and the full PiStorm reboot path used by the `REBOOT` command.

### `src/aarch64/vectors.c`

Adds the native exception-side support required to print translated 68k crash context.

### `src/M68k_Exception.c`

Selects crash-like guest exceptions and injects the diagnostic SVC before normal exception delivery.

---

## Building

Overlay the supplied files onto a compatible Emu68 source tree, preserving their directory structure.

Then use the normal PiStorm Classic build:

```bash
cmake -S . -B build \
  -DTARGET=raspi64 \
  -DVARIANT=pistorm-classic \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake
```

Build:

```bash
cmake --build build -j$(nproc)
```

As with all low-level experimental Emu68 firmware, keep a known-good image available for rollback.

---

## Current status

This milestone demonstrates:

- USB CDC ACM device mode;
- persistent host-visible Emu68 logging;
- early-boot log buffering;
- CRLF-compatible Windows terminal output;
- bidirectional command console;
- `PING`;
- `STATUS`;
- Pi temperature and core-voltage reporting;
- JIT cache statistics;
- `DEBUG ON/OFF`;
- address-restricted debug logging;
- delayed full-system `REBOOT`;
- selected 68k exception crash dumps;
- CPU2-owned runtime DWC2 polling.

It is intended as a debugging and development tool rather than a general-purpose serial implementation.

---

## Why it matters

PiStorm makes unusually deep integration between the Amiga and Raspberry Pi possible.

This experiment turns that integration back onto Emu68 itself.

Instead of needing extra debug hardware, the accelerator can expose its own internal state through the same USB connector already present on the Pi.

For developers, that means one cable can provide visibility from the earliest Emu68 boot messages through runtime JIT diagnostics and guest crashes.

---

## Credits

This is an unofficial experimental Emu68 / PiStorm extension.

It is not an official Emu68 release.

The project builds on:

- Emu68 by Michal Schulz and contributors;
- PiStorm;
- the Raspberry Pi BCM2837 DWC2 USB controller;
- the standard USB CDC ACM device model.

Existing source files retain their original authorship and license notices.

Upstream Emu68:

```text
https://github.com/michalsc/Emu68
```

---

## License

The modified Emu68 files retain their respective upstream licenses.

Check the source headers and the Emu68 repository licensing terms before redistribution.
