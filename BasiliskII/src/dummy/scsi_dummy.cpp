/*
 *  scsi_s2p.cpp - SCSI Manager, network bridge to scsi2pi (s2p)
 *
 *  Forwards SCSI commands to a remote s2p instance over TCP using
 *  the s2p protobuf API. This enables SheepShaver running on any
 *  host to access SCSI devices connected to a Raspberry Pi running
 *  scsi2pi with a PiSCSI board.
 *
 *  Configuration (in SheepShaver prefs file):
 *    s2p_host  <hostname>    (default: localhost)
 *    s2p_port  <port>        (default: 6868)
 *
 *  Copyright (C) 2026 AudioControl
 *  Licensed under GPL v2+
 */

#include "sysdeps.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "main.h"
#include "prefs.h"
#include "scsi.h"

#define DEBUG 1
#include "debug.h"

// ---------------------------------------------------------------------------
// Protobuf encoding helpers (hand-rolled, no protoc dependency)
// ---------------------------------------------------------------------------

static std::vector<uint8> encode_varint(uint64 value)
{
	std::vector<uint8> buf;
	do {
		uint8 byte = value & 0x7F;
		value >>= 7;
		if (value) byte |= 0x80;
		buf.push_back(byte);
	} while (value);
	return buf;
}

// Encode a protobuf field: varint
static void pb_varint(std::vector<uint8> &out, int field, uint64 value)
{
	auto tag = encode_varint((field << 3) | 0);
	out.insert(out.end(), tag.begin(), tag.end());
	auto val = encode_varint(value);
	out.insert(out.end(), val.begin(), val.end());
}

// Encode a protobuf field: bytes/string
static void pb_bytes(std::vector<uint8> &out, int field, const uint8 *data, size_t len)
{
	auto tag = encode_varint((field << 3) | 2);
	out.insert(out.end(), tag.begin(), tag.end());
	auto length = encode_varint(len);
	out.insert(out.end(), length.begin(), length.end());
	out.insert(out.end(), data, data + len);
}

// Decode a varint from buffer
static uint64 decode_varint(const uint8 *data, size_t len, size_t &pos)
{
	uint64 val = 0;
	int shift = 0;
	while (pos < len) {
		uint8 b = data[pos++];
		val |= (uint64)(b & 0x7F) << shift;
		shift += 7;
		if (!(b & 0x80)) break;
	}
	return val;
}

// Simple protobuf field reader
struct PbField {
	int field_num;
	int wire_type;
	uint64 varint_val;
	const uint8 *bytes_ptr;
	size_t bytes_len;
};

static std::vector<PbField> pb_parse(const uint8 *data, size_t len)
{
	std::vector<PbField> fields;
	size_t pos = 0;
	while (pos < len) {
		uint64 tag = decode_varint(data, len, pos);
		PbField f;
		f.field_num = tag >> 3;
		f.wire_type = tag & 7;
		f.varint_val = 0;
		f.bytes_ptr = nullptr;
		f.bytes_len = 0;
		if (f.wire_type == 0) {
			f.varint_val = decode_varint(data, len, pos);
		} else if (f.wire_type == 2) {
			f.bytes_len = decode_varint(data, len, pos);
			f.bytes_ptr = data + pos;
			pos += f.bytes_len;
		} else if (f.wire_type == 5) {
			pos += 4;
		} else if (f.wire_type == 1) {
			pos += 8;
		} else {
			break;
		}
		fields.push_back(f);
	}
	return fields;
}

// ---------------------------------------------------------------------------
// s2p TCP client
// ---------------------------------------------------------------------------

static const char *s2p_host = "localhost";
static int s2p_port = 6868;

// Send a protobuf command to s2p and receive the result.
// Returns true on success, populates result_data/result_len.
static bool s2p_command(const std::vector<uint8> &payload,
                        std::vector<uint8> &result)
{
	result.clear();

	// Resolve host
	struct addrinfo hints = {}, *res;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", s2p_port);
	if (getaddrinfo(s2p_host, port_str, &hints, &res) != 0) {
		D(bug("scsi_s2p: cannot resolve %s:%d\n", s2p_host, s2p_port));
		return false;
	}

	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock < 0) {
		freeaddrinfo(res);
		fprintf(stderr, "scsi_s2p: socket() failed: %s\n", strerror(errno));
		return false;
	}

	// Set timeout
	struct timeval tv = { 30, 0 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
		freeaddrinfo(res);
		close(sock);
		fprintf(stderr, "scsi_s2p: connect to %s:%d failed: %s\n", s2p_host, s2p_port, strerror(errno));
		return false;
	}
	freeaddrinfo(res);

	// Send: "RASCSI" magic + 4-byte LE length + payload
	const char *magic = "RASCSI";
	if (write(sock, magic, 6) != 6) { close(sock); return false; }
	uint32 len = payload.size();
	uint8 len_buf[4] = {
		(uint8)(len & 0xFF), (uint8)((len >> 8) & 0xFF),
		(uint8)((len >> 16) & 0xFF), (uint8)((len >> 24) & 0xFF)
	};
	if (write(sock, len_buf, 4) != 4) { close(sock); return false; }
	size_t written = 0;
	while (written < payload.size()) {
		ssize_t n = write(sock, payload.data() + written, payload.size() - written);
		if (n <= 0) { close(sock); return false; }
		written += n;
	}

	// Read response: 4-byte LE length + payload
	uint8 resp_len_buf[4];
	size_t got = 0;
	while (got < 4) {
		ssize_t n = read(sock, resp_len_buf + got, 4 - got);
		if (n <= 0) { close(sock); return false; }
		got += n;
	}
	uint32 resp_len = resp_len_buf[0] | (resp_len_buf[1] << 8) |
	                  (resp_len_buf[2] << 16) | (resp_len_buf[3] << 24);

	result.resize(resp_len);
	got = 0;
	while (got < resp_len) {
		ssize_t n = read(sock, result.data() + got, resp_len - got);
		if (n <= 0) { close(sock); return false; }
		got += n;
	}

	close(sock);
	return true;
}

