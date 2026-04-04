# SheepShaver SCSI Bridge Driver - Product Requirements Document

**Created:** 2026-04-04
**Status:** Draft
**Owner:** Orion Letizi

## Problem Statement

Mac OS 9 applications (MESA II, ReCycle) running on SheepShaver cannot communicate with SCSI samplers because the SCSI Plug's Device Manager trap hooks are invisible to PPC code. SheepShaver's Mixed Mode Manager fails to route PPC `_Control`/`_Read`/`_Write` calls through 68k trap table patches installed by the SCSI Plug extension.

On a real Power Mac, the SCSI Plug patches `_Read`, `_Write`, `_Control`, `_Status` OS traps via `_SetOSTrapAddress`. When PPC apps call these traps, the ROM's Mixed Mode dispatcher calls the Plug's handlers. On SheepShaver, the Mixed Mode dispatch at ROM 0x140EE0 fails for 68k handlers, and the Plug's hooks are never reached.

This blocks the primary goal: capturing MESA II's SCSI traffic to understand why SysEx-over-SCSI writes to the Akai S3000XL don't persist.

## User Stories

- As a user running MESA II on SheepShaver, I want MESA II to discover and communicate with my Akai S3000XL sampler over the network SCSI bridge, so that I can use MESA II to edit samples on the S3000XL
- As a developer, I want to capture MESA II's MIDI-over-SCSI traffic to reverse-engineer the correct SysEx write sequence

## Success Criteria

- [ ] Mac OS 9 System Profiler shows SCSI devices (already works via SCSIAction)
- [ ] MESA II "Find Sampler..." shows the S3000XL in the device list dialog
- [ ] MESA II can read Programs and Samples from the S3000XL
- [ ] All MIDI-over-SCSI CDBs (0x09, 0x0C, 0x0D, 0x0E) appear in the SCSI trace log
- [ ] ReCycle can also discover SCSI samplers

## Scope

### In Scope

- A 68k Mac OS device driver installed in the unit table during `InstallDrivers`
- Driver handles `_Open`, `_Prime` (Read/Write), `_Control`, `_Status` via emulation ops
- Emulation op handlers call `scsi_send_cmd()` to route SCSI commands through the existing network backend
- Driver name matches what the SCSI Plug expects (likely `.EDisk` or a SCSI driver refNum the Plug checks)
- Support for MIDI-over-SCSI protocol (CDBs 0x09, 0x0C, 0x0D, 0x0E)
- Support for standard SCSI commands (INQUIRY, TEST UNIT READY, READ, WRITE)
- Logging of all SCSI commands passing through the driver

### Out of Scope

- Full SCSI Manager emulation (SCSIAction already works)
- SCSI bus emulation at the hardware register level
- Support for SCSI targets other than the S3000XL (initially)
- Writing samples TO the S3000XL (read-first, write later)
- Fixing SheepShaver's Mixed Mode Manager (too complex, driver approach bypasses it)

## Dependencies

- Existing `scsi_send_cmd()` network backend in `scsi_s2p.cpp` (working)
- SheepShaver's emulation op framework (well-established pattern from Sony/Disk/CDROM drivers)
- s2p-midi on Raspberry Pi with SCSI_EXEC support (working)
- Akai S3000XL connected to SCSI bus via PiSCSI board

## Open Questions

- [ ] What driver refNum does the SCSI Plug expect? Need to check what refNum the Plug passes in `_Control` calls (observed: ioRefNum=-50 in traces)
- [ ] Does the Plug check the driver NAME or the driver REFNUM to decide which `_Control` calls to intercept?
- [ ] What `csCode` values does the Plug use for `_Control` to send SCSI commands?
- [ ] Should the driver present itself as `.EDisk` (the real Apple SCSI disk driver name) or use a different name?
- [ ] What is the Plug's expected protocol for `_Control`-based SCSI I/O? (csCode values, parameter layout)

## Appendix

### Evidence from debugging (see DEBUGGING.md)

- 11 theories tested and disproven
- Plug's trap patches confirmed installed correctly in 68k trap table
- PPC _Control dispatch at ROM 0x140EF8/0x140F44/0x140F90 loads trap table but Mixed Mode fails
- Plug's handlers are Universal Procedure Pointers (mixed PPC/68k), not pure 68k
- Two applications (MESA II, ReCycle) on two disk images exhibit identical behavior
- Error -13003 (err_ReplyLength) confirms the Plug IS trying MIDI-over-SCSI communication
- System Profiler correctly enumerates SCSI devices (SCSIAction path works)

### Existing SheepShaver driver pattern

SheepShaver already implements 4 drivers using the emulation op pattern:
- `.Sony` (floppy) — OP_SONY_OPEN/PRIME/CONTROL/STATUS
- `.Disk` (generic disk) — OP_DISK_OPEN/PRIME/CONTROL/STATUS
- `.AppleCD` (CD-ROM) — OP_CDROM_OPEN/PRIME/CONTROL/STATUS
- Audio driver — OP_AUDIO_DISPATCH

The SCSI bridge driver follows the same proven pattern.
