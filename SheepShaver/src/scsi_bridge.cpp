/*
 *  scsi_bridge.cpp - SCSI bridge driver for HFS disk I/O
 *
 *  Phase 2: Real SCSI disk I/O. Translates Mac OS Device Manager
 *  Read/Write/Control/Status calls into SCSI READ(10)/WRITE(10)
 *  commands via scsi_send_cmd(), allowing Mac OS 9 to mount an
 *  HFS disk image served by s2p.
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "scsi.h"
#include "scsi_bridge.h"


// SCSI target for the HFS disk (hardcoded for now)
static const int kSCSITargetID = 0;
static const int kSCSITargetLUN = 0;
static const int kBlockSize = 512;

// Drive state
static uint32 drive_status = 0;     // Mac address of DrvSts record
static int drive_num = 0;           // Drive number in drive queue
static uint32 total_blocks = 0;     // Total block count from READ CAPACITY
static bool drive_mounted = false;  // Flag: drive needs mounting via accRun


/*
 *  Send a SCSI READ CAPACITY(10) to determine disk size.
 *  Returns total number of 512-byte blocks, or 0 on failure.
 */

static uint32 scsi_read_capacity(void)
{
	uint8 cdb[10] = { 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8 response[8];
	uint8 *sg_ptr[1] = { response };
	uint32 sg_len[1] = { 8 };
	uint16 stat = 0;

	scsi_set_cmd(10, cdb);
	if (!scsi_set_target(kSCSITargetID, kSCSITargetLUN)) {
		fprintf(stderr, "SCSIBridge: READ CAPACITY - target %d not present\n", kSCSITargetID);
		return 0;
	}

	if (!scsi_send_cmd(8, true, 1, sg_ptr, sg_len, &stat, 600)) {
		fprintf(stderr, "SCSIBridge: READ CAPACITY failed (transport error)\n");
		return 0;
	}

	if (stat != 0) {
		fprintf(stderr, "SCSIBridge: READ CAPACITY failed (SCSI status %d)\n", stat);
		return 0;
	}

	// READ CAPACITY returns last LBA (big-endian 4 bytes) + block size (big-endian 4 bytes)
	uint32 last_lba = ((uint32)response[0] << 24) | ((uint32)response[1] << 16) |
	                  ((uint32)response[2] << 8) | (uint32)response[3];
	uint32 block_size = ((uint32)response[4] << 24) | ((uint32)response[5] << 16) |
	                    ((uint32)response[6] << 8) | (uint32)response[7];

	fprintf(stderr, "SCSIBridge: READ CAPACITY: last_lba=%u block_size=%u\n", last_lba, block_size);

	// Total blocks = last_lba + 1 (READ CAPACITY returns the last addressable block)
	uint32 blocks = last_lba + 1;

	// If block size differs from 512, adjust
	if (block_size != 512 && block_size > 0) {
		blocks = (uint32)(((uint64)blocks * block_size) / 512);
		fprintf(stderr, "SCSIBridge: adjusted to %u 512-byte blocks\n", blocks);
	}

	return blocks;
}


/*
 *  Driver Open() routine
 *
 *  Register a drive in the Mac OS drive queue for SCSI target 0.
 */

int16 SCSIBridgeOpen(uint32 pb, uint32 dce)
{
	int16 refNum = ReadMacInt16(pb + ioRefNum);
	fprintf(stderr, "SCSIBridgeOpen pb=0x%08x dce=0x%08x ioRefNum=%d\n", pb, dce, refNum);

	// Initialize DCE position
	WriteMacInt32(dce + dCtlPosition, 0);

	// Get disk capacity via SCSI READ CAPACITY
	total_blocks = scsi_read_capacity();
	if (total_blocks == 0) {
		fprintf(stderr, "SCSIBridge: WARNING - READ CAPACITY returned 0 blocks, drive may not mount\n");
		// Continue anyway — the disk might not be attached yet
		total_blocks = 20480; // fallback: 10MB
	}

	fprintf(stderr, "SCSIBridge: disk has %u blocks (%u MB)\n", total_blocks, total_blocks / 2048);

	// Allocate DrvSts record in Mac system heap
	M68kRegisters r;
	r.d[0] = SIZEOF_DrvSts;
	Execute68kTrap(0xa71e, &r);  // NewPtrSysClear()
	if (r.a[0] == 0) {
		fprintf(stderr, "SCSIBridge: ERROR - cannot allocate DrvSts\n");
		return openErr;
	}
	drive_status = r.a[0];
	fprintf(stderr, "SCSIBridge: DrvSts allocated at 0x%08x\n", drive_status);

	// Set up drive status fields
	WriteMacInt8(drive_status + dsInstalled, 1);       // Driver is installed
	WriteMacInt8(drive_status + dsDiskInPlace, 8);     // Fixed disk (non-removable)
	WriteMacInt16(drive_status + dsQType, hard20);     // Hard disk type
	WriteMacInt8(drive_status + dsWriteProt, 0);       // Not write-protected

	// Set drive size (low 16 bits in dsDriveSize, high 16 bits in dsDriveS1)
	WriteMacInt16(drive_status + dsDriveSize, total_blocks & 0xffff);
	WriteMacInt16(drive_status + dsDriveS1, total_blocks >> 16);

	// Find a free drive number
	drive_num = FindFreeDriveNumber(5);
	fprintf(stderr, "SCSIBridge: using drive number %d\n", drive_num);

	// Add drive to the Mac OS drive queue via AddDrive trap
	// AddDrive expects: D0.hi = drive number, D0.lo = refNum; A0 = pointer to dsQLink in DrvSts
	r.d[0] = ((uint32)drive_num << 16) | ((uint32)SCSIBridgeRefNum & 0xffff);
	r.a[0] = drive_status + dsQLink;
	Execute68kTrap(0xa04e, &r);  // AddDrive()

	fprintf(stderr, "SCSIBridge: drive %d added to drive queue (refNum=%d, %u blocks)\n",
	        drive_num, SCSIBridgeRefNum, total_blocks);

	// Flag for mounting on first accRun
	drive_mounted = true;

	fflush(stderr);
	return noErr;
}


/*
 *  Driver Prime() routine (Read/Write)
 *
 *  Translates Mac OS read/write into SCSI READ(10)/WRITE(10).
 */

int16 SCSIBridgePrime(uint32 pb, uint32 dce)
{
	WriteMacInt32(pb + ioActCount, 0);

	// Determine read vs write from ioTrap
	uint16 trap = ReadMacInt16(pb + ioTrap);
	bool is_read = ((trap & 0xff) == aRdCmd);

	// Extract parameters
	uint32 mac_buffer = ReadMacInt32(pb + ioBuffer);
	uint32 req_count = ReadMacInt32(pb + ioReqCount);
	uint32 position = ReadMacInt32(dce + dCtlPosition);

	// Support 64-bit positioning if enabled
	if (ReadMacInt16(pb + ioPosMode) & 0x100) {
		position = ReadMacInt32(pb + ioPosOffset + 4); // low 32 bits of wide offset
	}

	// Validate alignment
	if ((req_count & 0x1ff) || (position & 0x1ff)) {
		fprintf(stderr, "SCSIBridge: Prime alignment error: req_count=%u position=%u\n", req_count, position);
		return paramErr;
	}

	uint32 lba = position / kBlockSize;
	uint32 block_count = req_count / kBlockSize;

	fprintf(stderr, "SCSIBridge: %s LBA=%u blocks=%u (%u bytes) buf=0x%08x\n",
	        is_read ? "READ" : "WRITE", lba, block_count, req_count, mac_buffer);

	// Build SCSI CDB
	uint8 cdb[10];
	if (is_read) {
		cdb[0] = 0x28;  // READ(10)
	} else {
		cdb[0] = 0x2A;  // WRITE(10)
	}
	cdb[1] = 0x00;
	cdb[2] = (lba >> 24) & 0xff;
	cdb[3] = (lba >> 16) & 0xff;
	cdb[4] = (lba >> 8) & 0xff;
	cdb[5] = lba & 0xff;
	cdb[6] = 0x00;
	cdb[7] = (block_count >> 8) & 0xff;
	cdb[8] = block_count & 0xff;
	cdb[9] = 0x00;

	// Get host pointer for the Mac buffer
	uint8 *host_buffer = Mac2HostAddr(mac_buffer);

	// Set up scatter-gather (single contiguous buffer)
	uint8 *sg_ptr[1] = { host_buffer };
	uint32 sg_len[1] = { req_count };
	uint16 stat = 0;

	// Send the SCSI command
	scsi_set_cmd(10, cdb);
	if (!scsi_set_target(kSCSITargetID, kSCSITargetLUN)) {
		fprintf(stderr, "SCSIBridge: target %d not available\n", kSCSITargetID);
		return is_read ? readErr : writErr;
	}

	size_t actual_transferred = 0;
	bool ok = scsi_send_cmd(req_count, is_read, 1, sg_ptr, sg_len, &stat, 1800, &actual_transferred);

	if (!ok || stat != 0) {
		fprintf(stderr, "SCSIBridge: SCSI %s failed: ok=%d stat=%d\n",
		        is_read ? "READ" : "WRITE", ok, stat);
		return is_read ? readErr : writErr;
	}

	// Update ParamBlock and DCE
	uint32 actual = (actual_transferred > 0) ? (uint32)actual_transferred : req_count;
	WriteMacInt32(pb + ioActCount, actual);
	WriteMacInt32(dce + dCtlPosition, position + actual);

	return noErr;
}


/*
 *  Driver Control() routine
 */

int16 SCSIBridgeControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);

	switch (code) {
	case 1:   // KillIO
		return noErr;

	case 5:   // Verify — disk is always ready
		return noErr;

	case 6:   // Format — no-op for SCSI disk
		return noErr;

	case 7: { // Eject — fixed disk, re-post diskEvent
		M68kRegisters r;
		r.d[0] = drive_num;
		r.a[0] = 7;  // diskEvent
		Execute68kTrap(0xa02f, &r);  // PostEvent()
		return noErr;
	}

	case 21:  // Get drive icon
	case 22:  // Get disk icon
		// We don't have an icon — return controlErr so the system uses default
		return controlErr;

	case 23:  // Get drive info
		WriteMacInt32(pb + csParam, 0x0601);  // Unspecified fixed SCSI disk
		return noErr;

	case 24:  // Get partition size
		WriteMacInt32(pb + csParam, total_blocks);
		return noErr;

	case 65:  // accRun — periodic action
		if (drive_mounted) {
			// Post a diskEvent to trigger mounting
			fprintf(stderr, "SCSIBridge: accRun — posting diskEvent for drive %d\n", drive_num);
			M68kRegisters r;
			r.d[0] = drive_num;
			r.a[0] = 7;  // diskEvent
			Execute68kTrap(0xa02f, &r);  // PostEvent()
			drive_mounted = false;

			// Disable periodic action now that we've posted the event
			WriteMacInt16(dce + dCtlFlags, ReadMacInt16(dce + dCtlFlags) & ~0x2000);
		}
		return noErr;

	default:
		fprintf(stderr, "SCSIBridgeControl pb=0x%08x csCode=%d (0x%04x) csParam:", pb, code, code);
		for (int i = 0; i < 22; i++) {
			fprintf(stderr, " %02x", ReadMacInt8(pb + csParam + i));
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		return controlErr;
	}
}


