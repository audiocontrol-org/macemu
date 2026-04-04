# SheepShaver SCSI Bridge Driver - Workplan

**GitHub Milestone:** TBD
**GitHub Issues:** TBD

## Technical Approach

Install a 68k Mac OS device driver (`.SCSI`) in the unit table using SheepShaver's proven emulation op pattern (same as Sony/Disk/CDROM drivers). The driver handles `_Control`/`_Read`/`_Write` via emulation ops that call the existing `scsi_send_cmd()` C++ backend.

### Why This Works

The PPC Device Manager dispatch has TWO code paths:
1. **Trap table path** (ROM 0x140EF8): reads 68k handler from trap table, calls via Mixed Mode — **BROKEN on SheepShaver**
2. **Unit table path**: reads driver DCE from unit table, calls driver entry point — **WORKS** (how Sony/Disk/CDROM already function from PPC code)

By installing our driver in the unit table, PPC _Control calls with our ioRefNum go through path #2 (working). The Plug's 68k INIT code also calls _Control through the 68k trap table, which also reaches our driver through the unit table.

## Implementation Phases

### Phase 1: Driver Stub + Installation

**Files:**
- `SheepShaver/src/include/emul_op.h` — add M68K_EMUL_OP constants
- `SheepShaver/src/rom_patches.cpp` — define `scsi_bridge_driver[]` stub, install in `InstallDrivers()`

**Tasks:**
1. Add `M68K_EMUL_OP_SCSI_BRIDGE_*` constants to emul_op.h
2. Define 68k driver stub (`.SCSI`, refNum -50, emulation ops at each entry point)
3. Copy stub to ROM at `sony_offset + 0x700` (free 256-byte slot)
4. Install via `DrvrInstallRsrvMem()` + unit table setup + `_Open` in `InstallDrivers()`

### Phase 2: Emulation Op Dispatch + Logging Implementation

**Files:**
- `SheepShaver/src/emul_op.cpp` — dispatch cases
- `SheepShaver/src/include/scsi_bridge.h` — header (NEW)
- `SheepShaver/src/scsi_bridge.cpp` — implementation (NEW)
- `SheepShaver/src/Unix/Makefile` — add scsi_bridge.o

**Tasks:**
1. Create `scsi_bridge.h` with function declarations
2. Create `scsi_bridge.cpp` with logging-mode handlers:
   - `SCSIBridgeOpen` — set up DCE, return noErr
   - `SCSIBridgePrime` — log ioBuffer/ioReqCount/direction, return noErr
   - `SCSIBridgeControl` — log csCode + hex dump csParam, return noErr
   - `SCSIBridgeStatus` — log csCode, return noErr
3. Add dispatch cases in `emul_op.cpp`
4. Update Makefile
5. Build + boot + test

### Phase 3: Protocol Discovery

**Tasks:**
1. Boot Mac OS 9 with MESA II and the new driver
2. Trigger MESA II scan (Find Sampler or SCSI icon click)
3. Analyze `scsi_bridge.log` for:
   - What csCodes the Plug sends
   - csParam layout for each csCode
   - Data sent via _Write, expected via _Read
4. If no traffic on refNum -50, try renaming driver to `.EDisk`
5. Document the discovered protocol

### Phase 4: SCSI Command Dispatch

**Tasks:**
1. Implement csCode handlers in `SCSIBridgeControl`:
   - Parse SCSI target, CDB, data from csParam
   - Call `scsi_set_cmd()` / `scsi_set_target()` / `scsi_send_cmd()`
   - Return results in PB
2. Implement `SCSIBridgePrime` for data transfers
3. Test with MESA II — verify "Find Sampler" shows S3000XL
4. Capture MIDI-over-SCSI traffic

## Success Criteria

- [ ] Driver installs without errors during boot
- [ ] `scsi_bridge.log` shows _Control calls from the Plug
- [ ] MESA II "Find Sampler" shows S3000XL device
- [ ] MIDI CDBs (0x09, 0x0C, 0x0D, 0x0E) appear in SCSI trace
- [ ] MESA II can read Programs/Samples from S3000XL
