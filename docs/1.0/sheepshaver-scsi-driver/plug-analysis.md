# SCSI Plug Binary Analysis: Post-INQUIRY Decision Logic

## Summary of Root Cause

The SCSI Plug rejects the S3000XL because the device record's **handler descriptor pointer** at offset +0x24 is NULL. For Processor-type SCSI devices (type 0x03), the handler at offset 0x1150 tests this field and, if NULL, silently returns success (0) without registering the device as a sampler. The function at 0x16E0 is called with a NULL first parameter, causing it to return immediately via a NULL check at 0x16F0.

The +0x24 field is only populated by deep initialization paths (at offsets 0x70EA, 0x714E, 0x4A24) that require the device to have been previously recognized and registered by the Plug's internal device database. Since the S3000XL is not in this database (or the database lookup fails in the emulated environment), +0x24 stays NULL, and the device is silently skipped.

**The Plug does NOT match devices by INQUIRY vendor/product strings.** There are no "AKAI", "S3000", "MESA", or "SAMPLER" strings anywhere in the 32KB binary. Device recognition is done through a vtable/descriptor architecture with pre-registered handler function pointers.

## Binary Details

- File: `plug_full.bin` (32768 bytes)
- Base address in memory: 0x1014E9DA
- SCSI Plug code region: approximately 0x0000-0x1800
- Remaining bytes (0x1800+): Mac OS system code (MenuMgrPatches, BlueBox, error strings)

## Architecture Overview

The Plug uses a layered dispatch architecture:

```
INQUIRY result
    |
    v
0x140A: Orchestrator
    |-- 0x0FBE: Unit Table Lookup (UTableBase at LM 0x011C)
    |-- 0x13E4: Classification (bits 9-10 of composite word)
    |-- 0x10FC: INQUIRY Handler (for Class 4, bit 3 clear)
    |   |-- Jump table by SCSI device type
    |   |-- Type 0x03 handler at 0x1150
    |   |   |-- Tests (record+0x24)
    |   |   |-- If NULL: returns 0 (NO REGISTRATION)
    |   |   |-- If non-NULL: vtable dispatch via 0x0DD6
    |-- 0x11D0: Alternate Handler (for bit 3 set)
    |-- 0x1332: Classifier (for Class 2 or default)
```

### Device Record Structure

The device record (parameter a4 in 0x10FC) has the following layout:

| Offset | Size | Description |
|--------|------|-------------|
| +0x04  | word | Flags (bit 3: handler variant, bit 5: capability, bit 7: processed, bit 8: alternate) |
| +0x06  | word | Composite word: bits 0-7 = handler type, bits 8-15 = mode, bits 0-2 also = SCSI device type |
| +0x10  | word | Status/result code |
| +0x18  | word | Unit number / driver refNum (negative value) |
| +0x1A  | word | Scan state (1 = already scanned) |
| +0x1C  | long | Pointer to stripped address |
| +0x20  | long | Vtable pointer (code offset table) |
| +0x24  | long | **Handler descriptor pointer (CRITICAL - must be non-NULL for recognition)** |
| +0x28  | long | Cleared for Processor/Tape devices |
| +0x2C  | word | Sub-type (low 2 bits used for secondary dispatch) |
| +0x2E  | long | Data value (used for offset accumulation) |

## Critical Code Path for S3000XL

### Step 1: Orchestrator (0x140A)

```
0x140A: LINK a6, #-4
0x140E: MOVEM.L d5-d7/a4, -(sp)
0x1412: MOVE.L (a6+12), d5        ; d5 = composite word (SCSI device type + class bits)
0x1416: MOVEA.L (a6+8), a4        ; a4 = device record
0x141A: MOVE.W d5, (a4+6)         ; store composite word in device record
0x141E: PEA (a6-4)                ; push &local_handle
0x1422: MOVE.L a4, -(sp)          ; push device record
0x1424: JSR 0x0FBE                ; Unit Table Lookup
0x1428: MOVE.W d0, d7             ; d7 = result (0 = success)
0x142A: ADDQ.L #8, sp
0x142C: BNE 0x14F2                ; ERROR: lookup failed -> store error and exit
0x1430: MOVE.W #1, (a4+0x10)      ; mark as found
0x1436: MOVE.W #2, (a4+4)         ; set initial flags
```

If 0x0FBE fails (returns non-zero), the entire scan for this device aborts at 0x142C.

### Step 2: Capability Check (0x1440-0x1464)

