# SheepShaver SCSI Bridge — Debugging Notes

## Goal

Capture MESA II's SCSI traffic to the Akai S3000XL to understand why SysEx-over-SCSI writes don't persist. The existing implementation (in `audiocontrol-scsi-midi-bridge`) can read from the S3000XL but writes don't stick. MESA presumably handles writes correctly, so capturing its traffic will reveal the difference.

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

## Current State: ROOT CAUSE IDENTIFIED

### The Problem

PPC applications (MESA II, ReCycle) cannot communicate with SCSI samplers because the SCSI Plug's Device Manager trap hooks are invisible to PPC code on SheepShaver.

### How It Works on a Real Mac

1. The SCSI Plug (system extension) patches `_Read`, `_Write`, `_Control`, `_Status` OS traps via `_SetOSTrapAddress` during its 68k INIT code
2. The Plug's hooks intercept Device Manager calls to the SCSI driver
3. When an app calls `_Control` to send a SCSI command, the Plug's hook handles MIDI-over-SCSI
4. On a real PPC Mac, the ROM's PPC Device Manager reads the 68k trap table and calls patched handlers through Mixed Mode

### How It Fails on SheepShaver

1. The Plug installs its 68k trap patches correctly (verified: trap table entries point to Plug handlers)
2. **But:** SheepShaver's PPC code path for Device Manager calls does NOT properly route through the patched 68k trap table
3. The ROM at 0x140EF8/0x140F44/0x140F90 loads the 68k handler address via `lwz r4,0x0410(r0)` and calls a Mixed Mode dispatcher at 0x140EE0
4. The Mixed Mode dispatcher fails to properly call the 68k handler — it tries to execute the address as PPC code, hitting illegal instructions

### Evidence

- MESA II's SCSI Plug IS loaded at ~0x1014E9DA, patches _Read/_Write/_Status correctly
- _Control patch gets overwritten by another extension (at ~0x1090xxxx), but even restoring it doesn't help
- The Plug's INIT scan via SCSIAction (INQUIRY) works — finds S3000XL
- "Find Sampler" returns instantly (cached "no samplers") — Plug decided at init time
- Clicking SCSI icon in Disk window → error -13003 (err_ReplyLength) but ZERO new scsi_send_cmd calls
- ReCycle (different app, different disk image) has the same behavior — zero SCSI commands from its sampler search
- System Profiler correctly enumerates all SCSI devices (uses SCSIAction, which DOES work)
- Patching the ROM's `bl 0x140EE0` to a native handler confirmed: handler IS called with correct PB, but calling Execute68k with the PPC handler address crashes, and calling Execute68kTrap(A004) causes infinite loop

### The Plug's Trap Patches (from binary analysis)

Located at Plug dump offset 0x0D18:
```
lea (pc+0x46),a0     → handler for _Read at Plug+0x0D60
move.w #$A002,d0     
_SetOSTrapAddress     ; patch _Read

lea (pc+0x70),a0     → handler for _Write at Plug+0x0D94
move.w #$A003,d0
_SetOSTrapAddress     ; patch _Write

lea (pc+0xF2),a0     → handler for _Control at Plug+0x0E20
move.w #$A004,d0
_SetOSTrapAddress     ; patch _Control

lea (pc+0x90),a0     → handler for _Status at Plug+0x0DC8
move.w #$A005,d0
_SetOSTrapAddress     ; patch _Status
```

### ROM PPC _Control Dispatch (3 identical sites)

At ROM 0x140EF8:
```
mfspr r0, LR
lwz   r4, 0x0410(r0)    ; load _Control handler from 68k trap table
stwu  r1, -64(r1)
stw   r0, 0x48(r1)
...
addi  r3, r4, 0          ; r3 = handler addr
addi  r6, r3, 0          ; r6 = PB (from original r3)
...
bl    0x140EE0           ; call Mixed Mode dispatcher ← THIS FAILS
```

## What Works

1. **SCSI network backend** — scsi_send_cmd forwards to s2p over TCP
2. **SCSI Manager 4.3 (SCSIAction)** — HandleSCSIAction + PPC thunks work
3. **Old SCSI Manager (SCSIDispatch)** — 68k path fully instrumented
4. **INQUIRY** — S3000XL found at target 6 by both Plug and System Profiler
5. **s2p emulated devices** — SCSI_EXEC now routes to emulated SCHD targets (fixed in scsi2pi)
6. **scsiDataResidual** — correctly computed from actual bytes transferred
7. **OldCall 0x86** — sends TEST UNIT READY to real targets

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

## MESA II Error Codes

Source: `~/tmp/Error Codes copy`

| Code | Define | Category | Meaning |
|------|--------|----------|---------|
| -13003 | err_ReplyLength | CAkaiMIDIDispatcher | MIDI reply wrong length |
| -14000 | err_scsiUnitRange | CSCSIUtils | SCSI unit out of range |
| -12001 | err_NoSamplerThere | CAkaiSampler | No sampler at target |
| -12002 | err_WrongTypeOfSampler | CAkaiSampler | Wrong sampler type |

## Disk Images

- **MESA II disk:** snapshot at `sheepshaver-data/disk-mesa2-snapshot-20260404.zip`
- **ReCycle disk:** from `/Users/orion/Documents/SheepShaver/MacOS 9/Macintosh HD Recycle.zip`
- **Akai disk images:** `/home/orion/images/HD0-HD7.hds` on s3k.local

## Key Files

### SheepShaver (macemu-os9-minimal)
- `SheepShaver/src/emul_op.cpp` — HandleSCSIAction, SCSIDispatch, idle-time _Control guard
- `SheepShaver/src/Unix/scsi_s2p.cpp` — Network SCSI backend
- `SheepShaver/src/rom_patches.cpp` — ROM patches, PPC thunks, Gestalt registration
- `SheepShaver/src/script_hook.cpp` — SHUTDOWN, Plug memory scanner
- `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` — NATIVE_SCSI_ACTION, NATIVE_CONTROL_DISPATCH

### scsi2pi (feature/midi-processor branch)
- `cpp/command/command_dispatcher.cpp` — SCSI_EXEC with emulated device routing

### Logging
- SheepShaver stderr → `/tmp/sheepshaver.log` (in container)
- SCSI trace → `{extfs}/scsi_trace.log`
- Plug binary dump → `sheepshaver-data/plug_full.bin` (32KB)

## Next Steps

1. **Fix SheepShaver's Mixed Mode dispatch for Device Manager traps.** The ROM at 0x140EE0 calls a PPC function to dispatch through Mixed Mode. On SheepShaver, this doesn't work for 68k handler addresses from the trap table. Need to understand WHY the Mixed Mode call fails and fix it. This is the critical path.

2. **Alternative: install a PPC SCSI driver in the unit table** that handles _Read/_Write/_Control by calling our scsi_send_cmd backend directly. This bypasses the Plug entirely and provides SCSI I/O to PPC apps without needing Mixed Mode to work. More work but more reliable.

3. **Alternative: patch the Plug to use SCSIAction instead of Device Manager** for its MIDI-over-SCSI commands. Would require modifying the Plug's 68k code in memory.
