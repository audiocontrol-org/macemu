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

- **Main fork:** `/Users/orion/work/scsi2pi-work/macemu` (branch `feature/scsi-network-bridge`)
  - Full ROM patches, breaks OS 7 boot, used for OS 9 container on port 16080
- **Minimal fork:** `/Users/orion/work/scsi2pi-work/macemu-os9-minimal` (branch `os9-minimal`)
  - Started from commit `8f67ef4e` (no ROM/thunk patches)
  - Added: HandleSCSIAction, SCSIDispatch tracing, Gestalt patches, SHUTDOWN command, PPC thunks
  - Boots both OS 7 and OS 9

## Docker Containers

| Container | Port | Disk | Source | Purpose |
|-----------|------|------|--------|---------|
| `sheepshaver-dev` | 16080 | OS 9 (sheepshaver-data/disk.hfv) | macemu (full fork) | Original OS 9 + MESA II |
| `sheepshaver-os9-minimal` | 16082 | OS 9 (sheepshaver-data/disk.hfv) | macemu-os9-minimal | Minimal patches for MESA II debugging |

All containers need `--privileged` for `vm.mmap_min_addr=0`.

## What Works

1. **SCSI network backend** — SheepShaver forwards SCSI commands to s2p over TCP
2. **Old SCSI Manager (SCSIDispatch)** — Fully instrumented, all calls logged with sequence numbers to stderr
3. **SCSI Manager 4.3 (SCSIAtomic/SCSIAction)** — HandleSCSIAction processes ExecIO, BusInquiry, etc., logged to scsi_trace.log
4. **INQUIRY** — S3000XL found at target 6, returns `AKAI EMIS3000XL SAMPLER 2.00`
5. **scsi_s2p fixes** — Removed incorrect `<< 1` status shift; CHECK CONDITION returns transport success
6. **SHUTDOWN command** — Write `SHUTDOWN` to `{extfs}/command.txt` for clean shutdown
7. **MESA I on OS 7** — Does NOT connect. TUR + INQUIRY succeed at SCSI level but MESA I does not establish a sampler connection.
8. **PPC SCSIAction thunks** — 5 thunks patched at ROM 0x150000-0x170000, confirmed in stderr
9. **Gestalt('scsi')** — Registered in InstallDrivers, returns gestaltAsyncSCSI flags
10. **ROM+0x12** — Patched from 0x28F1 to 0x2AF2

## What Doesn't Work

### MESA II "Not Online" — finds S3000XL but never sends MIDI-over-SCSI

MESA II (v1.2, OS 9) finds the S3000XL during SCSI scan (INQUIRY succeeds, both 36-byte and 96-byte). But shows "Not Online" and never sends MIDI-over-SCSI commands (CDB 0x09 init, 0x0C send, 0x0D poll, 0x0E read).

Both forks (full and minimal) exhibit identical behavior.

## Fixes Applied This Session

### 1. scsiDataResidual (FIXED)

ExecIO always set `scsiDataResidual = 0` even when the device returned fewer bytes than requested. For the 96-byte INQUIRY, S3000XL returns only 37 bytes but we told the caller all 96 bytes were valid. This caused the caller to read 59 bytes of memory garbage as if it were device data.

**Fix:** Added `size_t *actual_transferred` parameter to `scsi_send_cmd()`. The ExecIO handler now computes `scsiDataResidual = dataLength - actual`. Confirmed working in trace: `residual=60 (actual=36/96)` for the 96-byte INQUIRY.

Files changed: `scsi.h`, `scsi_s2p.cpp`, `emul_op.cpp`

### 2. Gestalt('mach') timing investigation (REVERTED)

**Theory:** The SCSI Plug checks Gestalt('mach') <= 0x7E at extension load time. Post-boot replacement via ScriptHookIdle is too late — the Plug has already cached "no SCSI."

**Finding:** The original Gestalt('mach') value is **0x43** (Power Mac 7200). Since 0x43 < 0x7E, the Plug's check should already pass without any replacement. **This means Gestalt('mach') is NOT the blocking issue.**

