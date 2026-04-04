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
7. **MESA I on OS 7** — Connects (TUR + INQUIRY succeed), but doesn't load data (OS version mismatch — MESA I needs S3000XL OS 1.x, device has 2.00)
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

### Theory F: XPRAM byte $00AF controls SCSI configuration

The Plug's capability function (dump 0x073E) reads XPRAM offset $00AF via trap $A051 (_ReadXPRam) after the .EDisk check passes. On SheepShaver, this byte is 0x00. On a real Mac with SCSI, it might be non-zero (SCSI configuration flag). If the Plug gates on this value, it would explain why .EDisk is found but SCSI is still "not available."

### Theory E: SCSI Plug never loaded / initialized correctly (LESS LIKELY)
The SCSI Plug is a system extension. The SCSI scans DO happen through SCSI Manager 4.3, and "Use MIDI" is grayed out (indicating the Plug detected SCSI capability). So the Plug IS active — it just fails the ".EDisk" check.

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

1. **Test XPRAM byte $00AF** — set XPRAM byte $AF to non-zero during InstallDrivers and check if the Plug's behavior changes.
2. **Fully disassemble the Plug's capability function** — understand every check between .EDisk and the final return value.
3. **Trace the Plug's actual decision** — add an emulation op at the Plug's function entry/exit to log what it returns at runtime.
4. **Compare with MESA I on OS 7** — MESA I connects via Old SCSI Manager. The difference between MESA I (works) and MESA II (doesn't) is the SCSI Plug. Understanding why MESA I's simpler path works may reveal what the Plug needs.
