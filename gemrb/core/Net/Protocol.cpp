// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/Protocol.h"

#include <algorithm>

namespace GemRB {

NetWriter::NetWriter(NetMsgType type)
{
	buffer.reserve(64);
	U8(NET_MAGIC);
	U8(NET_PROTOCOL_VERSION);
	U8(uint8_t(type));
}

void NetWriter::U8(uint8_t value)
{
	buffer.push_back(value);
}

void NetWriter::U16(uint16_t value)
{
	U8(uint8_t(value & 0xff));
	U8(uint8_t(value >> 8));
}

void NetWriter::U32(uint32_t value)
{
	U16(uint16_t(value & 0xffff));
	U16(uint16_t(value >> 16));
}

void NetWriter::I32(int32_t value)
{
	U32(static_cast<uint32_t>(value));
}

void NetWriter::U64(uint64_t value)
{
	U32(uint32_t(value & 0xffffffff));
	U32(uint32_t(value >> 32));
}

void NetWriter::Str(const std::string& value)
{
	size_t len = std::min(value.size(), NET_MAX_STRING);
	U16(uint16_t(len));
	buffer.insert(buffer.end(), value.begin(), value.begin() + len);
}

void NetWriter::Bytes(const uint8_t* data, size_t size)
{
	U32(uint32_t(size));
	buffer.insert(buffer.end(), data, data + size);
}

NetReader::NetReader(const uint8_t* data, size_t size)
	: data(data), size(data ? size : 0)
{
}

bool NetReader::Need(size_t bytes)
{
	if (!ok || size - pos < bytes) {
		ok = false;
	}
	return ok;
}

bool NetReader::Header(NetMsgType& type)
{
	if (pos != 0 || !Need(NET_HEADER_SIZE)) return false;
	if (U8() != NET_MAGIC || U8() != NET_PROTOCOL_VERSION) {
		ok = false;
		return false;
	}
	uint8_t raw = U8();
	if (raw == 0 || raw >= uint8_t(NetMsgType::count)) {
		ok = false;
		return false;
	}
	type = NetMsgType(raw);
	return true;
}

uint8_t NetReader::U8()
{
	if (!Need(1)) return 0;
	return data[pos++];
}

uint16_t NetReader::U16()
{
	uint16_t lo = U8();
	uint16_t hi = U8();
	return uint16_t(lo | (hi << 8));
}

uint32_t NetReader::U32()
{
	uint32_t lo = U16();
	uint32_t hi = U16();
	return lo | (hi << 16);
}

int32_t NetReader::I32()
{
	return static_cast<int32_t>(U32());
}

uint64_t NetReader::U64()
{
	uint64_t lo = U32();
	uint64_t hi = U32();
	return lo | (hi << 32);
}

std::string NetReader::Str()
{
	uint16_t len = U16();
	if (len > NET_MAX_STRING) {
		ok = false;
	}
	if (!Need(len)) return {};
	std::string value(reinterpret_cast<const char*>(data + pos), len);
	pos += len;
	return value;
}

std::vector<uint8_t> NetReader::Bytes(size_t maxSize)
{
	uint32_t len = U32();
	if (len > maxSize) {
		ok = false;
	}
	if (!Need(len)) return {};
	std::vector<uint8_t> value(data + pos, data + pos + len);
	pos += len;
	return value;
}

bool NetPeekType(const uint8_t* data, size_t size, NetMsgType& type)
{
	NetReader r(data, size);
	return r.Header(type);
}

void NetHello::Write(NetWriter& w) const
{
	w.Str(name);
	w.Str(gameType);
}

bool NetHello::Read(NetReader& r)
{
	name = r.Str();
	gameType = r.Str();
	return r.Ok();
}

void NetWelcome::Write(NetWriter& w) const
{
	w.U8(slot);
	w.Str(hostName);
}

bool NetWelcome::Read(NetReader& r)
{
	slot = r.U8();
	hostName = r.Str();
	return r.Ok();
}

void NetReject::Write(NetWriter& w) const
{
	w.Str(reason);
}

bool NetReject::Read(NetReader& r)
{
	reason = r.Str();
	return r.Ok();
}

void NetChat::Write(NetWriter& w) const
{
	w.U8(slot);
	w.Str(name);
	w.Str(text);
}

bool NetChat::Read(NetReader& r)
{
	slot = r.U8();
	name = r.Str();
	text = r.Str();
	return r.Ok();
}

void NetActorTable::Write(NetWriter& w) const
{
	w.U32(transferID);
	size_t count = std::min(actors.size(), NET_MAX_ACTORS);
	w.U16(uint16_t(count));
	for (size_t i = 0; i < count; ++i) {
		const auto& entry = actors[i];
		w.U32(entry.netID);
		w.U8(entry.partySlot);
		w.Str(entry.area);
		w.Str(entry.scriptName);
		w.I32(entry.x);
		w.I32(entry.y);
	}
}

bool NetActorTable::Read(NetReader& r)
{
	transferID = r.U32();
	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ACTORS) return false;
	actors.resize(count);
	for (auto& entry : actors) {
		entry.netID = r.U32();
		entry.partySlot = r.U8();
		entry.area = r.Str();
		entry.scriptName = r.Str();
		entry.x = r.I32();
		entry.y = r.I32();
	}
	return r.Ok();
}

void NetCommand::Write(NetWriter& w) const
{
	w.U32(actorID);
	w.U8(flags);
	w.Str(action);
}

bool NetCommand::Read(NetReader& r)
{
	actorID = r.U32();
	flags = r.U8();
	action = r.Str();
	return r.Ok();
}

void NetActorState::Write(NetWriter& w) const
{
	w.U32(tick);
	w.Str(area);
	size_t count = std::min(actors.size(), NET_MAX_ENTRIES);
	w.U16(uint16_t(count));
	for (size_t i = 0; i < count; ++i) {
		const auto& entry = actors[i];
		w.U32(entry.netID);
		w.I32(entry.x);
		w.I32(entry.y);
		w.U8(entry.orientation);
		w.U8(entry.stance);
	}
}

bool NetActorState::Read(NetReader& r)
{
	tick = r.U32();
	area = r.Str();
	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ENTRIES) return false;
	actors.resize(count);
	for (auto& entry : actors) {
		entry.netID = r.U32();
		entry.x = r.I32();
		entry.y = r.I32();
		entry.orientation = r.U8();
		entry.stance = r.U8();
	}
	return r.Ok();
}

void NetSaveBegin::Write(NetWriter& w) const
{
	w.U32(transferID);
	w.U32(size);
	w.U64(hash);
	w.Str(area);
}

bool NetSaveBegin::Read(NetReader& r)
{
	transferID = r.U32();
	size = r.U32();
	hash = r.U64();
	area = r.Str();
	return r.Ok() && size <= NET_MAX_SAVE_SIZE;
}

void NetSaveChunk::Write(NetWriter& w) const
{
	w.U32(transferID);
	w.U32(offset);
	w.Bytes(data.data(), std::min(data.size(), NET_SAVE_CHUNK));
}

bool NetSaveChunk::Read(NetReader& r)
{
	transferID = r.U32();
	offset = r.U32();
	data = r.Bytes(NET_SAVE_CHUNK);
	return r.Ok();
}

void NetReady::Write(NetWriter& w) const
{
	w.U32(transferID);
	w.U16(mapped);
}

bool NetReady::Read(NetReader& r)
{
	transferID = r.U32();
	mapped = r.U16();
	return r.Ok();
}

}
