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
		w.U8(uint8_t(entry.kind));
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
		uint8_t kind = r.U8();
		if (kind >= uint8_t(NetObjectKind::count)) r.Fail();
		entry.kind = NetObjectKind(kind);
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
	w.I32(int0);
	w.I32(int1);
	w.I32(int2);
	w.I32(x);
	w.I32(y);
	w.Str(string0);
	w.Str(string1);
	w.U32(actionFlags);
	w.U8(objectMask);
	for (size_t i = 0; i < NET_ACTION_OBJECTS; ++i) {
		if (!(objectMask & (1 << i))) continue;
		const NetActionObject& object = objects[i];
		for (int32_t field : object.fields) {
			w.I32(field);
		}
		for (int32_t filter : object.filters) {
			w.I32(filter);
		}
		for (int32_t value : object.rect) {
			w.I32(value);
		}
		w.Str(object.name);
	}
}

bool NetCommand::Read(NetReader& r)
{
	actorID = r.U32();
	flags = r.U8();
	action = r.Str();
	int0 = r.I32();
	int1 = r.I32();
	int2 = r.I32();
	x = r.I32();
	y = r.I32();
	string0 = r.Str();
	string1 = r.Str();
	actionFlags = r.U32();
	objectMask = r.U8();
	if (objectMask >= (1 << NET_ACTION_OBJECTS)) r.Fail();
	for (size_t i = 0; i < NET_ACTION_OBJECTS && r.Ok(); ++i) {
		objects[i] = NetActionObject();
		if (!(objectMask & (1 << i))) continue;
		NetActionObject& object = objects[i];
		for (int32_t& field : object.fields) {
			field = r.I32();
		}
		for (int32_t& filter : object.filters) {
			filter = r.I32();
		}
		for (int32_t& value : object.rect) {
			value = r.I32();
		}
		object.name = r.Str();
	}
	return r.Ok();
}

void NetActorState::Write(NetWriter& w) const
{
	w.U32(tick);
	w.U8(flags);
	w.Str(area);
	if (flags & CHECKSUM) {
		w.U64(checksum);
	}
	size_t count = std::min(actors.size(), NET_MAX_ENTRIES);
	w.U16(uint16_t(count));
	for (size_t i = 0; i < count; ++i) {
		const auto& entry = actors[i];
		w.U32(entry.netID);
		w.U8(entry.fields);
		if (entry.fields & NAF_POS) {
			w.I32(entry.x);
			w.I32(entry.y);
		}
		if (entry.fields & NAF_ORIENT) {
			w.U8(entry.orientation);
		}
		if (entry.fields & NAF_STANCE) {
			w.U8(entry.stance);
		}
		if (entry.fields & NAF_HP) {
			w.I32(entry.hp);
			w.I32(entry.maxHP);
		}
		if (entry.fields & NAF_STATE) {
			w.U32(entry.state);
		}
	}
}

bool NetActorState::Read(NetReader& r)
{
	tick = r.U32();
	flags = r.U8();
	area = r.Str();
	checksum = flags & CHECKSUM ? r.U64() : 0;
	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ENTRIES) return false;
	actors.resize(count);
	for (auto& entry : actors) {
		entry = NetActorStateEntry();
		entry.netID = r.U32();
		entry.fields = r.U8();
		if (entry.fields & ~NAF_ALL) r.Fail();
		if (entry.fields & NAF_POS) {
			entry.x = r.I32();
			entry.y = r.I32();
		}
		if (entry.fields & NAF_ORIENT) {
			entry.orientation = r.U8();
		}
		if (entry.fields & NAF_STANCE) {
			entry.stance = r.U8();
		}
		if (entry.fields & NAF_HP) {
			entry.hp = r.I32();
			entry.maxHP = r.I32();
		}
		if (entry.fields & NAF_STATE) {
			entry.state = r.U32();
		}
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
	size_t count = std::min(unmatched.size(), NET_MAX_ENTRIES);
	w.U16(uint16_t(count));
	for (size_t i = 0; i < count; ++i) {
		w.U32(unmatched[i]);
	}
}

bool NetReady::Read(NetReader& r)
{
	transferID = r.U32();
	mapped = r.U16();
	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ENTRIES) return false;
	unmatched.resize(count);
	for (auto& id : unmatched) {
		id = r.U32();
	}
	return r.Ok();
}

void NetResyncRequest::Write(NetWriter& w) const
{
	w.U8(reason);
}

bool NetResyncRequest::Read(NetReader& r)
{
	reason = r.U8();
	return r.Ok();
}

void NetSpawn::Write(NetWriter& w) const
{
	w.U32(netID);
	w.Str(area);
	w.Str(scriptName);
	w.I32(x);
	w.I32(y);
	w.U8(orientation);
	w.Bytes(creature.data(), std::min(creature.size(), NET_MAX_CREATURE_SIZE));
}