```
0x143C: MOVEA.L (a6-4), a0        ; a0 = lookup result (context struct)
0x1440: MOVE.W (a0+4), d0         ; d0 = capability flags from context
0x145C: MOVEQ #0x20, d0           ; check bit 5
0x145E: AND.W (a0+4), d0          ; bit 5 of capability flags
0x1462: BNE.S 0x1466              ; if set, continue
0x1464: MOVEQ #-28, d7            ; ERROR -28: bit 5 not set
```

The context structure's flags word at +4 must have bit 5 set. Error -28 means the device lacks a required capability.

### Step 3: Classification (0x13E4)

```
0x13E4: LINK a6, #0
0x13E8: MOVE.W (a6+10), d1        ; d1 = composite word
0x13EC: MOVE.W #0x0200, d0
0x13F0: AND.W d1, d0              ; test bit 9
0x13F2: BEQ.S 0x13F8              ; if clear, check bit 10
0x13F4: MOVEQ #4, d0              ; return CLASS 4 (bit 9 set)
0x13F6: BRA.S 0x1406
0x13F8: MOVE.W #0x0400, d0
0x13FC: AND.W d1, d0              ; test bit 10
0x13FE: BEQ.S 0x1404
0x1400: MOVEQ #2, d0              ; return CLASS 2 (bit 10 set)
0x1402: BRA.S 0x1406
0x1404: MOVEQ #1, d0              ; return CLASS 1 (default)
0x1406: UNLK a6
0x1408: RTS
```

Classification based on bits in the composite word:
- **Bit 9 set -> Class 4**: Routes to 0x10FC (INQUIRY handler) or 0x11D0 (alternate)
- **Bit 10 set -> Class 2**: Routes to 0x1332 (classifier)
- **Neither -> Class 1**: Routes to 0x1332 then 0x0E14 (post-processing)

### Step 4: INQUIRY Handler (0x10FC) - For Class 4

```
0x10FC: LINK a6, #0
0x1100: MOVEM.L d6-d7/a3-a4, -(sp)
0x1104: MOVEA.L (a6+8), a3        ; a3 = context struct (from lookup)
0x1108: MOVEA.L (a6+12), a4       ; a4 = device record

; Extract SCSI device type
0x110C: MOVEQ #7, d0
0x110E: AND.W (a4+6), d0          ; d0 = word_at(a4+6) & 0x07
0x1112: MOVEQ #0, d7
0x1114: MOVE.W d0, d7             ; d7 = SCSI device type (0-7)

; Check bit 5 of context flags (REQUIRED)
0x1116: MOVEQ #0x20, d0
0x1118: AND.W (a3+4), d0          ; bit 5 of context+4
0x111C: MOVEQ #0, d1
0x111E: MOVE.W d0, d1
0x1120: TST.L d1
0x1122: BNE.S 0x1130              ; if bit 5 set, continue
0x1124: MOVE.W #-28, (a4+16)      ; ERROR: store -28 in status
0x112A: MOVEQ #-28, d0            ; return -28
0x112C: BRA 0x11C6                ; exit
```

**Bit 5 check**: If bit 5 of (a3+4) is NOT set, error -28 is returned. This was the check that initially blocked the S3000XL before patching INQUIRY byte 5.

### Step 5: Device Type Dispatch (0x1130-0x114E)

```
0x1130: MOVE.L d7, d0
0x1132: SUBQ.L #2, d0             ; d0 = d7 - 2
0x1134: BCS.S 0x11A0              ; if d7 < 2, generic handler
0x1136: CMPI.L #3, d0             ; compare (d7-2) with 3
0x113C: BHI.S 0x11A0              ; if d7 > 5, generic handler

; d7 in range 2-5: jump table dispatch
0x113E: ADD.L d0, d0              ; d0 *= 2 (word index into table)
0x1140: MOVE.W (PC,d0.W,6), d0   ; read jump table at 0x1148
0x1144: JMP (PC,d0.W)             ; dispatch
```

Jump table at 0x1148 (4 entries for d7=2,3,4,5):

| d7 | SCSI Type | Table Entry | Target |
|----|-----------|-------------|--------|
| 2  | Tape      | +10         | 0x1150 |
| 3  | Processor | +10         | 0x1150 |
| 4  | Write-Once| +84         | 0x119A |
| 5  | CD-ROM    | +88         | 0x119E |

**For d7=3 (Processor): jumps to 0x1150.**

