/*
 *  script_hook.cpp - Host-side automation hook for SheepShaver
 *
 *  Watches the shared folder for script files and triggers execution
 *  inside the Mac OS 9 environment. Called from the OP_IDLE_TIME handler.
 *
 *  Approach: Write the script text to a file on the ExtFS "Unix" volume,
 *  then use the Finder's 'odoc' Apple Event to open it with Script Editor.
 *  Script Editor will compile and run the script when it opens.
 *
 *  For stay-open applets (compiled scripts), the Finder opening them
 *  causes them to launch. For text scripts, Script Editor opens them
 *  for editing. To auto-run a text script, we write a wrapper that
 *  Script Editor can execute.
 *
 *  Actually, the simplest approach: we don't need AppleScript at all.
 *  We just need to:
 *  1. Open MESA II (using Finder 'oapp' event)
 *  2. Send MESA II Apple Events directly (using its 'Sampler Suite')
 *
 *  This file implements the command dispatcher that reads commands from
 *  the host filesystem and translates them into Apple Events.
 */

#include "sysdeps.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu_emulation.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "thunks.h"

// Graceful shutdown: launch the "Shutdown" AppleScript applet from the
// ExtFS shared volume. The applet does `tell application "Finder" to shut down`.
//
// STATUS: NOT YET WORKING. Inline 68k AEM calls ($A816), CallMacOS via CFM
// TVectors, and FSMakeFSSpec+LaunchApplication all crash from OP_IDLE_TIME
// context. The Shutdown applet itself works when double-clicked manually.
// See DEBUGGING.md for details.
//
// TODO: Find a way to launch the applet programmatically, or find the
// correct AEM trap/calling convention that works from EMUL_OP context.
//
// For now, we fall back to _ShutDown trap which exits immediately.
void send_shutdown_event()
{
	fprintf(stderr, "send_shutdown_event: using _ShutDown trap (graceful AE launch not yet working)\n");
	fflush(stderr);
	M68kRegisters r;
	r.d[0] = 1; // ShutDwnPower
	Execute68kTrap(0xa895, &r); // _ShutDown
}

// Phase 2 per-target PB addresses, set during boot scan in emul_op.cpp
uint32 g_phase2_pb[8] = {};

// MESA SCSI Plug code base address, set by FIND_PLUG command
uint32 g_mesa_plug_code_base = 0;

// Check interval: only check every N idle cycles to avoid overhead
static int idle_counter = 0;
static const int CHECK_INTERVAL = 120; // ~2 seconds at 60Hz idle rate
static bool hook_initialized = false;
static const char *shared_path = nullptr;