**What we tried (all reverted):**
- Moved Gestalt('mach') replacement to InstallDrivers → triggered "This startup disk will not work on this Macintosh model" dialog
- Counter-based handler (return original for first N calls, then 0x7E) with thresholds 2 and 5 → dialog still appeared because 'gbly' resource check uses Gestalt('mach') and 0x7E isn't a valid Power Mac type
- Patched _StopAlert (A986) trap to suppress dialog → dialog still appeared because PPC boot code doesn't go through 68k trap table for alerts
- Fixed _StopAlert stub with Pascal calling convention → still no effect, same reason

**Conclusion:** Gestalt('mach') replacement is unnecessary (0x43 already passes the <= 0x7E check) and harmful (triggers boot validation failure). Reverted all Gestalt('mach') changes. The post-boot replacement in ScriptHookIdle was also removed.

## SCSI Trace Analysis

The saved trace (`scsi_trace_mesa2_discovery.log`, 750 lines) shows three scan rounds:

### Round 1: SCSI Plug initial scan (targets 6→0)
- OldCall 0x86 per target (presence check) + ExecIO INQUIRY (36 bytes)
- Target 6: OldCall returns 0 (exists), INQUIRY returns S3000XL data
- Other targets: OldCall returns -7932, INQUIRY returns scsiNoTarget

### Round 2: Mac OS driver scan (targets 0→6)
- ExecIO INQUIRY (36 bytes) only, no OldCall pre-check
- Different PB addresses (0x10a99410, 0x109d4d90, 0x10a89b00)

### Round 3: MESA II extended scan (targets 0→6)
- ExecIO INQUIRY (96 bytes), different PB address (0x10e491a0)
- Target 6: 36 bytes valid data + 60 bytes of garbage (before residual fix)

### After all 3 rounds: ZERO additional SCSI calls

No CDB 0x09, 0x0C, 0x0D, or 0x0E ever appears. The Plug finds the device but never initiates MIDI-over-SCSI communication.

### Boot-time Old SCSI Manager activity (from stderr)

During boot, Mac OS's disk driver probes via the Old SCSI Manager path:
- SCSIGet → SCSISelect target=6 → SCSICmd CDB `08 00 00 00 01 00` (READ(6)) → SCSIRead → SCSIComplete stat=2 (CHECK CONDITION)
- This is the disk driver checking if target 6 is a disk. CHECK CONDITION is correct — S3000XL is a processor device.
- Scanning all other targets (5→0): SCSISelect returns 2 (scCommErr, target not present)

## Theories About What's Blocking MESA II

### Theory A: Gestalt('mach') timing (DISPROVEN)
The Plug caches its decision at extension load time. **Disproven** — 0x43 (the native value) already satisfies <= 0x7E.

### Theory B: OldCall 0x86 is incomplete
Our handler only returns "target exists" without executing the embedded old-style SCSI command. A real Mac's SCSI Manager 4.3 translates OldCall into a full SCSIExecIO. The Plug might rely on OldCall actually executing a command (like TEST UNIT READY) and checking the result.

### Theory C: BusInquiry response is wrong
Our BusInquiry response has:
- `scsiInitiatorID = 0` (real Macs use 7)
- `scsiFeatureFlags = 0` (real Macs advertise capabilities)
- The Plug might validate these fields.

### Theory D: Plug checks for ".EDisk" DRVR resource (DISPROVEN)

Disassembly of the Plug's SCSI capability check (at dump offset 0x06BE) reveals:

```
GetNamedResource('DRVR', "\p.EDisk")
```

If this returns NULL (no such resource), the function returns 0 and the Plug declares "no SCSI capability." This is the gate that prevents MIDI-over-SCSI initialization.

The Plug does NOT check Gestalt('mach'). The only Gestalt call in the dump is for 'ram ' (installed RAM), not 'mach'.

**DISPROVEN:** Verification via GetNamedResource during InstallDrivers returned 0x10011d5c (non-NULL). The ".EDisk" DRVR already exists in the System file resource chain. Our AddResource was unnecessary. The Plug's GetNamedResource call succeeds natively — this is NOT the blocker.

The function at dump offset 0x073E that calls the .EDisk check also reads XPRAM byte $00AF (value: 0x00 in current NVRAM) and calls Gestalt('ram '). The post-.EDisk code may use these values to make a further decision.

### Theory F: XPRAM byte $00AF controls SCSI configuration (DISPROVEN)

