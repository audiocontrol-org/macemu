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
			hook_log("RESTART: initiating Mac OS restart");
			M68kRegisters r;
			r.d[0] = 2; // ShutDwnRestart
			Execute68kTrap(0xa895, &r); // _ShutDown
		}
		else if (strncmp(line, "SHUTDOWN", 8) == 0) {
			hook_log("SHUTDOWN: initiating Mac OS shutdown");
			M68kRegisters r;
			r.d[0] = 1; // ShutDwnPower
			Execute68kTrap(0xa895, &r); // _ShutDown
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
					hook_log("SCSI Plug base at 0x%08x", plug_base);
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