// Log to host filesystem
static void hook_log(const char *fmt, ...)
{
	if (!shared_path) return;
	char logpath[512];
	snprintf(logpath, sizeof(logpath), "%s/hook.log", shared_path);
	FILE *f = fopen(logpath, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// Check if a file exists on the host filesystem
static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

// Read a file from the host filesystem
static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return nullptr;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (char *)malloc(len + 1);
	if (buf) {
		if (fread(buf, 1, len, f) != (size_t)len) { free(buf); fclose(f); return nullptr; }
		buf[len] = 0;
	}
	fclose(f);
	return buf;
}

// Write a file to the host filesystem
static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

/*
 *  Process a command file from the shared folder.
 *
 *  The command file format is simple line-based:
 *    LAUNCH <app-name>    — launch an application
 *    AESEND <target> <event-class> <event-id> [params...]  — send Apple Event
 *    WAIT <seconds>       — pause
 *    LOG <message>        — write to log
 *
 *  For now, we implement the simplest useful thing: detect that the
 *  command file exists and log it. Actually executing Mac Toolbox calls
 *  requires careful construction using Execute68kTrap.
 */

static void process_command_file()
{
	char cmdpath[512], resultpath[512];
	snprintf(cmdpath, sizeof(cmdpath), "%s/command.txt", shared_path);
	snprintf(resultpath, sizeof(resultpath), "%s/result.txt", shared_path);

	if (!file_exists(cmdpath)) return;

	hook_log("Found command.txt, processing...");

	char *content = read_file(cmdpath);
	if (!content) {
		hook_log("Failed to read command.txt");
		return;
	}

	// Delete the command file immediately to prevent re-execution
	unlink(cmdpath);

	// Parse and execute commands
	char *line = strtok(content, "\n");
	while (line) {
		// Skip empty lines and comments
		if (line[0] == '\0' || line[0] == '#') {
			line = strtok(nullptr, "\n");
			continue;
		}

		hook_log("CMD: %s", line);

		if (strncmp(line, "LOG ", 4) == 0) {
			hook_log("LOG: %s", line + 4);
		}
		else if (strncmp(line, "PING", 4) == 0) {
			// Simple test: write a response to prove the hook works
			write_file(resultpath, "PONG - SheepShaver automation hook is active\n");
			hook_log("PONG sent");
		}
		else if (strncmp(line, "KEY ", 4) == 0) {
			// Send a keypress via xdotool (safe — doesn't touch Mac internals)
			const char *key = line + 4;
			char cmd[512];
			snprintf(cmd, sizeof(cmd), "DISPLAY=:0 xdotool key %s", key);
			int ret = system(cmd);
			hook_log("KEY %s → %d", key, ret);
		}
		else if (strncmp(line, "CLICK ", 6) == 0) {
			// Send a mouse click via xdotool
			int x = 0, y = 0;
			sscanf(line + 6, "%d %d", &x, &y);
			char cmd[512];
			snprintf(cmd, sizeof(cmd), "DISPLAY=:0 xdotool mousemove %d %d click 1", x, y);
			int ret = system(cmd);
			hook_log("CLICK %d %d → %d", x, y, ret);
		}
		else if (strncmp(line, "DOUBLECLICK ", 12) == 0) {
			int x = 0, y = 0;
			sscanf(line + 12, "%d %d", &x, &y);
			char cmd[512];
			snprintf(cmd, sizeof(cmd), "DISPLAY=:0 xdotool mousemove %d %d click --repeat 2 --delay 200 1", x, y);
			int ret = system(cmd);
			hook_log("DOUBLECLICK %d %d → %d", x, y, ret);
		}
		else if (strncmp(line, "TYPE ", 5) == 0) {
			const char *text = line + 5;
			char cmd[1024];
			snprintf(cmd, sizeof(cmd), "DISPLAY=:0 xdotool type --delay 50 '%s'", text);
			int ret = system(cmd);
			hook_log("TYPE '%s' → %d", text, ret);
		}
		else if (strncmp(line, "SCREENSHOT ", 11) == 0) {
			const char *name = line + 11;
			char cmd[512];
			snprintf(cmd, sizeof(cmd), "DISPLAY=:0 scrot /sheepshaver/shared/%s.png", name);
			int ret = system(cmd);
			hook_log("SCREENSHOT %s → %d", name, ret);
		}
		else if (strncmp(line, "SLEEP ", 6) == 0) {
			int secs = atoi(line + 6);
			hook_log("SLEEP %d", secs);
			sleep(secs);
		}
		else if (strncmp(line, "RESTART", 7) == 0) {
			hook_log("RESTART: initiating Mac OS restart via Finder Apple Event");
			// TODO: send 'rest' Apple Event for restart. For now, use shutdown.
			send_shutdown_event();
		}
		else if (strncmp(line, "SHUTDOWN", 8) == 0) {
			hook_log("SHUTDOWN: initiating Mac OS shutdown via Finder Apple Event");
			send_shutdown_event();
		}
		else if (strncmp(line, "FIND_PLUG", 9) == 0) {
			// Find MESA's SCSI Plug code in memory by searching for the
			// Gestalt check pattern: 303C A89F A746 (move.w #$A89F,d0; _GetToolTrapAddress)
			// This is at file offset 0x6E4. Once found, we know the code base
			// and can insert trace ops at known offsets.
			hook_log("Searching for MESA SCSI Plug code in memory...");
			uint32 gestalt_addr = 0;
			for (uint32 addr = 0x10000000; addr < 0x18000000; addr += 2) {
				if (ReadMacInt16(addr) == 0x303C &&
					ReadMacInt16(addr + 2) == 0xA89F &&
					ReadMacInt16(addr + 4) == 0xA746) {
					gestalt_addr = addr;
					break;
				}
			}
			if (!gestalt_addr) {
				fprintf(stderr, "MESA_PLUG_CODE_NOT_FOUND (no 303C A89F A746 pattern)\n");
			} else {
				uint32 code_base = gestalt_addr - 0x6E4;
				fprintf(stderr, "MESA_PLUG_CODE at 0x%08x (Gestalt check at 0x%08x)\n",
					code_base, gestalt_addr);

				// File offsets don't match memory due to PEF relocation.
				// Search for each function by its unique instruction pattern.
				struct { const char *name; uint8 pattern[6]; int plen; uint32 addr; uint16 orig; } funcs[] = {
					// ChooseSCSI: 4E56 F884 48E7 (LINK a6,#-$77C; MOVEM.L)
					{"ChooseSCSI", {0x4E,0x56,0xF8,0x84,0x48,0xE7}, 6, 0, 0},
					// SCSICommand: 4E56 0000 48E7 (LINK a6,#0; MOVEM.L)
					// Too generic — many functions start with LINK a6,#0
					// Use the unique sequence after: 48E7 1830 (specific register mask)
					{"SCSICommand", {0x4E,0x56,0x00,0x00,0x48,0xE7}, 6, 0, 0},
					// IdentifyBusses: 4E56 FF54 2F0A (LINK a6,#-172; MOVE.L a2,-(sp))
					{"IdentifyBusses", {0x4E,0x56,0xFF,0x54,0x2F,0x0A}, 6, 0, 0},
				};
				int nfuncs = 3;

				// Search in a 64KB range around the Gestalt check
				uint32 search_start = gestalt_addr - 0x2000;
				uint32 search_end = gestalt_addr + 0x8000;
				for (uint32 a = search_start; a < search_end; a += 2) {
					for (int f = 0; f < nfuncs; f++) {
						if (funcs[f].addr) continue;  // already found
						bool match = true;
						for (int b = 0; b < funcs[f].plen; b++) {
							if (ReadMacInt8(a + b) != funcs[f].pattern[b]) { match = false; break; }
						}
						if (match) {
							funcs[f].addr = a;
							fprintf(stderr, "  FOUND %s at 0x%08x\n", funcs[f].name, a);
						}
					}
				}

				// Now patch: Gestalt check (known), plus found functions
				// Gestalt check is right before the trap call — patch at gestalt_addr
				uint16 gestalt_orig = ReadMacInt16(gestalt_addr);
				WriteMacInt16(gestalt_addr, 0xFE79);
				fprintf(stderr, "  PATCHED Gestalt_check at 0x%08x (was %04x)\n", gestalt_addr, gestalt_orig);

				// Patch after Gestalt flag set: gestalt_addr + 0x14 (0x6F8 - 0x6E4 = 0x14)
				uint32 flag_addr = gestalt_addr + 0x14;
				uint16 flag_orig = ReadMacInt16(flag_addr);
				WriteMacInt16(flag_addr, 0xFE79);
				fprintf(stderr, "  PATCHED Gestalt_flag at 0x%08x (was %04x)\n", flag_addr, flag_orig);

				for (int f = 0; f < nfuncs; f++) {
					if (funcs[f].addr) {
						funcs[f].orig = ReadMacInt16(funcs[f].addr);
						WriteMacInt16(funcs[f].addr, 0xFE79);
						fprintf(stderr, "  PATCHED %s at 0x%08x (was %04x)\n",
							funcs[f].name, funcs[f].addr, funcs[f].orig);
					} else {
						fprintf(stderr, "  NOT FOUND: %s\n", funcs[f].name);
					}
				}

				extern uint32 g_mesa_plug_code_base;
				g_mesa_plug_code_base = code_base;
			}
			fflush(stderr);
		}
		else {
			hook_log("Unknown command: %s", line);
		}

		line = strtok(nullptr, "\n");
	}

	free(content);
	hook_log("Command processing complete");
}

/*
 *  Called from OP_IDLE_TIME in emul_op.cpp.
 *  Runs in the Mac OS 9 emulation context.
 */
static int init_log_count = 0;
void ScriptHookIdle()
{
	if (init_log_count < 3) {
		fprintf(stderr, "ScriptHookIdle called (init=%d)\n", hook_initialized);
		fflush(stderr);
		init_log_count++;
	}
	if (!hook_initialized) {
		// Get the shared folder path from ExtFS config
		shared_path = PrefsFindString("extfs");
		if (shared_path) {
			hook_initialized = true;
			fprintf(stderr, "BOOT_COMPLETE: Mac OS 9 idle handler active\n");

			// Search for MESA's SCSI Plug in memory by looking for its unique string
			// "AKAI & Living Memory 1995" which is at file offset 0x1A4.
			// Once found, instrument key functions with OP_PLUG_TRACE.
			{
				static const uint8 sig[] = {'A','K','A','I',' ','&',' ','L','i','v','i','n','g'};
				uint32 plug_load_addr = 0;
				for (uint32 addr = 0x10000000; addr < 0x18000000; addr += 2) {
					bool match = true;
					for (int i = 0; i < 13; i++) {
						if (ReadMacInt8(addr + i) != sig[i]) { match = false; break; }
					}
					if (match) {
						plug_load_addr = addr - 0x1A4;  // file offset of string
						fprintf(stderr, "MESA SCSI Plug found at 0x%08x (sig at 0x%08x)\n",
							plug_load_addr, addr);
						break;
					}
				}

				if (plug_load_addr) {
					// Dump key state at each function entry point.
					// Instead of patching with OP_PLUG_TRACE (which replaces instructions
					// and breaks behavior), dump the first few bytes at key offsets to
					// verify the code is there, then dump the Plug's internal state.

					// CSCSIUtils constructor area (Gestalt check at +0x6E8)
					fprintf(stderr, "  Plug+0x6D0 (constructor):");
					for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", ReadMacInt8(plug_load_addr + 0x6D0 + i));
					fprintf(stderr, "\n");

					// Check the scsi43 flag: CSCSIUtils stores at (a4+0x26A)
					// a4 for the SCSI Plug instance is stored somewhere... we don't know where.
					// But we can check the IdentifyBusses result by looking at busCount.

					// IdentifyBusses at +0x1F8E
					fprintf(stderr, "  Plug+0x1F8E (IdentifyBusses):");
					for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", ReadMacInt8(plug_load_addr + 0x1F8E + i));
					fprintf(stderr, "\n");

					// ChooseSCSI at +0x1780
					fprintf(stderr, "  Plug+0x1780 (ChooseSCSI):");
					for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", ReadMacInt8(plug_load_addr + 0x1780 + i));
					fprintf(stderr, "\n");

					// SCSICommand at +0x1C3E
					fprintf(stderr, "  Plug+0x1C3E (SCSICommand):");
					for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", ReadMacInt8(plug_load_addr + 0x1C3E + i));
					fprintf(stderr, "\n");

					// Now check if the Gestalt check code is intact
					// At +0x6E4: should be 303C A89F A746 (move.w #$A89F,d0; _GetToolTrapAddress)
					uint16 w6E4 = ReadMacInt16(plug_load_addr + 0x6E4);
					uint16 w6E6 = ReadMacInt16(plug_load_addr + 0x6E6);
					uint16 w6E8 = ReadMacInt16(plug_load_addr + 0x6E8);
					fprintf(stderr, "  Gestalt check: +0x6E4=%04x +0x6E6=%04x +0x6E8=%04x (expect 303C A89F A746)\n",
						w6E4, w6E6, w6E8);

					// Dump the entire constructor from +0x6C0 to +0x720 to trace the flow
					fprintf(stderr, "  Constructor dump (+0x6C0 to +0x730):\n");
					for (int i = 0x6C0; i < 0x730; i++) {
						if ((i - 0x6C0) % 32 == 0) fprintf(stderr, "    +%04x:", i);
						fprintf(stderr, " %02x", ReadMacInt8(plug_load_addr + i));
						if ((i - 0x6C0) % 32 == 31) fprintf(stderr, "\n");
					}
					fprintf(stderr, "\n");
				} else {
					fprintf(stderr, "MESA SCSI Plug NOT found in memory (MESA not running?)\n");
				}
			}
			fflush(stderr);

			hook_log("Script hook initialized. Shared path: %s", shared_path);
			hook_log("Drop 'command.txt' in the shared folder to execute commands.");

			// Find SCSI Plug in memory and log its base address.
			// Pattern: 285F 200C 6704 7E01 at dump offset 0x06EC
			// NOTE: Do NOT insert OP_PLUG_TRACE into Plug code — it replaces
			// original instructions and breaks the Plug's behavior.
			{
				static const uint8 pattern[] = {0x28, 0x5F, 0x20, 0x0C, 0x67, 0x04, 0x7E, 0x01};
				uint32 pattern_addr = 0;
				for (uint32 addr = 0x10000000; addr < 0x11000000; addr += 2) {
					bool match = true;
					for (int i = 0; i < 8; i++) {
						if (ReadMacInt8(addr + i) != pattern[i]) { match = false; break; }
					}
					if (match) { pattern_addr = addr; break; }
				}
				if (pattern_addr) {
					uint32 plug_base = pattern_addr - 0x06EC;
					fprintf(stderr, "SCSI Plug found: pattern at 0x%08x, base at 0x%08x\n",
						pattern_addr, plug_base);

					// Check if the Plug's trap patches are installed
					// OS trap table is at low memory 0x0400, each entry is 4 bytes
					// _Read = A002 → trap table at 0x0400 + 2*4 = 0x0408
					// _Write = A003 → 0x040C
					// _Control = A004 → 0x0410
					// _Status = A005 → 0x0414
					uint32 read_handler = ReadMacInt32(0x0400 + 0x02 * 4);
					uint32 write_handler = ReadMacInt32(0x0400 + 0x03 * 4);
					uint32 control_handler = ReadMacInt32(0x0400 + 0x04 * 4);
					uint32 status_handler = ReadMacInt32(0x0400 + 0x05 * 4);
					fprintf(stderr, "  OS trap table: _Read=0x%08x _Write=0x%08x _Control=0x%08x _Status=0x%08x\n",
						read_handler, write_handler, control_handler, status_handler);
					fprintf(stderr, "  Expected Plug handlers: _Read=0x%08x _Write=0x%08x _Control=0x%08x _Status=0x%08x\n",
						plug_base + 0x0D60, plug_base + 0x0D94, plug_base + 0x0E20, plug_base + 0x0DC8);
					// Identify what overwrote _Control by dumping memory around the handler
					if (control_handler != plug_base + 0x0E20) {
						fprintf(stderr, "  Conflicting _Control at 0x%08x — dumping 256 bytes before:\n", control_handler);
						// Search backward for ASCII strings (extension name)
						for (uint32 si = control_handler - 256; si < control_handler; si++) {
							uint8 c = ReadMacInt8(si);
							if (c >= 0x20 && c < 0x7F) {
								// Found printable char, read string
								char str[64] = {};
								int slen = 0;
								while (slen < 63) {
									uint8 cc = ReadMacInt8(si + slen);
									if (cc < 0x20 || cc >= 0x7F) break;
									str[slen++] = cc;
								}
								if (slen >= 4) {
									fprintf(stderr, "    String at 0x%08x: \"%s\"\n", si, str);
									si += slen - 1;
								}
							}
						}
						fflush(stderr);
					}

					// Re-install any Plug trap patches that were overwritten by later extensions
					uint32 patch_pairs[][2] = {
						{0x02, 0x0D60},  // _Read
						{0x03, 0x0D94},  // _Write
						{0x04, 0x0E20},  // _Control
						{0x05, 0x0DC8},  // _Status
					};
					const char *patch_names[] = {"_Read", "_Write", "_Control", "_Status"};
					for (int pi = 0; pi < 4; pi++) {
						uint32 expected = plug_base + patch_pairs[pi][1];
						uint32 actual = ReadMacInt32(0x0400 + patch_pairs[pi][0] * 4);
						if (actual != expected) {
							fprintf(stderr, "  FIXING %s: was 0x%08x, restoring Plug handler 0x%08x\n",
								patch_names[pi], actual, expected);
							WriteMacInt32(0x0400 + patch_pairs[pi][0] * 4, expected);
						}
					}
					fflush(stderr);
					// Dump the Plug's device records by scanning heap for
				// structures with SCSI device type 0x03 (Processor) at +0x06
				fprintf(stderr, "Searching for Plug device records...\n");
				for (uint32 addr = 0x10000000; addr < 0x10200000; addr += 2) {
					// Device record signature: word at +0x06 has low 3 bits = 3 (Processor)
					// AND long at +0x24 is 0 (the NULL handler descriptor)
					// AND word at +0x10 is non-zero (status was set)
					uint16 w06 = ReadMacInt16(addr + 0x06);
					if ((w06 & 0x07) == 0x03) {
						uint32 v24 = ReadMacInt32(addr + 0x24);
						int16 v18 = (int16)ReadMacInt16(addr + 0x18);
						uint16 v10 = ReadMacInt16(addr + 0x10);
						uint16 v04 = ReadMacInt16(addr + 0x04);
						// Filter: v18 should be a negative refNum, v10 should be small
						if (v18 < 0 && v18 > -128 && v10 < 100) {
							fprintf(stderr, "  Candidate device record at 0x%08x:\n", addr);
							fprintf(stderr, "    +04(flags)=%04x +06(composite)=%04x +10(status)=%04x\n", v04, w06, v10);
							fprintf(stderr, "    +18(refNum)=%d +1A(scan)=%04x +24(handler)=0x%08x\n",
								v18, ReadMacInt16(addr + 0x1A), v24);
							fprintf(stderr, "    Full dump:");
							for (int i = 0; i < 0x30; i++)
								fprintf(stderr, " %02x", ReadMacInt8(addr + i));
							fprintf(stderr, "\n");
						}
					}
				}
				fflush(stderr);
				// Dump the PPC code region at 0x1002xxxx
				// (jump table at 0x109B points to functions here)
				const char *extfs2 = PrefsFindString("extfs");
				if (extfs2) {
					char path2[512];
					snprintf(path2, sizeof(path2), "%s/ppc_code.bin", extfs2);
					FILE *df2 = fopen(path2, "wb");
					if (df2) {
						uint32 ppc_base = 0x10020000;
						for (uint32 si = 0; si < 0x20000; si++)
							fputc(ReadMacInt8(ppc_base + si), df2);
						fclose(df2);
						fprintf(stderr, "  Dumped 128KB PPC code at 0x%08x\n", ppc_base);
					}
				}
				hook_log("SCSI Plug base at 0x%08x", plug_base);


					// Dump Plug code to shared folder for offline analysis
					const char *extfs = PrefsFindString("extfs");
					if (extfs) {
						char path[512];
						snprintf(path, sizeof(path), "%s/plug_full.bin", extfs);
						FILE *df = fopen(path, "wb");
						if (df) {
							// Dump 32KB of Plug code
							for (uint32 i = 0; i < 0x8000; i++)
								fputc(ReadMacInt8(plug_base + i), df);
							fclose(df);
							fprintf(stderr, "  Dumped 32KB of Plug code to %s\n", path);
						}
					}
					fflush(stderr);
				}
			}
		}
		return;
	}

	// Only check periodically
	if (++idle_counter < CHECK_INTERVAL) return;
	idle_counter = 0;

	process_command_file();
}