/*
 *  Driver Status() routine
 */

int16 SCSIBridgeStatus(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);

	switch (code) {
	case 6: {
		// FormatList — return one format entry: total blocks, current format
		// Format list entry: 4 bytes capacity + 4 bytes (flags | block size)
		WriteMacInt32(pb + csParam, total_blocks);
		WriteMacInt32(pb + csParam + 4, 0x00000200);  // flags=0, block size=512
		// csParam+8 is the entry count (if the caller expects it)
		return noErr;
	}

	case 8:
		// DriveStatus — copy DrvSts record to csParam
		if (drive_status) {
			Mac2Mac_memcpy(pb + csParam, drive_status, 22);
			return noErr;
		}
		return statusErr;

	case 10:
		// DriverGestalt — not supported (fires constantly, don't log)
		return statusErr;

	case 43: {
		// Driver Gestalt — handle common selectors
		uint32 sel = ReadMacInt32(pb + csParam);
		switch (sel) {
		case FOURCC('v','e','r','s'):  // Version
			WriteMacInt32(pb + csParam + 4, 0x01008000);
			return noErr;
		case FOURCC('d','e','v','t'):  // Device type
			WriteMacInt32(pb + csParam + 4, FOURCC('d','i','s','k'));
			return noErr;
		case FOURCC('i','n','t','f'):  // Interface type
			WriteMacInt32(pb + csParam + 4, FOURCC('s','c','s','i'));
			return noErr;
		case FOURCC('s','y','n','c'):  // Synchronous only?
			WriteMacInt32(pb + csParam + 4, 0x01000000);
			return noErr;
		case FOURCC('b','o','o','t'):  // Boot ID
			WriteMacInt16(pb + csParam + 4, drive_num);
			WriteMacInt16(pb + csParam + 6, (uint16)SCSIBridgeRefNum);
			return noErr;
		case FOURCC('w','i','d','e'):  // 64-bit access
			WriteMacInt16(pb + csParam + 4, 0x0100);
			return noErr;
		case FOURCC('p','u','r','g'):  // Purge flags
			WriteMacInt32(pb + csParam + 4, 0);
			return noErr;
		case FOURCC('e','j','e','c'):  // Eject flags
			WriteMacInt32(pb + csParam + 4, 0x00030003);  // Don't eject on shutdown/restart
			return noErr;
		case FOURCC('f','l','u','s'):  // Flush flags
			WriteMacInt16(pb + csParam + 4, 0);
			return noErr;
		case FOURCC('v','m','o','p'):  // VM attributes
			WriteMacInt32(pb + csParam + 4, 0);
			return noErr;
		default:
			return statusErr;
		}
	}

	default:
		// Log unknown status codes (throttled)
		{
			static int status_log_count = 0;
			if (status_log_count < 50) {
				fprintf(stderr, "SCSIBridgeStatus pb=0x%08x csCode=%d (0x%04x)\n", pb, code, code);
				fflush(stderr);
				status_log_count++;
			}
		}
		return statusErr;
	}
}
