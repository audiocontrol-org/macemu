/*
 *  emul_op.cpp - 68k opcodes for ROM patches
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
#include "main.h"
#include "version.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "video.h"
#include "audio.h"
#include "ether.h"
#include "serial.h"
#include "clip.h"
#include "extfs.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "rsrc_patches.h"
#include "name_registry.h"
#include "user_strings.h"
#include "emul_op.h"
#include "thunks.h"

#define DEBUG 0
#include "debug.h"

extern bool tick_inhibit;

void PlayStartupSound();

// TVector of MakeExecutable
static uint32 MakeExecutableTvec;


/*
 *  Handle SCSIAction parameter block — shared between 68k and PPC callers.
 */
int32 HandleSCSIAction(uint32 pb)
{
	static FILE *scsi_log = nullptr;
	if (!scsi_log) {
		const char *extfs = PrefsFindString("extfs");
		if (extfs) {
			char logpath[512];
			snprintf(logpath, sizeof(logpath), "%s/scsi_trace.log", extfs);
			scsi_log = fopen(logpath, "w");
			if (scsi_log) fprintf(scsi_log, "=== SCSI Trace Log ===\n");
		}
	}

	int32 result = 0;
	uint8 functionCode = ReadMacInt8(pb + 8);
	uint8 busNum = ReadMacInt8(pb + 13);
	uint8 targetID = ReadMacInt8(pb + 14);
	uint8 lun = ReadMacInt8(pb + 15);
	uint16 pbLength = ReadMacInt16(pb + 6);

	if (scsi_log) {
		fprintf(scsi_log, "\n--- CALL func=0x%02x(%d) bus=%d target=%d lun=%d pbLen=%d pb=0x%08x ---\n",
			functionCode, functionCode, busNum, targetID, lun, pbLength, pb);
		int dumpSize = pbLength > 0 ? (pbLength < 256 ? pbLength : 256) : 176;
		fprintf(scsi_log, "PB_BEFORE[%d]:", dumpSize);
		for (int i = 0; i < dumpSize; i++) {
			if (i % 32 == 0) fprintf(scsi_log, "\n  %3d:", i);
			fprintf(scsi_log, " %02x", ReadMacInt8(pb + i));
		}
		fprintf(scsi_log, "\n");
		fflush(scsi_log);
	}

	switch (functionCode) {
	case 1: { // SCSIExecIO
		uint32 flags = ReadMacInt32(pb + 20);
		uint8 cdbLength = ReadMacInt8(pb + 53);
		uint32 dataPtr = ReadMacInt32(pb + 40);
		uint32 dataLength = ReadMacInt32(pb + 44);
		bool cdbIsPointer = (flags & 0x01000000) != 0;
		uint32 cdbAddr = cdbIsPointer ? ReadMacInt32(pb + 68) : (pb + 68);
		bool reading = (flags & 0x40000000) != 0;
		bool writing = (flags & 0x80000000) != 0;

		uint8 cdb[16] = {};
		for (int i = 0; i < cdbLength && i < 16; i++)
			cdb[i] = ReadMacInt8(cdbAddr + i);

		if (scsi_log) {
			fprintf(scsi_log, "  ExecIO: flags=0x%08x cdbLen=%d dir=%s dataPtr=0x%08x dataLen=%u\n",
				flags, cdbLength, reading ? "IN" : writing ? "OUT" : "NONE", dataPtr, dataLength);
			fprintf(scsi_log, "  CDB:");
			for (int i = 0; i < cdbLength; i++) fprintf(scsi_log, " %02x", cdb[i]);
			fprintf(scsi_log, "\n");
		}

		scsi_set_cmd(cdbLength, cdb);
		if (!scsi_set_target(targetID, lun)) {
			WriteMacInt16(pb + 10, (uint16)(int16)-7932);
			result = -7932;
			if (scsi_log) { fprintf(scsi_log, "  RESULT: scsiNoTarget\n"); fflush(scsi_log); }
			break;
		}

		uint16 stat = 0;
		size_t actual = 0;
		if (dataLength > 0 && dataPtr) {
			uint8 *host_data_ptr = Mac2HostAddr(dataPtr);
			uint8 *sg_ptr[1] = { host_data_ptr };
			uint32 sg_len[1] = { dataLength };
			bool ok = scsi_send_cmd(dataLength, reading, 1, sg_ptr, sg_len, &stat, 600, &actual);
			if (!ok) {
				WriteMacInt16(pb + 10, (uint16)(int16)-7936);
				result = -7936;
				if (scsi_log) { fprintf(scsi_log, "  RESULT: FAILED\n"); fflush(scsi_log); }
				break;
			}
		} else {
			uint8 *sg_ptr[1] = { nullptr };
			uint32 sg_len[1] = { 0 };
			scsi_send_cmd(0, false, 0, sg_ptr, sg_len, &stat, 600);
		}

		WriteMacInt8(pb + 60, stat);
		WriteMacInt32(pb + 64, dataLength - actual); // scsiDataResidual
		WriteMacInt16(pb + 36, 0);
		WriteMacInt32(pb + 16, 0);
		WriteMacInt16(pb + 10, (stat == 0) ? 0 : -7934);
		result = (int16)ReadMacInt16(pb + 10);

		if (scsi_log) {
			fprintf(scsi_log, "  RESULT: scsi_status=%d result=%d residual=%u (actual=%zu/%u)\n",
				stat, result, (unsigned)(dataLength - actual), actual, dataLength);
			// For INQUIRY (CDB 0x12) to target 6, dump the full PB and data buffer
			// so we can verify what the Plug's code reads
			if (reading && stat == 0 && cdb[0] == 0x12 && targetID == 6) {
				fprintf(scsi_log, "  TARGET6_INQUIRY_PB[%d] at 0x%08x:\n", pbLength, pb);
				int dumpPB = pbLength > 0 ? (pbLength < 256 ? pbLength : 256) : 176;
				for (int ii = 0; ii < dumpPB; ii++) {
					if (ii % 16 == 0) fprintf(scsi_log, "    pb+%02x:", ii);
					fprintf(scsi_log, " %02x", ReadMacInt8(pb + ii));
					if (ii % 16 == 15) fprintf(scsi_log, "\n");
				}
				fprintf(scsi_log, "\n");
				// Also dump memory at the INQUIRY data pointer and surrounding area
				fprintf(scsi_log, "  INQUIRY_BUF at 0x%08x (word at +4 = 0x%04x, word at +6 = 0x%04x):\n",
					dataPtr, ReadMacInt16(dataPtr + 4), ReadMacInt16(dataPtr + 6));
			}
			if (reading && dataLength > 0 && dataPtr && stat == 0) {
				uint32 dumpLen = dataLength < 256 ? dataLength : 256;
				fprintf(scsi_log, "  DATA_IN[%u]:", dataLength);
				for (uint32 i = 0; i < dumpLen; i++) {
					if (i % 32 == 0) fprintf(scsi_log, "\n   ");
					fprintf(scsi_log, " %02x", ReadMacInt8(dataPtr + i));
				}
				fprintf(scsi_log, "\n  ASCII: ");
				for (uint32 i = 0; i < dumpLen; i++) {
					uint8 c = ReadMacInt8(dataPtr + i);
					fprintf(scsi_log, "%c", (c >= 32 && c < 127) ? c : '.');
				}
				fprintf(scsi_log, "\n");
			}
			fflush(scsi_log);
		}
		break;
	}
	case 3: { // SCSIBusInquiry
		if (scsi_log) fprintf(scsi_log, "  BusInquiry: bus=%d\n", busNum);
		if (busNum != 0 && busNum != 0xFF) {
			WriteMacInt16(pb + 10, (uint16)(int16)-7869);
			result = -7869;
			if (scsi_log) { fprintf(scsi_log, "  RESULT: no such bus (0xE143)\n"); fflush(scsi_log); }
			break;
		}
		if (busNum == 0xFF) WriteMacInt8(pb + 13, 0);
		WriteMacInt16(pb + 36, 1);
		WriteMacInt16(pb + 38, 0);
		WriteMacInt32(pb + 40, 1);
		WriteMacInt16(pb + 44, 164);
		WriteMacInt16(pb + 46, 164);
		WriteMacInt32(pb + 48, 0);
		WriteMacInt8(pb + 52, 0x43);
		WriteMacInt8(pb + 53, 0);
		WriteMacInt8(pb + 68, 7);      // scsiInitiatorID — host adapter is SCSI ID 7
		WriteMacInt8(pb + 69, 7);      // scsiMaxTarget
		WriteMacInt16(pb + 84, 7);
		WriteMacInt16(pb + 86, 0);
		WriteMacInt16(pb + 10, 0);
		result = 0;
		if (scsi_log) { fprintf(scsi_log, "  RESULT: noErr\n"); fflush(scsi_log); }
		break;
	}
	case 0x80: { // SCSIGetVirtualIDInfo
		bool exists = scsi_is_target_present(targetID);
		WriteMacInt16(pb + 36, targetID);
		WriteMacInt8(pb + 38, exists ? 1 : 0);
		WriteMacInt16(pb + 10, 0);
		result = 0;
		if (scsi_log) { fprintf(scsi_log, "  VirtualIDInfo: target=%d exists=%d\n", targetID, exists); fflush(scsi_log); }
		break;
	}
	case 0x84: case 0x85: case 0x86: { // scsiOldCall variants
		// On a real Mac, OldCall translates old-style SCSI Manager calls
		// into SCSI Manager 4.3 ExecIO calls and executes them.
		// We execute a TEST UNIT READY to verify the target is responsive
		// and populate the PB with realistic results.
		bool exists = scsi_is_target_present(targetID);
		if (exists) {
			uint8 tur_cdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // TEST UNIT READY
			scsi_set_cmd(6, tur_cdb);
			if (scsi_set_target(targetID, lun)) {
				uint16 tur_stat = 0;
				uint8 *sg_ptr[1] = { nullptr };
				uint32 sg_len[1] = { 0 };
				scsi_send_cmd(0, false, 0, sg_ptr, sg_len, &tur_stat, 30);
				// Set SCSI status in the OldCall PB
				WriteMacInt8(pb + 60, tur_stat);  // scsiSCSIstatus
				result = (tur_stat == 0) ? 0 : -7934; // noErr or scsiDataRunError
				if (scsi_log) {
					fprintf(scsi_log, "  OldCall(0x%02x): target=%d TUR status=%d result=%d\n",
						functionCode, targetID, tur_stat, result);
				}
			} else {
				result = -7932; // scsiNoTarget
				if (scsi_log) fprintf(scsi_log, "  OldCall(0x%02x): target=%d set_target failed\n", functionCode, targetID);
			}
		} else {
			result = -7932;
			if (scsi_log) fprintf(scsi_log, "  OldCall(0x%02x): target=%d not present\n", functionCode, targetID);
		}
		WriteMacInt16(pb + 10, (uint16)(int16)result);
		if (scsi_log) fflush(scsi_log);
		break;
	}
	case 0: case 4: case 5: case 6: case 7: case 8:
	case 16: case 17: case 18:
		WriteMacInt16(pb + 10, 0);
		result = 0;
		break;
	default:
		WriteMacInt16(pb + 10, 0);
		result = 0;
		if (scsi_log) { fprintf(scsi_log, "  UNHANDLED func=0x%02x\n", functionCode); fflush(scsi_log); }
		break;
	}

	if (scsi_log) {
		int dumpSize = pbLength > 0 ? (pbLength < 256 ? pbLength : 256) : 176;
		fprintf(scsi_log, "PB_AFTER[%d]:", dumpSize);
		for (int i = 0; i < dumpSize; i++) {
			if (i % 32 == 0) fprintf(scsi_log, "\n  %3d:", i);
			fprintf(scsi_log, " %02x", ReadMacInt8(pb + i));
		}
		fprintf(scsi_log, "\n  result=%d\n", result);
		fflush(scsi_log);
	}

	return result;
}


