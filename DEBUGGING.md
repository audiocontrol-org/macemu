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

### SCSI Bridge Driver (Phase 1+2 COMPLETE)

A 68k Mac OS device driver (`.SCSI` at refNum -50, `.EDisk` at refNum -49) is installed in the unit table. The driver handles READ/WRITE to a 10MB HFS disk image on the Pi via scsi_send_cmd(). Mac OS 9 mounts the volume ("Untitled") on the desktop, reads/writes survive reboots.

### MESA II's SCSI Plug (Phase 3 IN PROGRESS)

**MESA II has its own SCSI Plug** (`MESA Pouch:PlugIns:SCSI Plug 2.1.2`, 12KB, 68k code, type PLUG/AK11). This is completely separate from the system SCSI driver at 0x1014EA0A (which we were wrongly patching for days). Extracted copy: `sheepshaver-data/scsi_plug_original.bin`.

**Constructor flow** (file offset 0x06C8):
```
06C8: MOVEM.L prologue
06D0: Check (a4+0x266) — if non-zero, skip init (already initialized)
06E0: clr.b (a4+0x26A)         — clear scsi43_flag
06E4: GetToolTrapAddress($A89F) — check if SCSIAtomic trap is implemented
06EC: GetOSTrapAddress($A198)   — get _Unimplemented address
06F2: Compare — if $A89F != _Unimplemented, set flag to 1
06F8: move.b d0,(a4+0x26A)    — store scsi43_flag
06FC-070A: Call two init functions (address computation + StripAddress)
070E: Store init marker at (a4+0x266)
0712: tst.b (a4+0x26A)        — check scsi43_flag
0716: beq.s +4                — if 0, skip $A198 call
0718: moveq #1,d0; $A198      — call when SCSI Manager 4.3 available
071C: MOVEM.L epilogue; RTS
```

**Known facts:**
- $A89F IS implemented (0x500187ce != _Unimplemented 0x500809f0)
- So scsi43_flag SHOULD be set to 1
- MESA generates ZERO `_SCSIDispatch` ($A089) calls — the scan never reaches SCSICommand
- `$A198` is called when scsi43_flag == 1 — purpose unknown (might be IdentifyBusses trigger)
- MESA uses ONLY `$A089` for SCSI calls (no `$A815`/`$A89F` in the binary)

**Unsolved:** We cannot trace the Plug's execution to find WHERE it fails. Need instrumentation.

### Instrumentation attempts (all failed)

| Approach | Result |
|----------|--------|
| SheepShaver emulation ops (0xFExx) in Plug file | Error type 12 (illegal instruction) — emulation ops don't work in Mixed Mode's 68k context |
| A-line trap $ABFF → handler with emulation op | "SheepShaver Warning ◆P" — emulation ops in trap handler also fail in Mixed Mode |
| A-line trap $ABFF → pure 68k handler (RTS only) | Still triggers "◆P" warning — SheepShaver's PPC emulator intercepts unknown traps before 68k dispatcher |
| In-memory patching via FIND_PLUG command | File offsets don't match memory layout due to PEF relocation; code and data sections loaded at different addresses |
| Trampoline patches in file padding area | Trampolines were beyond the 12053-byte logical file size — never loaded into memory |

**What works for instrumentation:**
- FIND_PLUG command finds the Plug's data in memory at 0x17badxxx
- Pattern search (`303C A89F A746`) finds the Plug's CODE section at 0x10069xxx
- These are at DIFFERENT addresses (PEF loader separates code and data)
- In-memory patching with `$ABFF` traps is possible but causes "◆P" warnings

**NEW FINDING — likely root cause:**
The `_SCSIDispatch` handler at ROM `base+22` contains `M68K_EMUL_OP_SCSI_DISPATCH` (0xFE7D) — an emulation op that only works in SheepShaver's native 68k context. When MESA's 68k Plug (running in Mixed Mode) calls `$A089`, the trap dispatcher jumps to this handler, hits the emulation op, and fails. This is the SAME problem as our Plug patching attempts (emulation ops crash in Mixed Mode with error type 12).

The system SCSI driver works because it calls SCSIAction through PPC thunks (OP_SCSI_ATOMIC), bypassing the 68k trap entirely. MESA's 68k Plug is the only code that calls `$A089` as a 68k trap — and it fails.

**The fix:** Replace the `_SCSIDispatch` handler with pure 68k code that works in Mixed Mode, then routes to our C handler through a mechanism that works in both contexts (e.g., writing to shared memory polled by idle handler, or calling through a PPC callback).