### Step 6: Processor/Tape Handler (0x1150) - THE CRITICAL CHECK

```
0x1150: MOVEQ #0, d0
0x1152: MOVE.L d0, (a4+0x28)      ; clear (a4+0x28) - reset accumulator
0x1156: MOVEQ #10, d7             ; d7 = 0x0A (vtable index for Processor handler)
0x1158: TST.L (a4+0x24)           ; *** TEST HANDLER DESCRIPTOR POINTER ***
0x115C: BNE.S 0x116E              ; if non-NULL, proceed to vtable dispatch

; +0x24 IS NULL: early exit path
0x115E: MOVEQ #0, d0              ; push 0 as first param
0x1160: MOVE.L d0, -(sp)
0x1162: MOVE.L a3, -(sp)          ; push context
0x1164: JSR 0x16E0                ; call cleanup function
0x1168: MOVEQ #0, d0              ; return 0 (silent "success")
0x116A: ADDQ.L #8, sp
0x116C: BRA.S 0x11C6              ; exit
```

**THIS IS THE REJECTION POINT.** When (a4+0x24) is NULL:
1. Calls 0x16E0(0, context) - which checks its first param at 0x16EC (`TST.L (a6+8)`) and returns immediately since it's 0
2. Returns 0 to caller - not an error, just silent non-registration
3. The device is never added to the sampler list

### Step 7: What Would Happen if +0x24 Were Non-NULL (0x116E+)

If +0x24 were non-NULL, execution would continue to the secondary dispatch:

```
0x116E: MOVEQ #3, d0
0x1170: AND.W (a4+0x2C), d0       ; sub-type from (a4+0x2C) & 3
0x1174: ADD.W d0, d0              ; word index
0x1176: MOVE.W (PC,d0.W,6), d0   ; second jump table
0x117A: JMP (PC,d0.W)             ; dispatch by sub-type
```

This leads to the common handler at 0x11A0:

```
0x11A0: MOVEQ #0, d0
0x11A2: MOVE.W d7, d0             ; d0 = 0x0A (vtable index)
0x11A4: MOVE.L d0, -(sp)          ; push vtable index
0x11A6: MOVE.L a4, -(sp)          ; push device record
0x11A8: MOVE.L a3, -(sp)          ; push context
0x11AA: JSR 0x1088                ; call vtable dispatcher wrapper
```

The wrapper at 0x1088 would dereference the context structure and dispatch through the code table at 0x0DD6, which reads a function offset from the vtable at index 0x0A and calls the Processor device handler. This handler would perform the actual device registration.

## Vtable Dispatcher (0x0DD6)

```
0x0DD6: MOVEM.L d1-d7/a0-a6, -(sp)  ; save all registers
0x0DDA: MOVEA.L (sp+0x3C), a2       ; a2 = vtable base pointer
0x0DDE: MOVE.L (sp+0x48), d1        ; d1 = vtable index (0x0A for Processor)
0x0DE2: MOVE.W (a2,d1.W), d1        ; d1 = function offset from vtable
0x0DEE: JSR (a2,d1.W)               ; call vtable[0x0A] handler
0x0DF2: EXT.L d0                     ; sign-extend result
0x0DF4: MOVEM.L (sp)+, all regs
0x0DF8: RTS
```

The vtable is a table of 16-bit offsets relative to the table base. For Processor devices, offset 0x0A in the vtable points to the handler that would recognize and register the S3000XL.

## Where +0x24 Gets Set (For Reference)

Three locations in the binary set +0x24 to non-NULL:

1. **0x70EA**: `MOVE.L a3, (a3+0x24)` - Sets to self-pointer. Context also gets vtable at a5+0x045A stored at +0x20.

2. **0x714E**: `MOVE.L a4, (a4+0x24)` - Sets to self-pointer. Context gets vtable at a5+0x0462 stored at +0x20.

3. **0x4A24**: `MOVE.L #1, (a3+0x24)` - Sets to 1 (simple boolean). Followed by device descriptor initialization.

All three paths are in code blocks that appear to handle known device initialization. The code at 0x70EA/0x714E is in the MenuMgrPatches/BlueBox region, which handles Classic-to-Carbon app switching -- these set +0x24 as part of app workspace initialization, not SCSI device registration. The path at 0x4A24 is in the BlueBox NM switch response handler.

**None of these paths are reachable through the normal SCSI INQUIRY scan flow for an unknown device.**

