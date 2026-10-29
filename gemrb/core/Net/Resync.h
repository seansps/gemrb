// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Resync.h
 * Joining and resyncing by save transfer, minus the transport and the engine.
 *
 * The host writes a normal save into a scratch directory, packs its files
 * into one blob and streams it in chunks with a size and hash up front. The
 * client reassembles and verifies it, unpacks the files and loads them like
 * any other save. Afterwards the host's actor table (network ID, area,
 * scripting name, position) is matched against what the client loaded.
 *
 * Everything here is pure, so it is unit tested without ENet or a game.
 */

#ifndef NET_RESYNC_H
#define NET_RESYNC_H

#include "exports.h"

#include "Net/Protocol.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GemRB {

// 64 bit FNV-1a, to catch corrupt or mismatched transfers (not an integrity guarantee against a hostile host)
GEM_EXPORT uint64_t NetHash(const uint8_t* data, size_t size);

struct NetFile {
	std::string name;
	std::vector<uint8_t> data;
};

// plain file names only: no directories, no dot files, no "..", nothing a host could use to escape the target dir
GEM_EXPORT bool NetIsSafeFileName(const std::string& name);
// u16 file count, then per file a u16 name length, the name, a u32 size and the data (little-endian)
GEM_EXPORT std::vector<uint8_t> NetPackFiles(const std::vector<NetFile>& files);
// strict like the packet decoders: false on truncation, trailing bytes or unsafe names
GEM_EXPORT bool NetUnpackFiles(const uint8_t* data, size_t size, std::vector<NetFile>& files);

// host side of one transfer: hands out the packed save a chunk at a time
class GEM_EXPORT NetSaveSender {
	NetSaveBegin begin;
	std::vector<uint8_t> data;
	size_t offset = 0;

public:
	NetSaveSender(uint32_t transferID, std::vector<uint8_t> packed, std::string area);

	const NetSaveBegin& Begin() const { return begin; }
	bool Done() const { return offset >= data.size(); }
	size_t Sent() const { return offset; }
	NetSaveChunk NextChunk(size_t chunkSize = NET_SAVE_CHUNK);
};

// client side of one transfer: chunks arrive in order on a reliable channel, so any gap is an error
class GEM_EXPORT NetSaveReceiver {
public:
	enum class Status : uint8_t {
		Idle,
		Receiving,
		Complete, // all bytes are in and the hash matches
		Failed
	};

private:
	NetSaveBegin begin;
	std::vector<uint8_t> data;
	Status status = Status::Idle;
	std::string error;

	Status Fail(std::string reason);

public:
	// starts over, dropping any transfer in progress
	Status Begin(const NetSaveBegin& msg);
	Status Add(const NetSaveChunk& chunk);
	void Reset();

	Status GetStatus() const { return status; }
	const std::string& Error() const { return error; }
	const NetSaveBegin& Info() const { return begin; }
	size_t Received() const { return data.size(); }
	const std::vector<uint8_t>& Data() const { return data; }
};

/**
 * Matches the host's actor table to the actors the client got from loading the same save.
 * Both lists use NetActorTableEntry; for local entries netID is the local global ID.
 * Party members pair up by party slot. Everyone else pairs within the same area and
 * scripting name, closest positions first (at most maxDistance apart), so duplicates
 * like a herd of rabbits still resolve, and load order doesn't matter.
 * Returns {network ID, local ID} pairs.
 */
GEM_EXPORT std::vector<std::pair<uint32_t, uint32_t>> NetMatchActors(const std::vector<NetActorTableEntry>& host, const std::vector<NetActorTableEntry>& local, int maxDistance);

}

#endif