**Alternative approaches:**
- Write a PPC wrapper PLUG that replaces the 68k SCSI Plug
- Use SheepShaver's PPC instruction tracer for the MESA code range
- Make the 68k trap handler call through the PPC SCSIAction path instead

### Previous analysis (system SCSI driver — NOT MESA's Plug)

The binary at 0x1014EA0A is the **Mac OS system SCSI driver** (contains ".EDisk", "Mac OS 9.0b9"), not MESA's Plug. The 44 SCSIAtomic calls during boot are from the system SCSI Manager, not MESA. The BNE→BRA patches and device type patches affected the system driver — irrelevant to MESA's scan path.

## What Works

1. **SCSI network backend** — scsi_send_cmd forwards to s2p over TCP
2. **SCSI Manager 4.3 (SCSIAction)** — HandleSCSIAction + PPC thunks work
3. **Old SCSI Manager (SCSIDispatch)** — 68k path fully instrumented
4. **INQUIRY** — S3000XL found at target 6 by both Plug and System Profiler
5. **s2p emulated devices** — SCSI_EXEC now routes to emulated SCHD targets
6. **scsiDataResidual** — correctly computed from actual bytes transferred
7. **OldCall 0x86** — sends TEST UNIT READY to real targets
8. **SCSI bridge driver** — installed in unit table, HFS disk mounts, read/write works
9. **INQUIRY device type patch** — changing type 0x03→0x00 during boot makes Plug run Disk init handler (populates +0x24)
10. **BNE→BRA patches** — bypass NULL handler checks at Plug+0x115C and +0x123C
11. **Keystroke injection** — alt+F sends Cmd+F to MESA II (Find Sampler) after title bar click for X11 focus

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
| N | BNE→BRA patches alone | Bypasses NULL check but +0x24 still NULL, registration fails downstream |
| O | Device type 0x03→0x00 (always) | Plug registers device but MESA skips it (expects type 0x03) |
| P | Device type 0x03→0x00 (boot only) | Plug registers as Disk, stores type 0x00 in record, MESA skips |
| Q | Binary at 0x1014EA0A is MESA's Plug | Contains ".EDisk", "Mac OS 9.0b9" — it's the SYSTEM SCSI driver, NOT MESA |
| R | Processor handler at 0x10159494 is SCSI device init | Contains "aevt", "quit", "odoc" — it's an APPLICATION's Apple Event handler |
| S | BNE→BRA patches + device type patches fix MESA | These patch the system driver, not MESA's Plug. MESA has its own SCSI Plug in MESA Pouch:PlugIns |
| T | $A89F is unimplemented, blocking MESA | $A89F=0x500187ce, _Unimplemented=0x500809f0 — they differ, trap IS implemented |
| U | Overwriting $A89F with our handler | Crashes boot ("unimplemented trap") — the existing ROM handler must stay |
| V | d0=1 in $A089 = SCSIAction | d0=1 for ALL OP_SCSI_DISPATCH calls (old SCSI Manager too) — not a discriminator |
| W | MESA calls Gestalt('scsi') | MESA's Plug checks GetToolTrapAddress($A89F) directly, not Gestalt |
| X | Emulation ops (0xFExx) in Plug code | Error type 12 — doesn't work in Mixed Mode 68k context |
| Y | A-line trap $ABFF for tracing | "SheepShaver Warning ◆P" — PPC emulator intercepts unknown traps. Also crashes boot (overwrites system trap) |
| Z | Writing handler to OS trap table 0x0624 | Handler NOT reached from Mixed Mode. Marker test: wrote 0xDEADBEEF marker to handler, value stayed 0x00000000 after Find Sampler. Mixed Mode bypasses the OS trap table at 0x0400 |

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
- SheepShaver stderr → `sheepshaver-data/sheepshaver.log` (host-accessible via shared volume)
- Script hook log → `sheepshaver-data/hook.log`
- Plug binary dump → `sheepshaver-data/plug_full.bin` (32KB)
- PPC code dump → `sheepshaver-data/ppc_code.bin` (128KB)

### Test Cycle Script

`scripts/rebuild-and-boot.sh` automates the build-restart-wait cycle:

```bash
./scripts/rebuild-and-boot.sh              # build, restart, wait for boot
./scripts/rebuild-and-boot.sh --no-wait    # build, restart, exit immediately
./scripts/rebuild-and-boot.sh --log-only   # tail the log (no build/boot)
```