bool NetSpawn::Read(NetReader& r)
{
	netID = r.U32();
	area = r.Str();
	scriptName = r.Str();
	x = r.I32();
	y = r.I32();
	orientation = r.U8();
	creature = r.Bytes(NET_MAX_CREATURE_SIZE);
	return r.Ok();
}

void NetDespawn::Write(NetWriter& w) const
{
	w.U32(netID);
}

bool NetDespawn::Read(NetReader& r)
{
	netID = r.U32();
	return r.Ok();
}

void NetProjectile::Write(NetWriter& w) const
{
	w.Str(name);
	w.U16(index);
	w.U8(flags);
	w.U32(casterID);
	w.U32(targetID);
	w.I32(x);
	w.I32(y);
	w.I32(tx);
	w.I32(ty);
}

bool NetProjectile::Read(NetReader& r)
{
	name = r.Str();
	index = r.U16();
	flags = r.U8();
	casterID = r.U32();
	targetID = r.U32();
	x = r.I32();
	y = r.I32();
	tx = r.I32();
	ty = r.I32();
	return r.Ok();
}

void NetVfx::Write(NetWriter& w) const
{
	w.U32(key);
	w.U8(flags);
	if (flags & END) return;
	w.U32(actorID);
	w.Str(resource);
	w.I32(x);
	w.I32(y);
	w.I32(xOffset);
	w.I32(yOffset);
	w.I32(zOffset);
	w.U32(tint);
	w.U32(transparency);
	w.U32(sequenceFlags);
	w.U32(duration);
	w.U8(orientation);
}

bool NetVfx::Read(NetReader& r)
{
	key = r.U32();
	flags = r.U8();
	if (flags & END) return r.Ok();
	actorID = r.U32();
	resource = r.Str();
	x = r.I32();
	y = r.I32();
	xOffset = r.I32();
	yOffset = r.I32();
	zOffset = r.I32();
	tint = r.U32();
	transparency = r.U32();
	sequenceFlags = r.U32();
	duration = r.U32();
	orientation = r.U8();
	return r.Ok();
}

void NetSound::Write(NetWriter& w) const
{
	w.Str(resource);
	w.U8(flags);
	if (flags & SPATIAL) {
		w.I32(x);
		w.I32(y);
		w.I32(z);
	}
	w.U8(masterVolume);
	w.U8(channelVolume);
	w.U16(muteDistance);
}

bool NetSound::Read(NetReader& r)
{
	resource = r.Str();
	flags = r.U8();
	if (flags & SPATIAL) {
		x = r.I32();
		y = r.I32();
		z = r.I32();
	}
	masterVolume = r.U8();
	channelVolume = r.U8();
	muteDistance = r.U16();
	return r.Ok();
}

void NetText::Write(NetWriter& w) const
{
	w.U8(kind);
	if (kind == OVERHEAD) {
		w.U32(ownerID);
		w.U32(color);
	}
	w.Str(text);
}

bool NetText::Read(NetReader& r)
{
	kind = r.U8();
	if (kind > OVERHEAD) r.Fail();
	if (kind == OVERHEAD) {
		ownerID = r.U32();
		color = r.U32();
	}
	text = r.Str();
	return r.Ok();
}

void NetObjectState::Write(NetWriter& w) const
{
	w.U32(netID);
	w.U8(uint8_t(kind));
	w.U32(flags);
	w.U8(trapDetected);
	w.U8(trapped);
	w.U16(items);
}

bool NetObjectState::Read(NetReader& r)
{
	netID = r.U32();
	uint8_t raw = r.U8();
	if (raw == uint8_t(NetObjectKind::Actor) || raw >= uint8_t(NetObjectKind::count)) r.Fail();
	kind = NetObjectKind(raw);
	flags = r.U32();
	trapDetected = r.U8();
	trapped = r.U8();
	items = r.U16();
	return r.Ok();
}

void NetPause::Write(NetWriter& w) const
{
	w.U8(on);
	w.U8(slot);
}

bool NetPause::Read(NetReader& r)
{
	on = r.U8();
	slot = r.U8();
	return r.Ok();
}

void NetAreaChange::Write(NetWriter& w) const
{
	w.Str(area);
}

bool NetAreaChange::Read(NetReader& r)
{
	area = r.Str();
	return r.Ok();
}

void NetOwnership::Write(NetWriter& w) const
{
	size_t count = std::min(entries.size(), NET_MAX_ENTRIES);
	w.U16(uint16_t(count));
	for (size_t i = 0; i < count; ++i) {
		w.U32(entries[i].netID);
		w.U8(entries[i].partySlot);
		w.U8(entries[i].owner);
	}
}

bool NetOwnership::Read(NetReader& r)
{
	uint16_t count = r.U16();
	if (!r.Ok() || count > NET_MAX_ENTRIES) return false;
	entries.resize(count);
	for (auto& entry : entries) {
		entry.netID = r.U32();
		entry.partySlot = r.U8();
		entry.owner = r.U8();
	}
	return r.Ok();
}

}
