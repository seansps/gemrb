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

void NetWriter::VarU32(uint32_t value)
{
	while (value >= 0x80) {
		U8(uint8_t(value | 0x80));
		value >>= 7;
	}
	U8(uint8_t(value));
}

void NetWriter::VarI32(int32_t value)
{
	VarU32((uint32_t(value) << 1) ^ uint32_t(value >> 31));
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

uint32_t NetReader::VarU32()
{
	uint32_t value = 0;
	for (int shift = 0; shift < 35; shift += 7) {
		uint8_t byte = U8();
		if (!ok) return 0;
		// the fifth byte only has 4 bits left; anything longer or wider is malformed
		if (shift == 28 && byte > 0x0f) {
			ok = false;
			return 0;
		}
		value |= uint32_t(byte & 0x7f) << shift;
		if (!(byte & 0x80)) return value;
	}
	ok = false;
	return 0;
}

int32_t NetReader::VarI32()
{
	uint32_t zz = VarU32();
	return int32_t((zz >> 1) ^ (~(zz & 1) + 1));
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
	w.U64(token);
}

bool NetHello::Read(NetReader& r)
{
	name = r.Str();
	gameType = r.Str();
	token = r.U64();
	return r.Ok();
}

void NetWelcome::Write(NetWriter& w) const
{
	w.U8(slot);
	w.Str(hostName);
	w.U64(token);
	w.U8(flags);
}

bool NetWelcome::Read(NetReader& r)
{
	slot = r.U8();
	hostName = r.Str();
	token = r.U64();
	flags = r.U8();
	if (flags & ~(REJOINED | IN_GAME)) r.Fail();
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

std::vector<uint8_t> NetEncodeReject(const NetReject& reject, uint8_t version)
{
	std::vector<uint8_t> data = NetEncode(reject);
	data[1] = version;
	return data;
}

bool NetPeekForeign(const uint8_t* data, size_t size, uint8_t& version, uint8_t& type)
{
	if (!data || size < NET_HEADER_SIZE || data[0] != NET_MAGIC || data[1] == NET_PROTOCOL_VERSION) return false;
	version = data[1];
	type = data[2];
	return true;
}

bool NetDecodeAnyReject(const uint8_t* data, size_t size, NetReject& reject, uint8_t& version)
{
	if (!data || size < NET_HEADER_SIZE || data[0] != NET_MAGIC || data[2] != uint8_t(NetMsgType::Reject)) return false;
	version = data[1];
	// read it as if it were ours: the layout never changes
	std::vector<uint8_t> copy(data, data + size);
	copy[1] = NET_PROTOCOL_VERSION;
	return NetDecode(copy.data(), copy.size(), reject);
}

void NetChat::Write(NetWriter& w) const
{
	w.U8(slot);
	w.U8(flags);
	w.U8(target);
	w.Str(name);
	w.Str(targetName);
	w.Str(text);
}

bool NetChat::Read(NetReader& r)
{
	slot = r.U8();
	flags = r.U8();
	target = r.U8();
	name = r.Str();
	targetName = r.Str();
	text = r.Str();
	if (slot > NET_MAX_PEERS || target > NET_MAX_PEERS || flags & ~(WHISPER | SYSTEM)) r.Fail();
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

// reads a u16 list count, failing on more than limit
static size_t ReadCount(NetReader& r, size_t limit = NET_MAX_ENTRIES)
{
	uint16_t count = r.U16();
	if (count > limit) r.Fail();
	return r.Ok() ? count : 0;
}

static size_t WriteCount(NetWriter& w, size_t size, size_t limit = NET_MAX_ENTRIES)
{
	size_t count = std::min(size, limit);
	w.U16(uint16_t(count));
	return count;
}

void NetDialog::Write(NetWriter& w) const
{
	w.U8(kind);
	w.U32(seq);
	w.U8(owner);
	w.U8(flags);
	w.U32(speakerID);
	w.U32(targetID);
	w.U16(continueIndex);
	w.Str(text);
	size_t count = WriteCount(w, options.size());
	for (size_t i = 0; i < count; ++i) {
		w.U16(options[i].index);
		w.Str(options[i].text);
	}
}

bool NetDialog::Read(NetReader& r)
{
	kind = r.U8();
	if (kind >= count) r.Fail();
	seq = r.U32();
	owner = r.U8();
	flags = r.U8();
	if (flags & ~(FREEZE | CONTINUE | END_BUTTON | PARTY)) r.Fail();
	speakerID = r.U32();
	targetID = r.U32();
	continueIndex = r.U16();
	text = r.Str();
	options.resize(ReadCount(r));
	for (auto& option : options) {
		option.index = r.U16();
		option.text = r.Str();
	}
	return r.Ok();
}

void NetDialogChoice::Write(NetWriter& w) const
{
	w.U32(seq);
	w.U16(index);
}

bool NetDialogChoice::Read(NetReader& r)
{
	seq = r.U32();
	index = r.U16();
	return r.Ok();
}

void NetInventory::Write(NetWriter& w) const
{
	w.U32(netID);
	w.U8(uint8_t(kind));
	w.Str(area);
	w.I32(x);
	w.I32(y);
	w.U16(uint16_t(equipped));
	w.U16(equippedHeader);
	size_t count = WriteCount(w, items.size());
	for (size_t i = 0; i < count; ++i) {
		const NetItem& item = items[i];
		w.U16(item.slot);
		w.Str(item.resref);
		for (uint16_t usage : item.usages) {
			w.U16(usage);
		}
		w.U32(item.flags);
		w.U16(item.expired);
	}
}

bool NetInventory::Read(NetReader& r)
{
	netID = r.U32();
	uint8_t raw = r.U8();
	if (raw != uint8_t(NetObjectKind::Actor) && raw != uint8_t(NetObjectKind::Container)) r.Fail();
	kind = NetObjectKind(raw);
	area = r.Str();
	x = r.I32();
	y = r.I32();
	equipped = int16_t(r.U16());
	equippedHeader = r.U16();
	items.resize(ReadCount(r));
	for (auto& item : items) {
		item.slot = r.U16();
		item.resref = r.Str();
		for (auto& usage : item.usages) {
			usage = r.U16();
		}
		item.flags = r.U32();
		item.expired = r.U16();
	}
	return r.Ok();
}

void NetActorStats::Write(NetWriter& w) const
{
	w.U32(netID);
	w.U8(flags);
	size_t count = WriteCount(w, stats.size(), NET_MAX_STATS);
	for (size_t i = 0; i < count; ++i) {
		const NetStatEntry& entry = stats[i];
		bool same = entry.base == entry.modified;
		w.U16(uint16_t(entry.stat | (same ? SAME : 0)));
		w.VarI32(int32_t(entry.base));
		if (!same) w.VarI32(int32_t(entry.modified));
	}
}

bool NetActorStats::Read(NetReader& r)
{
	netID = r.U32();
	flags = r.U8();
	if (flags & ~FULL) r.Fail();
	stats.resize(ReadCount(r, NET_MAX_STATS));
	for (auto& entry : stats) {
		uint16_t raw = r.U16();
		entry.stat = uint16_t(raw & ~SAME);
		if (entry.stat >= NET_MAX_STATS) r.Fail();
		entry.base = uint32_t(r.VarI32());
		entry.modified = (raw & SAME) ? entry.base : uint32_t(r.VarI32());
	}
	return r.Ok();
}

void NetPartyState::Write(NetWriter& w) const
{
	w.U32(gold);
	w.U32(reputation);
}

bool NetPartyState::Read(NetReader& r)
{
	gold = r.U32();
	reputation = r.U32();
	return r.Ok();
}

void NetSpellbook::Write(NetWriter& w) const
{
	w.U32(netID);
	size_t count = WriteCount(w, pages.size());
	for (size_t i = 0; i < count; ++i) {
		const NetSpellPage& page = pages[i];
		w.U8(page.type);
		w.U8(page.level);
		w.U16(page.slots);
		w.U16(page.slotsWithBonus);
		size_t known = WriteCount(w, page.known.size());
		for (size_t k = 0; k < known; ++k) {
			w.Str(page.known[k]);
		}
		size_t memorized = WriteCount(w, page.memorized.size());
		for (size_t m = 0; m < memorized; ++m) {
			w.Str(page.memorized[m].resref);
			w.U32(page.memorized[m].flags);
		}
	}
}

bool NetSpellbook::Read(NetReader& r)
{
	netID = r.U32();
	pages.resize(ReadCount(r));
	for (auto& page : pages) {
		page.type = r.U8();
		page.level = r.U8();
		page.slots = r.U16();
		page.slotsWithBonus = r.U16();
		page.known.resize(ReadCount(r));
		for (auto& known : page.known) {
			known = r.Str();
		}
		page.memorized.resize(ReadCount(r));
		for (auto& memorized : page.memorized) {
			memorized.resref = r.Str();
			memorized.flags = r.U32();
		}
	}
	return r.Ok();
}

void NetDCCExtension::Write(NetWriter& w) const
{
	w.U32(netID);
	w.Bytes(data.data(), std::min(data.size(), NET_MAX_DCC_EXTENSION));
}

bool NetDCCExtension::Read(NetReader& r)
{
	netID = r.U32();
	data = r.Bytes(NET_MAX_DCC_EXTENSION);
	return r.Ok();
}

void NetGuiCall::Write(NetWriter& w) const
{
	w.U8(uint8_t(op));
	w.U8(selected);
	size_t count = WriteCount(w, args.size(), NET_GUI_ARGS);
	for (size_t i = 0; i < count; ++i) {
		w.I32(args[i]);
	}
	w.Str(text);
}

bool NetGuiCall::Read(NetReader& r)
{
	uint8_t raw = r.U8();
	if (raw == 0 || raw >= uint8_t(NetGuiOp::count)) r.Fail();
	op = NetGuiOp(raw);
	selected = r.U8();
	args.resize(ReadCount(r, NET_GUI_ARGS));
	for (auto& arg : args) {
		arg = r.I32();
	}
	text = r.Str();
	return r.Ok();
}

void NetStoreState::Write(NetWriter& w) const
{
	w.U8(flags);
	w.Str(store);
	w.U32(ownerID);
	w.U32(actorID);
	size_t count = WriteCount(w, items.size());
	for (size_t i = 0; i < count; ++i) {
		const NetStoreItem& item = items[i];
		w.Str(item.resref);
		for (uint16_t usage : item.usages) {
			w.U16(usage);
		}
		w.U32(item.flags);
		w.U32(item.amount);
		w.I32(item.infinite);
		w.U16(item.purchased);
	}
}

bool NetStoreState::Read(NetReader& r)
{
	flags = r.U8();
	if (flags & ~(OPEN | CLOSE)) r.Fail();
	store = r.Str();
	ownerID = r.U32();
	actorID = r.U32();
	items.resize(ReadCount(r));
	for (auto& item : items) {
		item.resref = r.Str();
		for (auto& usage : item.usages) {
			usage = r.U16();
		}
		item.flags = r.U32();
		item.amount = r.U32();
		item.infinite = r.I32();
		item.purchased = r.U16();
	}
	return r.Ok();
}

void NetContainerOpen::Write(NetWriter& w) const
{
	w.U32(actorID);
	w.U32(containerID);
	w.I32(x);
	w.I32(y);
}

bool NetContainerOpen::Read(NetReader& r)
{
	actorID = r.U32();
	containerID = r.U32();
	x = r.I32();
	y = r.I32();
	return r.Ok();
}

void NetHeartbeat::Write(NetWriter& w) const
{
	w.U32(seq);
	w.U32(time);
	w.U32(echo);
	w.U16(rtt);
}

bool NetHeartbeat::Read(NetReader& r)
{
	seq = r.U32();
	time = r.U32();
	echo = r.U32();
	rtt = r.U16();
	return r.Ok();
}

void NetLobbyState::Write(NetWriter& w) const
{
	w.U8(uint8_t(phase));
	size_t count = WriteCount(w, players.size(), size_t(NET_MAX_PEERS) + 1);
	for (size_t i = 0; i < count; ++i) {
		const NetLobbyPlayer& player = players[i];
		w.U8(player.slot);
		w.U8(player.flags);
		w.U16(player.ping);
		w.Str(player.name);
		w.U32(player.allowed);
	}
	count = WriteCount(w, party.size(), NET_PARTY_SLOTS);
	for (size_t i = 0; i < count; ++i) {
		w.U8(party[i].partySlot);
		w.U8(party[i].owner);
		w.U8(party[i].controller);
		w.Str(party[i].name);
	}
}

bool NetLobbyState::Read(NetReader& r)
{
	uint8_t raw = r.U8();
	if (raw >= uint8_t(NetPhase::count)) r.Fail();
	phase = NetPhase(raw);
	players.resize(ReadCount(r, size_t(NET_MAX_PEERS) + 1));
	for (auto& player : players) {
		player.slot = r.U8();
		player.flags = r.U8();
		if (player.slot > NET_MAX_PEERS || player.flags & ~(NetLobbyPlayer::CONNECTED | NetLobbyPlayer::READY)) r.Fail();
		player.ping = r.U16();
		player.name = r.Str();
		player.allowed = r.U32();
	}
	party.resize(ReadCount(r, NET_PARTY_SLOTS));
	for (auto& slot : party) {
		slot.partySlot = r.U8();
		slot.owner = r.U8();
		slot.controller = r.U8();
		if (!slot.partySlot || slot.partySlot > NET_PARTY_SLOTS || slot.owner > NET_MAX_PEERS || slot.controller > NET_MAX_PEERS) r.Fail();
		slot.name = r.Str();
	}
	return r.Ok();
}

void NetLobbyReady::Write(NetWriter& w) const
{
	w.U8(ready);
}

bool NetLobbyReady::Read(NetReader& r)
{
	ready = r.U8();
	if (ready > 1) r.Fail();
	return r.Ok();
}

void NetAreaData::Write(NetWriter& w) const
{
	w.U32(transferID);
	w.Str(area);
	size_t count = WriteCount(w, party.size(), NET_PARTY_SLOTS);
	for (size_t i = 0; i < count; ++i) {
		const NetAreaPartyEntry& entry = party[i];
		w.U32(entry.netID);
		w.U8(entry.partySlot);
		w.I32(entry.x);
		w.I32(entry.y);
		w.U8(entry.orientation);
	}
	w.Bytes(are.data(), are.size());
}

bool NetAreaData::Read(NetReader& r)
{
	transferID = r.U32();
	area = r.Str();
	party.resize(ReadCount(r, NET_PARTY_SLOTS));
	for (auto& entry : party) {
		entry.netID = r.U32();
		entry.partySlot = r.U8();
		entry.x = r.I32();
		entry.y = r.I32();
		entry.orientation = r.U8();
	}
	are = r.Bytes(NET_MAX_AREA_SIZE);
	return r.Ok();
}

void NetGlobals::Write(NetWriter& w) const
{
	size_t count = WriteCount(w, vars.size(), NET_MAX_ACTORS);
	for (size_t i = 0; i < count; ++i) {
		w.Str(vars[i].name);
		w.I32(vars[i].value);
	}
}

bool NetGlobals::Read(NetReader& r)
{
	vars.resize(ReadCount(r, NET_MAX_ACTORS));
	for (auto& var : vars) {
		var.name = r.Str();
		var.value = r.I32();
	}
	return r.Ok();
}

void NetChatHistory::Write(NetWriter& w) const
{
	size_t count = WriteCount(w, lines.size(), NET_CHAT_HISTORY);
	// the newest ones, if there are more
	for (size_t i = lines.size() - count; i < lines.size(); ++i) {
		lines[i].Write(w);
	}
}

bool NetChatHistory::Read(NetReader& r)
{
	lines.resize(ReadCount(r, NET_CHAT_HISTORY));
	for (auto& line : lines) {
		if (!line.Read(r)) break;
	}
	return r.Ok();
}

void NetPrompt::Write(NetWriter& w) const
{
	w.U32(id);
	w.U8(state);
	w.U32(remaining);
	w.U8(queued);
	w.U8(uint8_t(question.kind));
	w.U8(question.flags);
	w.Str(question.tag);
	w.Str(question.title);
	w.Str(question.text);
	size_t count = WriteCount(w, question.choices.size(), PROMPT_MAX_CHOICES);
	for (size_t i = 0; i < count; ++i) {
		w.Str(question.choices[i]);
	}
	count = WriteCount(w, question.fields.size(), PROMPT_MAX_FIELDS);
	for (size_t i = 0; i < count; ++i) {
		w.Str(question.fields[i].label);
		w.I32(question.fields[i].min);
		w.I32(question.fields[i].max);
		w.I32(question.fields[i].value);
	}
	w.I32(question.defaultChoice);
	w.U32(question.timeout);
	w.U32(question.actorID);
	count = WriteCount(w, voters.size(), PROMPT_MAX_VOTERS);
	for (size_t i = 0; i < count; ++i) {
		w.U8(voters[i].slot);
		w.U8(voters[i].by);
		w.U8(voters[i].state);
		w.U8(voters[i].away);
		w.Str(voters[i].name);
		w.Str(voters[i].byName);
	}
	w.U8(outcome);
	count = WriteCount(w, values.size(), PROMPT_MAX_FIELDS);
	for (size_t i = 0; i < count; ++i) {
		w.I32(values[i]);
	}
	w.Str(outcomeText);
}

bool NetPrompt::Read(NetReader& r)
{
	id = r.U32();
	state = r.U8();
	remaining = r.U32();
	queued = r.U8();
	uint8_t kind = r.U8();
	if (kind >= uint8_t(PromptKind::count) || state > CLOSED) r.Fail();
	question.kind = PromptKind(kind);
	question.flags = r.U8();
	if (question.flags & ~(PromptQuestion::VOTE | PromptQuestion::STOP_ON_NO)) r.Fail();
	question.tag = r.Str();
	question.title = r.Str();
	question.text = r.Str();
	question.choices.resize(ReadCount(r, PROMPT_MAX_CHOICES));
	for (auto& choice : question.choices) {
		choice = r.Str();
	}
	question.fields.resize(ReadCount(r, PROMPT_MAX_FIELDS));
	for (auto& field : question.fields) {
		field.label = r.Str();
		field.min = r.I32();
		field.max = r.I32();
		field.value = r.I32();
	}
	question.defaultChoice = r.I32();
	question.timeout = r.U32();
	question.actorID = r.U32();
	voters.resize(ReadCount(r, PROMPT_MAX_VOTERS));
	for (auto& voter : voters) {
		voter.slot = r.U8();
		voter.by = r.U8();
		voter.state = r.U8();
		voter.away = r.U8();
		voter.name = r.Str();
		voter.byName = r.Str();
		if (voter.state >= uint8_t(PromptVoter::State::count) || voter.away > 1) r.Fail();
	}
	outcome = r.U8();
	if (outcome >= uint8_t(PromptOutcome::count)) r.Fail();
	values.resize(ReadCount(r, PROMPT_MAX_FIELDS));
	for (auto& value : values) {
		value = r.I32();
	}
	outcomeText = r.Str();
	// a question nobody could answer isn't one
	if (r.Ok() && !PromptWellFormed(question)) r.Fail();
	return r.Ok();
}

void NetPromptAnswer::Write(NetWriter& w) const
{
	w.U32(id);
	size_t count = WriteCount(w, values.size(), PROMPT_MAX_FIELDS);
	for (size_t i = 0; i < count; ++i) {
		w.I32(values[i]);
	}
}

bool NetPromptAnswer::Read(NetReader& r)
{
	id = r.U32();
	values.resize(ReadCount(r, PROMPT_MAX_FIELDS));
	for (auto& value : values) {
		value = r.I32();
	}
	return r.Ok();
}

}
