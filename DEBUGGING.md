# SheepShaver SCSI Bridge — Debugging Notes

## Goals

### Primary: Understand the S3000XL SCSI conversation protocol

We need to observe a working, two-way conversation between a software editor (MESA II) and the Akai S3000XL hardware. The audiocontrol web editor can talk to the S3000XL via MIDI-over-SCSI, but we don't completely understand how the protocol works — particularly for writes that persist. Capturing MESA II's actual SCSI traffic will show us the correct conversation pattern so we can implement it in the audiocontrol web editor.

### Secondary: MESA II on modern hardware for the vintage sampler community

Provide the vintage Akai sampler community a way to run MESA II on modern hardware (via SheepShaver) and communicate with vintage S3000XL/S3200XL hardware over SCSI-over-network (via scsi2pi on a Raspberry Pi). This eliminates the need for a vintage Mac with a SCSI card.

## Architecture

```
SheepShaver (Docker, linux/amd64 via QEMU on Apple Silicon)
  → scsi_s2p.cpp (network SCSI backend, TCP)
  → s2p-midi on Raspberry Pi "s3k.local" (port 6868)
  → SCSI bus → Akai S3000XL (target ID 6)
```

## Repository Layout

- **macemu fork:** `/Users/orion/work/scsi2pi-work/macemu-os9-minimal` (branch `os9-minimal`)
- **scsi2pi fork:** `/Users/orion/work/scsi2pi-work/scsi2pi` (branch `feature/midi-processor`)
- **Container:** `sheepshaver-os9-minimal` on port 16082 (VNC: `vnc://localhost:16082`)

## Current State

### SCSI Bridge Driver (Phase 1 COMPLETE, Phase 2 IN PROGRESS)

A 68k Mac OS device driver (`.SCSI`) is installed in the unit table at refNum -49 using SheepShaver's emulation op pattern. The driver receives Device Manager traffic (Status polling, Control accRun) from Mac OS during boot.

**Feature documentation:** `docs/1.0/sheepshaver-scsi-driver/`

**What works:**
- Driver stub installed in ROM at `sony_offset + 0x700`
- Driver opens successfully at refNum -49 (`.EDisk` slot)
- Mac OS sends Status (csCode 10/12/13/17/18/28) and Control (csCode 65=accRun) calls
- Driver doesn't crash (after fixing logging throttle — 59K+ Status calls per boot)

**What doesn't work yet:**
- Driver returns noErr/statusErr without doing actual SCSI I/O
- MESA II's SCSI Plug does NOT route traffic through our driver (tested at both refNum -49 and -50)
- The Plug's `_Control` ioRefNum is still unknown

**Next: Phase 2 — mount an HFS disk image to validate SCSI I/O end-to-end:**
- 10MB HFS image created on Pi: `/home/orion/images/hfs_test.hds`
- Mounted in s2p at SCSI ID 0
- Need to implement Prime (READ/WRITE) and disk Status/Control handlers
- Success = Mac OS 9 mounts the volume on the desktop

### Root Cause: PPC Device Manager bypasses 68k trap table

The SCSI Plug patches `_Read`/`_Write`/`_Control`/`_Status` OS traps via `_SetOSTrapAddress` during its 68k INIT code. But SheepShaver's PPC Device Manager does NOT properly route through 68k trap table patches. The ROM's Mixed Mode dispatch at 0x140EE0 fails for 68k/UPP handlers.

**Evidence:**
- Plug's trap patches confirmed installed correctly in 68k trap table
- `_Control` patch gets overwritten by another extension (at ~0x1090xxxx)
- ReCycle's "search for samplers" generates ZERO log entries on any SCSI path
- ROM _Control dispatch at 0x140EF8/0x140F44/0x140F90 loads trap table but Mixed Mode fails
- Plug's handlers are Universal Procedure Pointers (mixed PPC/68k), not pure 68k — Execute68k crashes
- Patching the ROM `bl` to Mixed Mode causes crashes (infinite loop or illegal instructions)

**Solution approach:** Install a proper driver in the unit table. PPC _Control calls with a specific ioRefNum go through the unit table path (which works), bypassing the broken trap table path. This is how Sony/Disk/CDROM drivers already work from PPC.

### Unit Table at Boot

```
refNum=-2:  ".Sony"     drv=0x500f7a40
refNum=-4:  ".Sound"    drv=0x50108ea0
refNum=-5:  ".Sony"     drv=0x500f7a40
refNum=-49: ".EDisk"    drv=0x500dd5f0  ← Apple SCSI disk driver
refNum=-50: ".SCSI"     drv=0x500f8140  ← our bridge driver
refNum=-63: ".Disk"     drv=0x500f7b40
```

The Plug likely opens `.EDisk` by name during its 68k INIT and gets refNum -49. Our driver at -50 doesn't receive Plug traffic. We attempted installing at -49 but the Plug still didn't route to us — the Plug's refNum check needs further investigation.

