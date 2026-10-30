// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/Resync.h"

#include <algorithm>
#include <tuple>

namespace GemRB {

uint64_t NetHash(const uint8_t* data, size_t size)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < size; ++i) {
		hash ^= data[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

bool NetIsSafeFileName(const std::string& name)
{
	if (name.empty() || name.size() > 64 || name[0] == '.') return false;
	for (char c : name) {
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
		if (!ok) return false;
	}
	return true;
}

std::vector<uint8_t> NetPackFiles(const std::vector<NetFile>& files)
{
	// reuse the packet writer for the byte order, then drop its header
	NetWriter w(NetMsgType::SaveChunk);
	w.U16(uint16_t(std::min(files.size(), NET_MAX_ENTRIES)));
	for (size_t i = 0; i < files.size() && i < NET_MAX_ENTRIES; ++i) {
		w.Str(files[i].name);
		w.Bytes(files[i].data.data(), files[i].data.size());
	}
	const auto& raw = w.Data();
	return std::vector<uint8_t>(raw.begin() + NET_HEADER_SIZE, raw.end());
}

bool NetUnpackFiles(const uint8_t* data, size_t size, std::vector<NetFile>& files)
{
	// give the reader the header it expects
	std::vector<uint8_t> buffer = NetWriter(NetMsgType::SaveChunk).Data();
	buffer.insert(buffer.end(), data, data + size);
	NetReader r(buffer.data(), buffer.size());
	NetMsgType type;
	if (!r.Header(type)) return false;

	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ENTRIES) return false;
	files.clear();
	files.resize(count);
	for (auto& file : files) {
		file.name = r.Str();
		file.data = r.Bytes(NET_MAX_SAVE_SIZE);
		if (!r.Ok() || !NetIsSafeFileName(file.name)) return false;
	}
	return r.AtEnd();
}

NetSaveSender::NetSaveSender(uint32_t transferID, std::vector<uint8_t> packed, std::string area)
	: data(std::move(packed))
{
	begin.transferID = transferID;
	begin.size = uint32_t(data.size());
	begin.hash = NetHash(data.data(), data.size());
	begin.area = std::move(area);
}

NetSaveChunk NetSaveSender::NextChunk(size_t chunkSize)
{
	chunkSize = std::min(chunkSize, NET_SAVE_CHUNK);
	NetSaveChunk chunk;
	chunk.transferID = begin.transferID;
	chunk.offset = uint32_t(offset);
	size_t len = std::min(chunkSize, data.size() - offset);
	chunk.data.assign(data.begin() + offset, data.begin() + offset + len);
	offset += len;
	return chunk;
}

NetSaveReceiver::Status NetSaveReceiver::Fail(std::string reason)
{
	error = std::move(reason);
	data.clear();
	status = Status::Failed;
	return status;
}

NetSaveReceiver::Status NetSaveReceiver::Begin(const NetSaveBegin& msg)
{
	Reset();
	begin = msg;
	if (msg.size == 0 || msg.size > NET_MAX_SAVE_SIZE) {
		return Fail("bad save size " + std::to_string(msg.size));
	}
	data.reserve(msg.size);
	status = Status::Receiving;
	return status;
}

NetSaveReceiver::Status NetSaveReceiver::Add(const NetSaveChunk& chunk)
{
	if (status != Status::Receiving) {
		return Fail("chunk outside of a transfer");
	}
	if (chunk.transferID != begin.transferID) {
		return Fail("chunk for transfer " + std::to_string(chunk.transferID) + ", expected " + std::to_string(begin.transferID));
	}
	if (chunk.offset != data.size() || chunk.data.empty()) {
		return Fail("chunk at offset " + std::to_string(chunk.offset) + ", expected " + std::to_string(data.size()));
	}
	if (data.size() + chunk.data.size() > begin.size) {
		return Fail("more data than announced");
	}
	data.insert(data.end(), chunk.data.begin(), chunk.data.end());
	if (data.size() < begin.size) {
		return status;
	}
	if (NetHash(data.data(), data.size()) != begin.hash) {
		return Fail("hash mismatch");
	}
	status = Status::Complete;
	return status;
}

void NetSaveReceiver::Reset()
{
	begin = NetSaveBegin();
	data.clear();
	data.shrink_to_fit();
	error.clear();
	status = Status::Idle;
}

std::vector<std::pair<uint32_t, uint32_t>> NetMatchActors(const std::vector<NetActorTableEntry>& host, const std::vector<NetActorTableEntry>& local, int maxDistance)
{
	std::vector<std::pair<uint32_t, uint32_t>> matches;
	std::vector<bool> hostDone(host.size());
	std::vector<bool> localDone(local.size());

	// the party is saved in the GAM in slot order, so slots are exact
	for (size_t h = 0; h < host.size(); ++h) {
		if (!host[h].partySlot) continue;
		for (size_t l = 0; l < local.size(); ++l) {
			if (localDone[l] || local[l].partySlot != host[h].partySlot) continue;
			matches.emplace_back(host[h].netID, local[l].netID);
			hostDone[h] = localDone[l] = true;
			break;
		}
	}

	// everyone else: all candidate pairs with the same area and name, nearest first;
	// ties keep list order, so identical stacks of actors pair up in load order
	using Candidate = std::tuple<int64_t, size_t, size_t>;
	std::vector<Candidate> candidates;
	int64_t maxDist2 = int64_t(maxDistance) * maxDistance;
	for (size_t h = 0; h < host.size(); ++h) {
		if (hostDone[h]) continue;
		for (size_t l = 0; l < local.size(); ++l) {
			if (localDone[l] || local[l].partySlot) continue;
			if (local[l].area != host[h].area || local[l].scriptName != host[h].scriptName) continue;
			int64_t dx = int64_t(local[l].x) - host[h].x;
			int64_t dy = int64_t(local[l].y) - host[h].y;
			int64_t dist2 = dx * dx + dy * dy;
			if (dist2 > maxDist2) continue;
			candidates.emplace_back(dist2, h, l);
		}
	}
	std::sort(candidates.begin(), candidates.end());
	for (const auto& candidate : candidates) {
		size_t h = std::get<1>(candidate);
		size_t l = std::get<2>(candidate);
		if (hostDone[h] || localDone[l]) continue;
		matches.emplace_back(host[h].netID, local[l].netID);
		hostDone[h] = localDone[l] = true;
	}
	return matches;
}

}