// Build a SCSI_EXEC protobuf command
static std::vector<uint8> build_scsi_exec(int target_id, int target_lun,
                                           const uint8 *cdb, int cdb_len,
                                           const uint8 *data_out, size_t data_out_len,
                                           int expected_data_in, int timeout)
{
	// Build PbScsiRequest
	std::vector<uint8> scsi_req;
	pb_varint(scsi_req, 1, target_id);
	pb_varint(scsi_req, 2, target_lun);
	pb_bytes(scsi_req, 3, cdb, cdb_len);
	if (data_out && data_out_len > 0) {
		pb_bytes(scsi_req, 4, data_out, data_out_len);
	}
	pb_varint(scsi_req, 5, expected_data_in);
	pb_varint(scsi_req, 6, timeout);

	// Build PbCommand
	std::vector<uint8> cmd;
	pb_varint(cmd, 1, 210); // SCSI_EXEC = 210
	// field 21 = scsi_request (length-delimited)
	pb_bytes(cmd, 21, scsi_req.data(), scsi_req.size());

	return cmd;
}

// Parse PbScsiResponse from PbResult
struct ScsiResult {
	bool ok;
	int status;
	std::vector<uint8> sense_data;
	std::vector<uint8> data_in;
	int bytes_transferred;
};

static ScsiResult parse_scsi_result(const std::vector<uint8> &result_data)
{
	ScsiResult r = { false, 0, {}, {}, 0 };

	auto fields = pb_parse(result_data.data(), result_data.size());
	for (auto &f : fields) {
		if (f.field_num == 1 && f.wire_type == 0) {
			r.ok = f.varint_val != 0; // PbResult.status
		}
		if (f.field_num == 102 && f.wire_type == 2) {
			// PbScsiResponse
			auto inner = pb_parse(f.bytes_ptr, f.bytes_len);
			for (auto &g : inner) {
				if (g.field_num == 1 && g.wire_type == 0) r.status = g.varint_val;
				if (g.field_num == 2 && g.wire_type == 2) r.sense_data.assign(g.bytes_ptr, g.bytes_ptr + g.bytes_len);
				if (g.field_num == 3 && g.wire_type == 2) r.data_in.assign(g.bytes_ptr, g.bytes_ptr + g.bytes_len);
				if (g.field_num == 4 && g.wire_type == 0) r.bytes_transferred = g.varint_val;
			}
		}
	}
	return r;
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static uint8 the_cmd[12];
static int the_cmd_len;
static int current_target_id = -1;
static int current_target_lun = 0;

// Cache of which targets are present (checked once on init)
static bool target_present[8];

// Autosense buffer
static uint8 sense_buffer[256];
static bool have_sense = false;

// ---------------------------------------------------------------------------
// SCSI backend implementation
// ---------------------------------------------------------------------------

void SCSIInit(void)
{
	// Read s2p connection settings from prefs
	const char *host = PrefsFindString("s2p_host");
	if (host) s2p_host = host;
	const char *port = PrefsFindString("s2p_port");
	if (port) s2p_port = atoi(port);

	fprintf(stderr, "scsi_s2p: connecting to s2p at %s:%d\n", s2p_host, s2p_port);
	fflush(stderr);
	// Also write to a file for debugging in Docker
	FILE *logf = fopen("/tmp/scsi_s2p.log", "w");
	if (logf) { fprintf(logf, "scsi_s2p: connecting to s2p at %s:%d\n", s2p_host, s2p_port); fflush(logf); fclose(logf); }

	// Probe all 8 SCSI IDs to check which targets are present
	for (int id = 0; id < 8; id++) {
		// Send INQUIRY via SCSI_EXEC
		uint8 inquiry_cdb[6] = { 0x12, 0x00, 0x00, 0x00, 0x24, 0x00 };
		auto cmd = build_scsi_exec(id, 0, inquiry_cdb, 6, nullptr, 0, 36, 3);
		std::vector<uint8> result;
		if (s2p_command(cmd, result)) {
			auto r = parse_scsi_result(result);
			fprintf(stderr, "scsi_s2p: target %d: ok=%d status=%d data_in=%zu\n", id, r.ok, r.status, r.data_in.size());
			target_present[id] = r.ok && r.status == 0 && r.data_in.size() >= 5;
			if (target_present[id]) {
				fprintf(stderr, "scsi_s2p: target %d present (type %d)\n", id, r.data_in[0] & 0x1F);
			}
		} else {
			target_present[id] = false;
			fprintf(stderr, "scsi_s2p: target %d probe: s2p_command returned false\n", id);
		}
	}

	int count = 0;
	for (int id = 0; id < 8; id++) if (target_present[id]) count++;
	fprintf(stderr, "scsi_s2p: init complete, %d target(s) found\n", count);

	SCSIReset();
}

void SCSIExit(void)
{
	D(bug("scsi_s2p: exit\n"));
}

void scsi_set_cmd(int cmd_length, uint8 *cmd)
{
	memcpy(the_cmd, cmd, the_cmd_len = cmd_length);

	// Log the CDB
	D(bug("scsi_s2p: set_cmd(%d):", cmd_length));
	for (int i = 0; i < cmd_length; i++) D(bug(" %02x", cmd[i]));
	D(bug("\n"));
}

bool scsi_is_target_present(int id)
{
	if (id < 0 || id > 7) return false;
	return target_present[id];
}

bool scsi_set_target(int id, int lun)
{
	if (id < 0 || id > 7) return false;
	if (!target_present[id]) return false;
	current_target_id = id;
	current_target_lun = lun;
	have_sense = false;
	D(bug("scsi_s2p: set_target(%d, %d)\n", id, lun));
	return true;
}

bool scsi_send_cmd(size_t data_length, bool reading, int sg_size,
                   uint8 **sg_ptr, uint32 *sg_len, uint16 *stat, uint32 timeout)
{
	if (current_target_id < 0) return false;

	// Log the command
	D(bug("scsi_s2p: send_cmd target=%d:%d cdb=", current_target_id, current_target_lun));
	for (int i = 0; i < the_cmd_len; i++) D(bug("%02x ", the_cmd[i]));
	D(bug(" %s %zu bytes, %d sg entries\n", reading ? "READ" : "WRITE", data_length, sg_size));

	// Handle autosense: if this is REQUEST SENSE and we have cached sense
	if (reading && the_cmd[0] == 0x03 && have_sense) {
		D(bug("scsi_s2p: returning cached sense data\n"));
		size_t copy_len = data_length < 18 ? data_length : 18;
		uint8 *buffer_ptr = sense_buffer;
		for (int i = 0; i < sg_size && copy_len > 0; i++) {
			size_t chunk = sg_len[i] < copy_len ? sg_len[i] : copy_len;
			memcpy(sg_ptr[i], buffer_ptr, chunk);
			buffer_ptr += chunk;
			copy_len -= chunk;
		}
		have_sense = false;
		*stat = 0;
		return true;
	}

	// Flatten S/G table for writes
	std::vector<uint8> data_out;
	if (!reading && data_length > 0) {
		data_out.reserve(data_length);
		for (int i = 0; i < sg_size; i++) {
			data_out.insert(data_out.end(), sg_ptr[i], sg_ptr[i] + sg_len[i]);
		}
	}

	// Build and send SCSI_EXEC command
	int timeout_sec = timeout > 0 ? (timeout / 60) + 1 : 10;
	auto cmd = build_scsi_exec(
		current_target_id, current_target_lun,
		the_cmd, the_cmd_len,
		reading ? nullptr : data_out.data(),
		reading ? 0 : data_out.size(),
		reading ? data_length : 0,
		timeout_sec
	);

	std::vector<uint8> result;
	if (!s2p_command(cmd, result)) {
		D(bug("scsi_s2p: s2p_command failed\n"));
		*stat = 2; // CHECK CONDITION
		return false;
	}

	auto r = parse_scsi_result(result);
	if (!r.ok) {
		D(bug("scsi_s2p: SCSI_EXEC returned error\n"));
		*stat = 2;
		return false;
	}

	*stat = r.status << 1; // Mac SCSI status is shifted left by 1

	D(bug("scsi_s2p: status=%d, data_in=%zu bytes\n", r.status, r.data_in.size()));

	// Scatter response data back to S/G table
	if (reading && !r.data_in.empty()) {
		const uint8 *src = r.data_in.data();
		size_t remaining = r.data_in.size();
		for (int i = 0; i < sg_size && remaining > 0; i++) {
			size_t chunk = sg_len[i] < remaining ? sg_len[i] : remaining;
			memcpy(sg_ptr[i], src, chunk);
			src += chunk;
			remaining -= chunk;
		}
	}

	// Cache sense data if CHECK CONDITION
	if (r.status == 2 && !r.sense_data.empty()) {
		size_t copy_len = r.sense_data.size() < sizeof(sense_buffer) ? r.sense_data.size() : sizeof(sense_buffer);
		memcpy(sense_buffer, r.sense_data.data(), copy_len);
		have_sense = true;
	}

	return true;
}