The script:
1. **Shutdown**: sends `SHUTDOWN` via `command.txt` → script_hook calls `_ShutDown` trap → SheepShaver exits. Falls back to `kill -9` after 60s. Waits for exit via `tail --pid` (event-based, not polling).
2. Builds SheepShaver (`make -j`) in the container
3. Truncates log files
4. Starts SheepShaver with stderr redirected to `sheepshaver-data/sheepshaver.log`
5. Dismisses Disk First Aid dialog via xdotool Return key
6. Waits for boot completion by watching `hook.log` for "Script hook initialized" (event-based, not polling)

### Graceful Shutdown (TODO)

The `_ShutDown` trap exits SheepShaver immediately without the full Mac OS 9 shutdown sequence (close apps, flush caches, unmount volumes). ROM patches in `rom_patches.cpp` short-circuit the Shutdown Manager (FE0A→RTS) and PowerOff (→`M68K_EMUL_RETURN`).

**What works manually:** A "Shutdown" AppleScript classic applet (`tell application "Finder" to shut down`) saved on the Unix ExtFS volume. Double-clicking it triggers a proper Finder shutdown sequence.

**What doesn't work programmatically (all crash from OP_IDLE_TIME/EMUL_OP context):**
- Inline 68k AEM calls (trap $A816 with AECreateDesc/AECreateAppleEvent/AESend selectors) — first attempt appeared to trigger Finder shutdown (MESA quit dialog appeared), subsequent attempts caused system errors
- Inline 68k FSMakeFSSpec ($A260/_HFSDispatch) + LaunchApplication ($A88F/_OSDispatch) — system lockup
- CallMacOS via CFM TVectors (FindLibSymbol + CallMacOS4/CallMacOS1) — system lockup
- Inline 68k AEM with $A9EF instead of $A816 — Finder crash (error 8553)

**Potential approaches not yet tried:**
- Run the Shutdown applet from Shutdown Items folder (runs during _ShutDown sequence)
- Use ADB power key injection to trigger the native shutdown dialog
- Send Apple Events from a DIFFERENT execution context (not OP_IDLE_TIME)
- Fix the ROM patches to allow the full shutdown sequence to complete

### Sending Keystrokes to Mac OS 9

To send keyboard shortcuts (e.g., Cmd+F for MESA II's Find Sampler) from the host:

```bash
# 1. Click the SheepShaver TITLE BAR to give X11 focus without affecting Mac app focus
docker exec sheepshaver-os9-minimal bash -c 'export DISPLAY=:0; xdotool mousemove 400 155 click 1'

# 2. Send the keystroke — alt = Mac Command key in SheepShaver's SDL mapping
docker exec sheepshaver-os9-minimal bash -c 'export DISPLAY=:0; xdotool key alt+f'
```

Key mappings: `alt` = Mac Command, `super` does NOT work. The title bar click (y≈155) gives X11 focus to the SheepShaver window without clicking inside the Mac desktop (which would change the foreground Mac app). The `--window` flag on xdotool sends `XSendEvent` which SDL ignores — must use real key events.

To bring MESA II to the foreground without disrupting it, click the app switcher menu (top-right of Mac OS 9 menu bar, ~x=810 y=177) and select MESA II:

```bash
# Click app switcher, then click MESA II entry
docker exec sheepshaver-os9-minimal bash -c 'export DISPLAY=:0; xdotool mousemove 810 177 click 1; sleep 0.5; xdotool mousemove 770 268 click 1'
```

Alternatively, double-click the "MESA II alias" icon on the desktop (visible in Finder) to bring MESA to the foreground if it's already running.

## Next Steps

1. **Instrument MESA's SCSI Plug to trace execution.** The Plug's 68k code runs in Mixed Mode where SheepShaver emulation ops don't work and unknown A-line traps cause warnings. Approaches to try:
   - Intercept a KNOWN working trap (e.g., `_SCSIDispatch` $A089 or `_StripAddress` $A055) that the Plug calls, and log the caller address to identify which Plug function is executing
   - Use SheepShaver's PPC instruction tracer to log execution in the MESA code range
   - Write a PPC wrapper PLUG that loads the original 68k PLUG and instruments its calls
   - Patch the Plug to call a trap we control (need to find one that doesn't trigger "◆P")

2. **Determine why MESA generates zero SCSI calls.** The $A89F trap IS implemented, the scsi43_flag SHOULD be set, but no `$A089` calls come from MESA. Either the flag isn't actually set (Mixed Mode difference) or something after the flag set prevents scanning.

3. **Once MESA finds the S3000XL:** capture the MIDI-over-SCSI conversation (CDB 0x09/0x0C/0x0D/0x0E) to understand the protocol for the audiocontrol web editor.