/*
 *  Execute EMUL_OP opcode (called by 68k emulator)
 */

void EmulOp(M68kRegisters *r, uint32 pc, int selector)
{
	D(bug("EmulOp %04x at %08x\n", selector, pc));
	switch (selector) {
		case OP_BREAK:				// Breakpoint
			printf("*** Breakpoint\n");
			Dump68kRegs(r);
			break;

		case OP_XPRAM1: {			// Read/write from/to XPRam
			uint32 len = r->d[3];
			uint8 *adr = Mac2HostAddr(r->a[3]);
			D(bug("XPRAMReadWrite d3: %08lx, a3: %p\n", len, adr));
			int ofs = len & 0xffff;
			len >>= 16;
			if (len & 0x8000) {
				len &= 0x7fff;
				for (uint32 i=0; i<len; i++)
					XPRAM[((ofs + i) & 0xff) + 0x1300] = *adr++;
			} else {
				for (uint32 i=0; i<len; i++)
					*adr++ = XPRAM[((ofs + i) & 0xff) + 0x1300];
			}
			break;
		}

		case OP_XPRAM2:				// Read from XPRam
			r->d[1] = XPRAM[(r->d[1] & 0xff) + 0x1300];
			break;

		case OP_XPRAM3:				// Write to XPRam
			XPRAM[(r->d[1] & 0xff) + 0x1300] = r->d[2];
			break;

		case OP_NVRAM1: {			// Read from NVRAM
			int ofs = r->d[0];
			r->d[0] = XPRAM[ofs & 0x1fff];
			bool localtalk = !(XPRAM[0x13e0] || XPRAM[0x13e1]);	// LocalTalk enabled?
			switch (ofs) {
				case 0x13e0:			// Disable LocalTalk (use EtherTalk instead)
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e1:
					if (localtalk)
						r->d[0] = 0x01;
					break;
				case 0x13e2:
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e3:
					if (localtalk)
						r->d[0] = 0x0a;
					break;
			}
			break;
		}

		case OP_NVRAM2:				// Write to NVRAM
			XPRAM[r->d[0] & 0x1fff] = r->d[1];
			break;

		case OP_NVRAM3:				// Read/write from/to NVRAM
			if (r->d[3]) {
				r->d[0] = XPRAM[(r->d[4] + 0x1300) & 0x1fff];
			} else {
				XPRAM[(r->d[4] + 0x1300) & 0x1fff] = r->d[5];
				r->d[0] = 0;
			}
			break;

		case OP_FIX_MEMTOP:			// Fixes MemTop in BootGlobs during startup
			D(bug("Fix MemTop\n"));
			WriteMacInt32(BootGlobsAddr - 20, RAMBase + RAMSize);	// MemTop
			r->a[6] = RAMBase + RAMSize;
			break;

		case OP_FIX_MEMSIZE: {		// Fixes physical/logical RAM size during startup
			D(bug("Fix MemSize\n"));
			uint32 diff = ReadMacInt32(0x1ef8) - ReadMacInt32(0x1ef4);
			WriteMacInt32(0x1ef8, RAMSize);			// Physical RAM size
			WriteMacInt32(0x1ef4, RAMSize - diff);	// Logical RAM size
			break;
		}

		case OP_FIX_BOOTSTACK:		// Fixes boot stack pointer in boot 3 resource
			D(bug("Fix BootStack\n"));
			r->a[1] = r->a[7] = RAMBase + RAMSize * 3 / 4;
			break;

		case OP_SONY_OPEN:			// Floppy driver functions
			r->d[0] = SonyOpen(r->a[0], r->a[1]);
			break;
		case OP_SONY_PRIME:
			r->d[0] = SonyPrime(r->a[0], r->a[1]);
			break;
		case OP_SONY_CONTROL:
			r->d[0] = SonyControl(r->a[0], r->a[1]);
			break;
		case OP_SONY_STATUS:
			r->d[0] = SonyStatus(r->a[0], r->a[1]);
			break;

		case OP_DISK_OPEN:			// Disk driver functions
			r->d[0] = DiskOpen(r->a[0], r->a[1]);
			break;
		case OP_DISK_PRIME:
			r->d[0] = DiskPrime(r->a[0], r->a[1]);
			break;
		case OP_DISK_CONTROL:
			r->d[0] = DiskControl(r->a[0], r->a[1]);
			break;
		case OP_DISK_STATUS:
			r->d[0] = DiskStatus(r->a[0], r->a[1]);
			break;

		case OP_CDROM_OPEN:			// CD-ROM driver functions
			r->d[0] = CDROMOpen(r->a[0], r->a[1]);
			break;
		case OP_CDROM_PRIME:
			r->d[0] = CDROMPrime(r->a[0], r->a[1]);
			break;
		case OP_CDROM_CONTROL:
			r->d[0] = CDROMControl(r->a[0], r->a[1]);
			break;
		case OP_CDROM_STATUS:
			r->d[0] = CDROMStatus(r->a[0], r->a[1]);
			break;

		case OP_AUDIO_DISPATCH:		// Audio component functions
			r->d[0] = AudioDispatch(r->a[3], r->a[4]);
			break;

		case OP_SOUNDIN_OPEN:		// Sound input driver functions
			r->d[0] = SoundInOpen(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_PRIME:
			r->d[0] = SoundInPrime(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CONTROL:
			r->d[0] = SoundInControl(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_STATUS:
			r->d[0] = SoundInStatus(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CLOSE:
			r->d[0] = SoundInClose(r->a[0], r->a[1]);
			break;

		case OP_ADBOP:				// ADBOp() replacement
			ADBOp(r->d[0], Mac2HostAddr(ReadMacInt32(r->a[0])));
			break;

		case OP_INSTIME:			// InsTime() replacement
			r->d[0] = InsTime(r->a[0], r->d[1]);
			break;
		case OP_RMVTIME:			// RmvTime() replacement
			r->d[0] = RmvTime(r->a[0]);
			break;
		case OP_PRIMETIME:			// PrimeTime() replacement
			r->d[0] = PrimeTime(r->a[0], r->d[0]);
			break;

		case OP_MICROSECONDS:		// Microseconds() replacement
			Microseconds(r->a[0], r->d[0]);
			break;

		case OP_ZERO_SCRAP:			// ZeroScrap() patch
			ZeroScrap();
			break;

		case OP_PUT_SCRAP:			// PutScrap() patch
			PutScrap(ReadMacInt32(r->a[7] + 8), Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_GET_SCRAP:			// GetScrap() patch
			GetScrap((void **)Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_DEBUG_STR:			// DebugStr() shows warning message
			if (PrefsFindBool("nogui")) {
				uint8 *pstr = Mac2HostAddr(ReadMacInt32(r->a[7] + 4));
				char str[256];
				int i;
				for (i=0; i<pstr[0]; i++)
					str[i] = pstr[i+1];
				str[i] = 0;
				WarningAlert(str);
			}
			break;

		case OP_INSTALL_DRIVERS: {	// Patch to install our own drivers during startup
			// Install drivers
			InstallDrivers();

			// Patch MakeExecutable()
			MakeExecutableTvec = FindLibSymbol("\023PrivateInterfaceLib", "\016MakeExecutable");
			D(bug("MakeExecutable TVECT at %08x\n", MakeExecutableTvec));
			WriteMacInt32(MakeExecutableTvec, NativeFunction(NATIVE_MAKE_EXECUTABLE));
#if !EMULATED_PPC
			WriteMacInt32(MakeExecutableTvec + 4, (uint32)TOC);
#endif

			// Patch DebugStr()
			static const uint8 proc_template[] = {
				M68K_EMUL_OP_DEBUG_STR >> 8, M68K_EMUL_OP_DEBUG_STR & 0xFF,
				0x4e, 0x74,			// rtd	#4
				0x00, 0x04
			};
			BUILD_SHEEPSHAVER_PROCEDURE(proc);
			WriteMacInt32(0x1dfc, proc);
			break;
		}

		case OP_NAME_REGISTRY:		// Patch Name Registry and initialize CallUniversalProc
			r->d[0] = (uint32)-1;
			PatchNameRegistry();
			InitCallUniversalProc();
			break;

		case OP_RESET:				// Early in MacOS reset
			D(bug("*** RESET ***\n"));
			tick_inhibit = true;
			CDROMRemount(); // for System 7.x
			TimerReset();
			MacOSUtilReset();
			EtherResetCachedAllocation();
			ether_reset();
			AudioReset();
#ifdef USE_SDL_AUDIO
			PlayStartupSound();
#endif
			// Enable DR emulator (disabled for now)
			if (PrefsFindBool("jit68k") && 0) {
				D(bug("DR activated\n"));
				WriteMacInt32(KernelDataAddr + 0x17a0, 3);		// Prepare for DR emulator activation
				WriteMacInt32(KernelDataAddr + 0x17c0, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x17c4, DR_CACHE_SIZE);
				WriteMacInt32(KernelDataAddr + 0x1b04, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x1b00, DR_EMULATOR_BASE);
				memcpy((void *)DR_EMULATOR_BASE, (void *)(ROMBase + 0x370000), DR_EMULATOR_SIZE);
				MakeExecutable(0, DR_EMULATOR_BASE, DR_EMULATOR_SIZE);
			}
			tick_inhibit = false;
			break;

		case OP_IRQ:			// Level 1 interrupt
			WriteMacInt16(ReadMacInt32(KernelDataAddr + 0x67c), 0);	// Clear interrupt
			r->d[0] = 0;
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
#if !PRECISE_TIMING
					TimerInterrupt();
#endif
					ExecuteNative(NATIVE_VIDEO_VBL);

					static int tick_counter = 0;
					if (++tick_counter >= 60) {
						tick_counter = 0;
						SonyInterrupt();
						DiskInterrupt();
						CDROMInterrupt();
					}

					r->d[0] = 1;		// Flag: 68k interrupt routine executes VBLTasks etc.
				}
				if (InterruptFlags & INTFLAG_SERIAL) {
					ClearInterruptFlag(INTFLAG_SERIAL);
					SerialInterrupt();
				}
				if (InterruptFlags & INTFLAG_ETHER) {
					ClearInterruptFlag(INTFLAG_ETHER);
					ExecuteNative(NATIVE_ETHER_IRQ);
				}
				if (InterruptFlags & INTFLAG_TIMER) {
					ClearInterruptFlag(INTFLAG_TIMER);
					TimerInterrupt();
				}
				if (InterruptFlags & INTFLAG_AUDIO) {
					ClearInterruptFlag(INTFLAG_AUDIO);
					AudioInterrupt();
				}
				if (InterruptFlags & INTFLAG_ADB) {
					ClearInterruptFlag(INTFLAG_ADB);
					ADBInterrupt();
				}
			} else
				r->d[0] = 1;
			break;

		case OP_SCSI_DISPATCH: {	// SCSIDispatch() replacement
			static int scsi_disp_seq = 0;
			scsi_disp_seq++;
			uint32 ret = ReadMacInt32(r->a[7]);
			uint16 sel = ReadMacInt16(r->a[7] + 4);
			r->a[7] += 6;
			int stack;
			switch (sel) {
				case 0:		// SCSIReset
					WriteMacInt16(r->a[7], SCSIReset());
					fprintf(stderr, "SCSIDispatch[%d] Reset -> %d\n", scsi_disp_seq, ReadMacInt16(r->a[7]));
					stack = 0;
					break;
				case 1: {	// SCSIGet
					int16 res = SCSIGet();
					WriteMacInt16(r->a[7], res);
					fprintf(stderr, "SCSIDispatch[%d] Get -> %d\n", scsi_disp_seq, res);
					stack = 0;
					break;
				}
				case 2:		// SCSISelect
				case 11: {	// SCSISelAtn
					uint8 tgt = ReadMacInt8(r->a[7] + 1);
					int16 res = SCSISelect(tgt);
					WriteMacInt16(r->a[7] + 2, res);
					fprintf(stderr, "SCSIDispatch[%d] %s target=%d -> %d\n", scsi_disp_seq,
						sel == 2 ? "Select" : "SelAtn", tgt, res);
					stack = 2;
					break;
				}
				case 3: {	// SCSICmd
					uint16 cmdLen = ReadMacInt16(r->a[7]);
					uint8 *cmdPtr = Mac2HostAddr(ReadMacInt32(r->a[7] + 2));
					fprintf(stderr, "SCSIDispatch[%d] Cmd len=%d CDB:", scsi_disp_seq, cmdLen);
					for (int i = 0; i < cmdLen && i < 16; i++)
						fprintf(stderr, " %02x", cmdPtr[i]);
					int16 res = SCSICmd(cmdLen, cmdPtr);
					WriteMacInt16(r->a[7] + 6, res);
					fprintf(stderr, " -> %d\n", res);
					stack = 6;
					break;
				}
				case 4: {	// SCSIComplete
					uint32 timeout = ReadMacInt32(r->a[7]);
					uint32 msgAddr = ReadMacInt32(r->a[7] + 4);
					uint32 statAddr = ReadMacInt32(r->a[7] + 8);
					int16 res = SCSIComplete(timeout, msgAddr, statAddr);
					WriteMacInt16(r->a[7] + 12, res);
					uint16 stat = ReadMacInt16(statAddr);
					uint16 msg = ReadMacInt16(msgAddr);
					fprintf(stderr, "SCSIDispatch[%d] Complete timeout=%d -> %d stat=%d msg=%d\n",
						scsi_disp_seq, timeout, res, stat, msg);
					stack = 12;
					break;
				}
				case 5:		// SCSIRead
				case 8: {	// SCSIRBlind
					uint32 tibAddr = ReadMacInt32(r->a[7]);
					int16 res = SCSIRead(tibAddr);
					WriteMacInt16(r->a[7] + 4, res);
					fprintf(stderr, "SCSIDispatch[%d] %s tib=0x%08x -> %d\n", scsi_disp_seq,
						sel == 5 ? "Read" : "RBlind", tibAddr, res);
					stack = 4;
					break;
				}
				case 6:		// SCSIWrite
				case 9: {	// SCSIWBlind
					uint32 tibAddr = ReadMacInt32(r->a[7]);
					int16 res = SCSIWrite(tibAddr);
					WriteMacInt16(r->a[7] + 4, res);
					fprintf(stderr, "SCSIDispatch[%d] %s tib=0x%08x -> %d\n", scsi_disp_seq,
						sel == 6 ? "Write" : "WBlind", tibAddr, res);
					stack = 4;
					break;
				}
				case 10:	// SCSIStat
					WriteMacInt16(r->a[7], SCSIStat());
					fprintf(stderr, "SCSIDispatch[%d] Stat -> 0x%04x\n", scsi_disp_seq, SCSIStat());
					stack = 0;
					break;
				case 12:	// SCSIMsgIn
					WriteMacInt16(r->a[7] + 4, 0);
					fprintf(stderr, "SCSIDispatch[%d] MsgIn\n", scsi_disp_seq);
					stack = 4;
					break;
				case 13:	// SCSIMsgOut
					WriteMacInt16(r->a[7] + 2, 0);
					fprintf(stderr, "SCSIDispatch[%d] MsgOut\n", scsi_disp_seq);
					stack = 2;
					break;
				case 14:	// SCSIMgrBusy
					WriteMacInt16(r->a[7], SCSIMgrBusy());
					fprintf(stderr, "SCSIDispatch[%d] MgrBusy -> %d\n", scsi_disp_seq, ReadMacInt16(r->a[7]));
					stack = 0;
					break;
				default:
					fprintf(stderr, "SCSIDispatch[%d] UNKNOWN sel=%d\n", scsi_disp_seq, sel);
					stack = 0;
			}
			fflush(stderr);
			r->a[0] = ret;
			r->a[7] += stack;
			break;
		}

		case OP_SCSI_ATOMIC: {		// SCSIAction/SCSIAtomic replacement (68k callers)
			uint32 pb = r->a[0];
			uint32 caller = ReadMacInt32(r->a[7]);  // return address on stack
			fprintf(stderr, "SCSIAtomic: pb=0x%08x func=%d target=%d caller=0x%08x a4=0x%08x\n",
				pb, ReadMacInt8(pb + 8), ReadMacInt8(pb + 14), caller, r->a[4]);
			fflush(stderr);
			r->d[0] = (uint32)HandleSCSIAction(pb);
			break;
		}

		case OP_CHECK_SYSV: {		// Check we are not using MacOS < 8.1 with a NewWorld ROM
			r->a[1] = r->d[1];
			r->a[0] = ReadMacInt32(r->d[1]);
			uint32 sysv = ReadMacInt16(r->a[0]);
			D(bug("Detected MacOS version %d.%d.%d\n", (sysv >> 8) & 0xf, (sysv >> 4) & 0xf, sysv & 0xf));
			if (ROMType == ROMTYPE_NEWWORLD && sysv < 0x0801)
				r->d[1] = 0;
			break;
		}

		case OP_NTRB_17_PATCH:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			if (ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH2:
			r->a[7] += 8;
			PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH3:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH4:
			r->d[0] = ReadMacInt16(r->a[7]);
			r->a[7] += 2;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_CHECKLOAD: {		// vCheckLoad() patch
			uint32 type = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			int16 id = ReadMacInt16(r->a[2]);
			if (r->a[0] == 0)
				break;
			uint32 adr = ReadMacInt32(r->a[0]);
			if (adr == 0)
				break;
			uint16 *p = (uint16 *)Mac2HostAddr(adr);
			uint32 size = ReadMacInt32(adr - 8) & 0xffffff;
			CheckLoad(type, id, p, size);
			break;
		}

		case OP_EXTFS_COMM:			// External file system routines
			WriteMacInt16(r->a[7] + 14, ExtFSComm(ReadMacInt16(r->a[7] + 12), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 4)));
			break;

		case OP_EXTFS_HFS:
			WriteMacInt16(r->a[7] + 20, ExtFSHFS(ReadMacInt32(r->a[7] + 16), ReadMacInt16(r->a[7] + 14), ReadMacInt32(r->a[7] + 10), ReadMacInt32(r->a[7] + 6), ReadMacInt16(r->a[7] + 4)));
			break;

		case OP_IDLE_TIME: {
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);

			// Guard: protect the SCSI Plug's _Control trap patch.
			// Another extension overwrites _Control after the Plug installs it.
			// We detect the Plug's handler by checking if _Read points to Plug code
			// (0x1014xxxx range), then ensure _Control points to Plug+0x0E20.
			{
				static uint32 plug_control_addr = 0;
				static int guard_log_count = 0;
				uint32 read_handler = ReadMacInt32(0x0400 + 0x02 * 4);
				// Detect Plug by checking _Read handler is in extension memory
				if (read_handler > 0x10000000 && read_handler < 0x11000000 && !plug_control_addr) {
					// First detection: compute expected _Control from _Read
					// _Read is at Plug+0x0D60, _Control is at Plug+0x0E20
					// Plug base = _Read - 0x0D60
					uint32 plug_base = read_handler - 0x0D60;
					plug_control_addr = plug_base + 0x0E20;
				}
				if (plug_control_addr) {
					uint32 current_control = ReadMacInt32(0x0400 + 0x04 * 4);
					if (current_control != plug_control_addr) {
						WriteMacInt32(0x0400 + 0x04 * 4, plug_control_addr);
						if (guard_log_count < 5) {
							fprintf(stderr, "GUARD: _Control was 0x%08x, restored to Plug 0x%08x\n",
								current_control, plug_control_addr);
							fflush(stderr);
							guard_log_count++;
						}
					}
				}
			}

			// Check for automation commands from host
			extern void ScriptHookIdle();
			ScriptHookIdle();
			break;
		}

		case OP_IDLE_TIME_2:
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->d[0] = (uint32)-2;
			break;

		case OP_PLUG_TRACE:
			fprintf(stderr, "PLUG_TRACE pc=0x%08x d0=0x%08x d7=0x%08x a0=0x%08x a4=0x%08x\n",
				pc, r->d[0], r->d[7], r->a[0], r->a[4]);
			fflush(stderr);
			break;

		default:
			printf("FATAL: EMUL_OP called with bogus selector %08x\n", selector);
			QuitEmulator();
			break;
	}
}