The Plug's capability function (dump 0x073E) reads XPRAM offset $00AF via trap $A051 (_ReadXPRam) after the .EDisk check passes. On SheepShaver, this byte is 0x00. Set it to 0x01 during InstallDrivers via _WriteXPRam. No change in behavior — still "Not Online", still only INQUIRY CDBs.

### Summary of disproven theories

| Theory | What | Result |
|--------|------|--------|
| A | Gestalt('mach') timing | Native value 0x43 already passes <= 0x7E check |
| B | OldCall 0x86 incomplete | Not tested yet |
| C | BusInquiry fields wrong | Fixed initiatorID to 7, no change |
| D | .EDisk DRVR missing | Already exists in resource chain (handle 0x10011d5c) |
| E | Plug not loaded | Plug IS loaded — SCSI scans happen, "Use MIDI" grayed |
| F | XPRAM byte $AF | Set to 0x01, no change |
| G | Pre-init MIDI session (CDB 0x09) | Sent during SCSIInit, S3000XL accepted (status=0), no change |
| H | Patch INQUIRY byte 5 bit 5 | Set bit 5 (0x20) in INQUIRY response byte 5, no change |
| I | Implement OldCall 0x86 with TUR | OldCall now sends TEST UNIT READY (status=0 for target 6), no change |
| J | Mount Akai disk images via s2p | s2p has HD0-HD7.hds mounted at IDs 0-5,7. SCSI_EXEC to emulated targets returns status=255 (not supported by s2p-midi). BUT: MESA II now shows errors -13003 and -14000! |

## MESA II Error Codes (from MESA documentation)

Source: `~/tmp/Error Codes copy`

| Code | Define | Meaning |
|------|--------|---------|
| -13003 | err_ReplyLength | MIDI reply had wrong length |
| -14000 | err_scsiUnitRange | SCSI unit out of range |
| -12000 | err_MIDITimedOut | MIDI timed out |
| -12001 | err_NoSamplerThere | No sampler found at target |
| -12002 | err_WrongTypeOfSampler | Wrong sampler type |
| -14001 | err_scsiStatus | SCSI status error |
| -14002 | err_scsiBusBusy | SCSI bus busy |

### Error -13003 (err_ReplyLength)
This is a CAkaiMIDIDispatcher error, not a CSCSIUtils error. It means MESA sent a MIDI message and got a reply with an unexpected length. This could be from:
1. The MIDI-over-SCSI path (CDB 0x0D poll returning wrong byte count)
2. The standard MIDI path (OMS) — but there's no MIDI interface in this OS 9 instance

### Error -14000 (err_scsiUnitRange)
CSCSIUtils error — SCSI unit ID is out of the valid range. This fires when the Plug tries to access emulated disk targets that our SCSI_EXEC can't reach (s2p-midi returns status 255 for emulated devices).

## Detailed Disassembly of Plug Function 0x10FC

This is the INQUIRY result handler. Parameters:
- a3 = caller's a4 (from 0x12AA: first param = a4 from ITS caller)
- a4 = caller's a3 (from 0x12AA: second param)

```
0x110C: moveq #7,d0
0x110E: and.w (a4+6),d0       ; d7 = low 3 bits of word at a4+6
0x1116: moveq #0x20,d0
0x1118: and.w (a3+4),d0       ; d1 = bit 5 of word at a3+4
0x1120: tst.l d1
0x1122: bne.s 0x1130           ; if d1 != 0, continue to device type check
        → ERROR: stores -28 in (a4+16), returns 0xE4

0x1130: d0 = d7 - 2
0x1134: if d7 < 2 → skip to 0x11A0
0x1136: if d7 > 5 → skip to 0x11A0
        → Jump table for d7 values 2-5 (device types?)

0x1150: (d7=2 or 3): handler for Processor/Tape types
```

**CRITICAL UNKNOWN: what do a3 and a4 actually point to?**
Assumed a3 = INQUIRY data, which would make (a3+4) = INQUIRY bytes 4-5 = 0x2000.
0x2000 AND 0x0020 = 0 → FAILS. But patching byte 5 to 0x20 makes word = 0x2020,
0x2020 AND 0x0020 = 0x0020 → PASSES. Yet MESA still shows "Not Online."

