# SheepShaver SCSI Bridge Driver - Workplan

**GitHub Milestone:** TBD
**GitHub Issues:** TBD

## Technical Approach

Install a 68k Mac OS device driver in the unit table using SheepShaver's proven emulation op pattern (same as Sony/Disk/CDROM drivers). The driver handles `_Control`/`_Read`/`_Write` via emulation ops that call the existing `scsi_send_cmd()` C++ backend.

### Why This Works

The PPC Device Manager dispatch has TWO code paths:
1. **Trap table path** (ROM 0x140EF8): reads 68k handler from trap table, calls via Mixed Mode — **BROKEN on SheepShaver**
2. **Unit table path**: reads driver DCE from unit table, calls driver entry point — **WORKS** (how Sony/Disk/CDROM already function from PPC code)

By installing our driver in the unit table, PPC calls with our ioRefNum go through path #2 (working).

## Implementation Phases

### Phase 1: Driver Stub + Installation (COMPLETE)

Installed driver `.SCSI` at refNum -49 (`.EDisk` slot in unit table) with emulation ops for Open/Prime/Control/Status. Driver receives traffic from Mac OS (Status polling, Control accRun).

**Files modified:**
- `SheepShaver/src/include/emul_op.h` — OP_SCSI_BRIDGE_* opcodes + constants
- `SheepShaver/src/rom_patches.cpp` — driver stub + installation in InstallDrivers()
- `SheepShaver/src/emul_op.cpp` — dispatch cases

**Files created:**
- `SheepShaver/src/include/scsi_bridge.h`
- `SheepShaver/src/scsi_bridge.cpp` (logging-mode handlers)

### Phase 2: HFS Disk Mount (NEXT)

Validate the driver can do real SCSI I/O by mounting an HFS disk image served by s2p.

**Setup:**
- Create 10MB HFS disk image on Pi: `hfs_test.hds` (DONE)
- Mount in s2p at SCSI ID 0: `s2pctl -c attach -i 0 -f hfs_test.hds` (DONE)

**Implementation:**
1. Implement `SCSIBridgePrime` to translate Mac OS READ/WRITE into SCSI READ(10)/WRITE(10) CDBs via `scsi_send_cmd()`
2. Implement disk-related Status csCodes (8=DriveStatus, 6=FormatList) 
3. Implement disk-related Control csCodes (1=KillIO, 5=Verify)
4. Register a drive in the Mac OS drive queue (via `AddDrive`) so Finder sees it
5. Handle partition mapping if needed (Apple Partition Map or raw volume)

**Success criteria:**
- [ ] Mac OS 9 mounts the HFS volume on the desktop
- [ ] Files can be read from the mounted volume
- [ ] SCSI READ commands appear in `scsi_send_cmd` trace

**Key technical details:**
- Mac OS sends READ with ioBuffer (data destination), ioReqCount (byte count), ioPosOffset (disk position)
- Driver builds SCSI READ(10) CDB: `28 00 [LBA 4 bytes] 00 [count 2 bytes] 00`
- LBA = ioPosOffset / 512, count = ioReqCount / 512
- Block size is 512 bytes (standard for SCHD)

### Phase 3: MESA II Protocol Discovery

Once disk I/O works, investigate what the SCSI Plug needs:
1. Determine what ioRefNum the Plug actually uses for _Control calls
2. Trace the Plug's _Control handler code to find its refNum check
3. Install our driver at the correct refNum
4. Log the Plug's csCode/csParam protocol
5. Document findings

### Phase 4: MIDI-over-SCSI Implementation

Implement the Plug's SCSI protocol based on Phase 3 findings:
1. Handle Plug-specific csCodes in SCSIBridgeControl
2. Translate to MIDI-over-SCSI CDBs (0x09, 0x0C, 0x0D, 0x0E)
3. Test with MESA II — verify "Find Sampler" shows S3000XL
4. Capture MIDI-over-SCSI traffic for the write persistence investigation

## Success Criteria

- [ ] Phase 1: Driver installs and receives Device Manager traffic (DONE)
- [ ] Phase 2: HFS disk image mounts in Mac OS 9
- [ ] Phase 3: Plug's _Control protocol discovered and documented
- [ ] Phase 4: MESA II finds and communicates with S3000XL
