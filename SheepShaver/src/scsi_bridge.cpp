/*
 *  scsi_bridge.cpp - SCSI bridge driver for protocol discovery
 *
 *  Logging-mode driver: every handler logs all PB fields to stderr
 *  and returns noErr. This is Phase 2 — protocol discovery only.
 *  Phase 4 will add real SCSI I/O dispatch.
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

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "scsi_bridge.h"


/*
 *  Driver Open() routine
 */

int16 SCSIBridgeOpen(uint32 pb, uint32 dce)
{
	int16 refNum = ReadMacInt16(pb + ioRefNum);
	fprintf(stderr, "SCSIBridgeOpen pb=0x%08x dce=0x%08x ioRefNum=%d\n", pb, dce, refNum);

	// Dump the first 20 entries of the unit table to find other SCSI drivers
	uint32 unit_table = ReadMacInt32(0x11C);
	fprintf(stderr, "  Unit table at 0x%08x:\n", unit_table);
	for (int i = 0; i < 65; i++) {
		uint32 entry = ReadMacInt32(unit_table + i * 4);
		if (entry) {
			uint32 dce_ptr = ReadMacInt32(entry);
			if (dce_ptr) {
				uint32 drv_ptr = ReadMacInt32(dce_ptr);
				if (drv_ptr > 0x1000) {
					// Read driver name (Pascal string at drv_ptr + 18)
					uint8 name_len = ReadMacInt8(drv_ptr + 18);
					if (name_len > 0 && name_len < 32) {
						char name[33] = {};
						for (int j = 0; j < name_len; j++)
							name[j] = ReadMacInt8(drv_ptr + 19 + j);
						fprintf(stderr, "    refNum=%d: \"%s\" drv=0x%08x\n", -(i+1), name, drv_ptr);
					}
				}
			}
		}
	}
	fflush(stderr);
	return noErr;
}


/*
 *  Driver Prime() routine (Read/Write)
 */

int16 SCSIBridgePrime(uint32 pb, uint32 dce)
{
	uint16 trap = ReadMacInt16(pb + ioTrap);
	uint32 buffer = ReadMacInt32(pb + ioBuffer);
	uint32 req_count = ReadMacInt32(pb + ioReqCount);
	uint16 pos_mode = ReadMacInt16(pb + ioPosMode);
	uint32 pos_offset = ReadMacInt32(pb + ioPosOffset);

	fprintf(stderr, "SCSIBridgePrime pb=0x%08x ioTrap=0x%04x ioBuffer=0x%08x ioReqCount=%u ioPosMode=0x%04x ioPosOffset=0x%08x\n",
		pb, trap, buffer, req_count, pos_mode, pos_offset);
	fflush(stderr);

	// Set ioActCount to 0 (no data transferred in logging mode)
	WriteMacInt32(pb + ioActCount, 0);
	return noErr;
}


/*
 *  Driver Control() routine
 */

int16 SCSIBridgeControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);

	switch (code) {
	case 1: // KillIO
		return noErr;
	case 65: // accRun — periodic, don't log
		return noErr;
	default:
		// Log and return noErr for unknown codes (discovery mode)
		fprintf(stderr, "SCSIBridgeControl pb=0x%08x csCode=%d (0x%04x) csParam:", pb, code, code);
		for (int i = 0; i < 22; i++) {
			fprintf(stderr, " %02x", ReadMacInt8(pb + csParam + i));
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		return noErr;
	}
}


/*
 *  Driver Status() routine
 */

int16 SCSIBridgeStatus(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);

	// Return statusErr for most codes — returning noErr without data
	// causes Mac OS to loop infinitely on csCode 13/17.
	// Log non-polling codes for discovery.
	static int status_log_count = 0;
	if (code != 10 && status_log_count < 50) {
		fprintf(stderr, "SCSIBridgeStatus pb=0x%08x csCode=%d (0x%04x)\n", pb, code, code);
		fflush(stderr);
		status_log_count++;
	}
	return statusErr;
}