This means either:
1. a3 does NOT point to raw INQUIRY data (it points to a processed structure)
2. The check passes but a later check fails
3. The disassembly or offset calculation is wrong

### Theory E: SCSI Plug never loaded / initialized correctly (DISPROVEN)
The SCSI Plug IS loaded (found in memory at 0x1014E9DA) and IS active — it makes OldCall 0x86 and ExecIO INQUIRY calls during boot via SCSIAtomic (68k path, caller=0x101501DA). Note: "Use MIDI" is grayed because there's no MIDI interface in the OS 9 instance, NOT because the Plug detected SCSI.

## Key Files

### macemu-os9-minimal
- `SheepShaver/src/emul_op.cpp` — HandleSCSIAction, SCSIDispatch tracing, SCSIAtomic handler
- `SheepShaver/src/Unix/scsi_s2p.cpp` — Network SCSI backend with residual fix
- `SheepShaver/src/rom_patches.cpp` — ROM+0x12 patch, Gestalt('scsi') registration, PPC thunks
- `SheepShaver/src/script_hook.cpp` — SHUTDOWN command (Gestalt('mach') replacement removed)
- `SheepShaver/src/scsi.cpp` — Old SCSI Manager (SCSIGet/Select/Cmd/Read/Write/Complete)
- `SheepShaver/src/include/scsi.h` — Backend function declarations

### Pi (s3k.local)
- `/tmp/s2p-midi` — Custom s2p build with SCSI_EXEC support
- Start: `sudo /tmp/s2p-midi --port 6868`
- s2pexec: `/opt/scsi2pi/bin/s2pexec -i 6 -c CDB`

### Logging
- SheepShaver stderr → `/tmp/sheepshaver.log` (in container)
- SCSI trace → `{extfs}/scsi_trace.log` (shared folder, created by HandleSCSIAction)
- Script hook → `{extfs}/hook.log`
- Saved traces → `sheepshaver-data/scsi_trace_mesa2_discovery.log`
- PLUG binary dump → `sheepshaver-data/plug_memory.bin`

## Bisect Results

The commit that breaks OS 7: `25848e89` (SCSIAction handler with ROM patches).
The last good commit for OS 7: `8f67ef4e` (automation hooks, no ROM patches).

The specific changes that cause the hang:
- ROM+0x12 header word modification
- Trap 0xA089 redirection
- PPC SCSIAction thunk rewriting (pattern scan at ROM 0x150000-0x170000)
- Gestalt registration via Execute68kTrap during InstallDrivers

## Next Steps

## Runtime Trace Results

Inserted OP_PLUG_TRACE at the device type handler (dump 0x1150) and device state check (dump 0x115C). Key findings:

- **devtype3_handler fires repeatedly** — 2445 times for type 2 (Tape), 3 times for type 3 (Processor/S3000XL). This is a continuous polling loop.
- **a4 = 0x000003A4** — low-memory address, not a heap pointer. Should point to a per-device data structure. This value is suspicious and may indicate the Plug's data structures aren't properly initialized.
- **Capability function traces never fired** — the cap_entry/cap_return/caller functions ran during extension loading (before idle hook installed traces). Their behavior cannot be observed post-boot.
- **CAUTION**: Inserting OP_PLUG_TRACE replaces original instructions and breaks the Plug's behavior. The trace ops for devtype3_handler replaced `moveq #0,d0` (0x7000) and for devstate_check replaced `bne.s +16` (0x6610). These were removed.

## Next Steps

1. **Investigate a4=0x03A4** — is this a valid device context or a bug? Read memory at 0x03A4+$24 and 0x03A4+$28 to see what the Plug's state check finds there.
2. **Non-invasive tracing** — instead of patching Plug code, intercept SCSIAction to log the Mac stack/caller when the Plug makes calls. This reveals the Plug's call chain without modifying its code.
3. **Compare with MESA I on OS 7** — MESA I connects via Old SCSI Manager without the Plug. Understanding why MESA I's simpler path works may reveal what the Plug needs.
4. **Check if MESA II needs UI interaction** — the device shows "Not Online" — maybe the user must click on it to trigger connection. Automate this via xdotool (need correct window coordinates).
