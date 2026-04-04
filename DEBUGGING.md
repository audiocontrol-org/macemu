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
  - Added: HandleSCSIAction, SCSIDispatch tracing, Gestalt patches, SHUTDOWN command
  - Boots both OS 7 and OS 9

## Docker Containers

| Container | Port | Disk | Source | Purpose |
|-----------|------|------|--------|---------|
| `sheepshaver-dev` | 16080 | OS 9 (sheepshaver-data/disk.hfv) | macemu (full fork) | Original OS 9 + MESA II |
| `sheepshaver-os9-minimal` | 16082 | OS 9 (sheepshaver-data/disk.hfv) | macemu-os9-minimal | Minimal patches for MESA II debugging |

All containers need `--privileged` for `vm.mmap_min_addr=0`.

## What Works

1. **SCSI network backend** — SheepShaver forwards SCSI commands to s2p over TCP
2. **Old SCSI Manager (SCSIDispatch)** — Fully instrumented, all calls logged with sequence numbers
3. **SCSI Manager 4.3 (SCSIAtomic/SCSIAction)** — HandleSCSIAction processes ExecIO, BusInquiry, etc.
4. **INQUIRY** — S3000XL found at target 6, returns `AKAI EMIS3000XL SAMPLER 2.00`
5. **scsi_s2p fixes** — Removed incorrect `<< 1` status shift; CHECK CONDITION returns transport success
6. **SHUTDOWN command** — Write `SHUTDOWN` to `{extfs}/command.txt` for clean shutdown
7. **MESA I on OS 7** — Connects (TUR + INQUIRY succeed), but doesn't load data (OS version mismatch — MESA I needs S3000XL OS 1.x, device has 2.00)

## What Doesn't Work

### MESA II "Find Sampler" generates zero SCSI calls

MESA II (v1.2, OS 9) finds the S3000XL during boot-time SCSI scan (INQUIRY succeeds with 36-byte response showing correct Akai vendor/product). But clicking Sampler > Find Sampler generates **zero** new SCSI calls on any API path.

**Root cause:** MESA II's SCSI Plug has prerequisite checks that fail:

1. **Gestalt('scsi')** — Must return gestaltAsyncSCSI flags. **FIXED** — registered in InstallDrivers.
2. **ROM+0x12** — Must equal 0x2AF2. **FIXED** — patched in patch_68k.
3. **Gestalt('mach')** — Must be <= 0x7E (identifies Macs with built-in SCSI). **PARTIALLY FIXED** — replaced post-boot in ScriptHookIdle. Problem: MESA starts via Startup Items during boot, before the idle hook runs.

### Gestalt('mach') timing problem

Mac OS 9 checks `Gestalt('mach')` during boot to verify the system folder matches the hardware. If we replace it too early (in InstallDrivers), Mac OS 9 shows "This startup disk will not work on this Macintosh model." If we replace it post-boot (ScriptHookIdle), MESA's SCSI Plug has already cached its "no SCSI" decision.

**The original fork solved this** with PPC SCSIAction thunk patching (rewriting branch targets in ROM at 0x150000-0x170000). But those patches break Mac OS 7 boot — the pattern scan hits wrong code in the OS 7 boot path.

### Possible solutions

1. **Make thunk patches conditional** — detect OS version and skip thunk patching for OS 7
2. **Remove MESA from Startup Items** — boot without MESA, let Gestalt('mach') replace, then launch MESA manually. The question is whether MESA re-checks Gestalt on Find Sampler or caches it from startup.
3. **Patch MESA's SCSI Plug directly** — find the Gestalt('mach') check in the Plug code and NOP it
4. **Find a different timing hook** — replace Gestalt('mach') after Mac OS boot but before Startup Items run

## S3000XL MIDI-via-SCSI Protocol (Known)

Documented in `audiocontrol-scsi-midi-bridge/docs/1.0/scsi-midi-bridge/capture-notes.md`:

| Step | CDB | Direction | Purpose |
|------|-----|-----------|---------|
| Init | `09:00:01:01:00:00` | No data | Activate MIDI-via-SCSI session |
| Send | `0C:00:00:00:LL:00` | DATA OUT | Send SysEx to S3000XL |
| Poll | `0D:00:00:00:00:00` | DATA IN (3 bytes) | Read pending byte count |
| Read | `0E:00:00:00:LL:00` | DATA IN | Read SysEx response |

The existing bridge implementation (`audiocontrol-scsi-midi-bridge`) uses these successfully for reads. Writes via `0C` succeed (no SCSI error) but data doesn't persist in the sampler.

## Key Files

### macemu-os9-minimal
- `SheepShaver/src/emul_op.cpp` — HandleSCSIAction, SCSIDispatch tracing, SCSIAtomic handler
- `SheepShaver/src/Unix/scsi_s2p.cpp` — Network SCSI backend with fixes
- `SheepShaver/src/rom_patches.cpp` — ROM+0x12 patch, Gestalt('scsi') registration
- `SheepShaver/src/script_hook.cpp` — SHUTDOWN command, Gestalt('mach') post-boot replacement

### Pi (s3k.local)
- `/tmp/s2p-midi` — Custom s2p build with SCSI_EXEC support
- Start: `sudo /tmp/s2p-midi --port 6868`
- s2pexec: `/opt/scsi2pi/bin/s2pexec -i 6 -c CDB`

### Logging
- SheepShaver stderr → `/tmp/sheepshaver.log` (in container)
- SCSI trace → `{extfs}/scsi_trace.log` (shared folder, created by HandleSCSIAction)
- Script hook → `{extfs}/hook.log`

## Bisect Results

The commit that breaks OS 7: `25848e89` (SCSIAction handler with ROM patches).
The last good commit for OS 7: `8f67ef4e` (automation hooks, no ROM patches).

The specific changes that cause the hang:
- ROM+0x12 header word modification
- Trap 0xA089 redirection
- PPC SCSIAction thunk rewriting (pattern scan at ROM 0x150000-0x170000)
- Gestalt registration via Execute68kTrap during InstallDrivers

## Next Steps

1. Try removing MESA from Startup Items, boot OS 9, let mach Gestalt replace, then launch MESA manually via Find Sampler
2. If that works, we have our capture environment
3. If not, investigate whether MESA's SCSI Plug caches its decision or re-checks on Find Sampler
4. Alternative: add the PPC thunk patches but make them conditional on a pref flag (`scsi_thunks true/false`)