## Unit Table Lookup (0x0FBE)

```
0x0FBE: LINK a6, #0
0x0FC6: MOVEA.L (a6+12), a2       ; a2 = output pointer
0x0FCA: MOVEA.L (a6+8), a0        ; a0 = device record
0x0FCE: MOVE.W (a0+0x18), d7      ; d7 = unit number (negative refNum)
0x0FD2: NOT.W d7                   ; complement to get table index
0x0FD4: CMP.W (0x01D2).W, d7      ; compare with UnitNtryCnt
0x0FD8: BCS.S 0x0FDE              ; if within range, continue
0x0FDA: MOVEQ #-21, d0            ; error: invalid unit number
0x0FDE: MOVEQ #0, d0
0x0FE0: MOVE.W d7, d0             ; d0 = table index
0x0FE2: MOVEA.L (0x011C).W, a0    ; a0 = UTableBase
0x0FE6: MOVEA.L (a0,d0), a3       ; a3 = DCE handle from unit table
```

Key Mac OS low memory globals used:
- **0x011C** = UTableBase (pointer to unit table array)
- **0x01D2** = UnitNtryCnt (number of entries in unit table)

The lookup converts a driver refNum to a unit table index via NOT (complement), then indexes into the unit table to find the Device Control Entry (DCE).

## Trap Patching (0x0D18)

The Plug patches four Device Manager traps:

| Trap | Name | Patch Address |
|------|------|---------------|
| A002 | _Read | 0x0D60 |
| A003 | _Write | 0x0D94 |
| A004 | _Control | 0x0E20 |
| A005 | _Status | 0x0DCA |

Each patch checks the ioRefNum field of the parameter block. If the refNum matches a registered SCSI sampler device, the patch intercepts the call. Otherwise, it falls through to the original trap handler via saved vectors (at low memory 0x07A8, 0x07AC, etc.).

A global flag byte at low memory 0x0DD5 acts as a master enable: if 0, all patches pass through without checking.

## Likely Root Causes (Ordered by Probability)

### 1. The +0x24 Field is an Internal Plug State, Not Set by INQUIRY

The +0x24 field appears to be set during the Plug's INIT phase, not during INQUIRY processing. The Plug's initialization code must pre-register known device handlers before the scan loop runs. If the INIT phase doesn't complete properly (e.g., missing resources, failed driver installation), no devices will have +0x24 set, and the scan will find "no samplers" for every target.

**Evidence**: The three locations that set +0x24 are all in initialization/setup code, not in INQUIRY response processing. The scan at 0x10FC only *reads* +0x24, never writes it.

### 2. The Plug's INIT Depends on .EDisk Driver Resource

The INIT code at 0x06BE checks for a DRVR resource named ".EDisk" via GetNamedResource. If this resource is not found, the Plug may abort initialization, leaving all device records uninitialized. In SheepShaver, if the MESA II extension's resource fork is not properly loaded, this check would fail.

### 3. SheepShaver SCSI Emulation Lacks Device Manager Integration

Mac OS normally creates Device Manager entries (DCEs in the unit table) for SCSI devices during boot. SheepShaver's SCSI emulation via the network bridge to scsi2pi may not create these entries, causing the 0x0FBE lookup to fail with error -22 (no entry found). This would abort the entire scan path at 0x142C.

### 4. Classification Routing Issue

The composite word's bits 9-10 determine the processing class. If these bits are not set correctly for the S3000XL, the device may be routed to Class 1 (default) instead of Class 4 (full INQUIRY handler), bypassing the vtable dispatch entirely.

## Recommended Next Steps

1. **Instrument 0x1158**: Add a breakpoint or log at the TST.L (a4+0x24) instruction to confirm +0x24 is NULL when the S3000XL is processed.

2. **Instrument 0x0FBE**: Check whether the unit table lookup succeeds or fails. If it fails, the entire scan aborts before reaching the +0x24 check.

3. **Instrument 0x142C**: Check if the orchestrator exits early due to a lookup error.

4. **Dump the device record**: At 0x1150 (entry to Processor handler), dump the entire a4 struct (at least 0x30 bytes) to see which fields are populated and which are zero.

5. **Check .EDisk resource**: Verify that the MESA II extension's DRVR ".EDisk" resource is accessible in SheepShaver's resource chain.

6. **Check INIT completion**: Verify that the trap patching code at 0x0D18 actually runs (check if the patched trap vectors are installed).