## What Works

1. **SCSI network backend** — scsi_send_cmd forwards to s2p over TCP
2. **SCSI Manager 4.3 (SCSIAction)** — HandleSCSIAction + PPC thunks work
3. **Old SCSI Manager (SCSIDispatch)** — 68k path fully instrumented
4. **INQUIRY** — S3000XL found at target 6 by both Plug and System Profiler
5. **s2p emulated devices** — SCSI_EXEC now routes to emulated SCHD targets
6. **scsiDataResidual** — correctly computed from actual bytes transferred
7. **OldCall 0x86** — sends TEST UNIT READY to real targets
8. **SCSI bridge driver** — installed in unit table, receives Device Manager traffic

## Disproven Theories

| # | Theory | Result |
|---|--------|--------|
| A | Gestalt('mach') timing | Native value 0x43 already passes <= 0x7E check |
| B | OldCall 0x86 incomplete | Implemented TUR, no change |
| C | BusInquiry fields wrong | Fixed initiatorID to 7, no change |
| D | .EDisk DRVR missing | Already exists in resource chain |
| E | Plug not loaded | Plug IS loaded and scans via SCSIAction |
| F | XPRAM byte $AF | Set to 0x01, no change |
| G | Pre-init MIDI session | S3000XL accepted CDB 0x09, no change |
| H | INQUIRY byte 5 bit 5 | Patched, no change |
| I | OldCall with TUR | Status=0 for target 6, no change |
| J | _Control trap overwrite | Restored via guard, but PPC still bypasses |
| K | ROM _Control NativeOp | Plug handlers are UPPs, Execute68k crashes on PPC code |
| L | Driver at refNum -50 | Plug doesn't route traffic to -50 |
| M | Driver at refNum -49 | Plug doesn't route traffic to -49 either |

## MESA II Error Codes

Source: `~/tmp/Error Codes copy`

| Code | Define | Category | Meaning |
|------|--------|----------|---------|
| -13003 | err_ReplyLength | CAkaiMIDIDispatcher | MIDI reply wrong length |
| -14000 | err_scsiUnitRange | CSCSIUtils | SCSI unit out of range |
| -12001 | err_NoSamplerThere | CAkaiSampler | No sampler at target |
| -12002 | err_WrongTypeOfSampler | CAkaiSampler | Wrong sampler type |

Clicking SCSI icon in MESA II Disk window produces: -13003, -13003, -14000, -13003, -13003.
These errors occur WITHOUT any new scsi_send_cmd calls — the Plug fails internally before attempting SCSI I/O, because no sampler connection was established during init.

## Disk Images

- **MESA II disk:** snapshot at `sheepshaver-data/disk-mesa2-snapshot-20260404.zip`
- **ReCycle disk:** from `/Users/orion/Documents/SheepShaver/MacOS 9/Macintosh HD Recycle.zip`
- **Akai disk images:** `/home/orion/images/HD0-HD7.hds` on s3k.local
- **HFS test image:** `/home/orion/images/hfs_test.hds` on s3k.local (10MB, mounted at SCSI ID 0)

## Key Files

### SheepShaver (macemu-os9-minimal)
- `SheepShaver/src/emul_op.cpp` — HandleSCSIAction, SCSIDispatch, SCSI bridge dispatch, idle-time _Control guard
- `SheepShaver/src/scsi_bridge.cpp` — SCSI bridge driver implementation
- `SheepShaver/src/include/scsi_bridge.h` — driver header
- `SheepShaver/src/Unix/scsi_s2p.cpp` — Network SCSI backend
- `SheepShaver/src/rom_patches.cpp` — ROM patches, PPC thunks, driver stub + installation
- `SheepShaver/src/script_hook.cpp` — SHUTDOWN, Plug memory scanner

### scsi2pi (feature/midi-processor branch)
- `cpp/command/command_dispatcher.cpp` — SCSI_EXEC with emulated device routing

### Logging
- SheepShaver stderr → `/tmp/sheepshaver.log` (in container)
- SCSI trace → `{extfs}/scsi_trace.log`
- Plug binary dump → `sheepshaver-data/plug_full.bin` (32KB)

## Next Steps

1. **Phase 2: Implement SCSI READ/WRITE in the driver** to mount the HFS disk image. This validates the driver can do real I/O end-to-end through `scsi_send_cmd()`.

2. **Phase 3: Discover what ioRefNum the Plug uses.** Disassemble the Plug's _Control handler at dump offset 0x0E40 which checks `cmpi.w #-11,24(a0)` — this compares PB ioRefNum with -11. The Plug may intercept calls to refNum -11, not -49 or -50.

3. **Phase 4: Implement MIDI-over-SCSI** based on discovered protocol.
