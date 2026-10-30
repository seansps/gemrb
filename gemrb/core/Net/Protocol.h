// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Protocol.h
 * Wire format for host-authoritative co-op.
 *
 * Every packet starts with a 3 byte header: the magic byte, the protocol
 * version and the message type. The body follows, little-endian, with
 * strings stored as a u16 byte count plus UTF-8 data. Decoding is strict:
 * a short, overlong or foreign packet is rejected as a whole.
 *
 * This file has no transport dependency, so it builds (and is tested)
 * even without ENet.
 */

#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "exports.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

constexpr uint8_t NET_MAGIC = 0xE7;
constexpr uint8_t NET_PROTOCOL_VERSION = 3;
constexpr uint16_t NET_DEFAULT_PORT = 27015;
constexpr size_t NET_HEADER_SIZE = 3;
constexpr size_t NET_MAX_STRING = 1024;
constexpr size_t NET_MAX_ENTRIES = 256;
// the actor table covers every actor, door, container and region in every loaded area
constexpr size_t NET_MAX_ACTORS = 4096;
// save transfers: chunk payload and whole save limits
constexpr size_t NET_SAVE_CHUNK = 32 * 1024;
constexpr size_t NET_MAX_SAVE_SIZE = 64 * 1024 * 1024;
// a spawned creature travels as a CRE file
constexpr size_t NET_MAX_CREATURE_SIZE = 1024 * 1024;
// slot 0 is always the host, clients get 1..NET_MAX_PEERS
constexpr uint8_t NET_MAX_PEERS = 3;
constexpr uint8_t NET_HOST_SLOT = 0;
// GameScript object layout (MAX_OBJECT_FIELDS, MAX_NESTING)
constexpr size_t NET_OBJECT_FIELDS = 10;
constexpr size_t NET_OBJECT_FILTERS = 5;
constexpr size_t NET_ACTION_OBJECTS = 3;

enum class NetChannel : uint8_t {
	Reliable = 0, // ordered and reliable: handshake, chat, commands, events
	State = 1, // unreliable but sequenced: periodic actor snapshots
	Transfer = 2, // ordered and reliable: save transfers, so they don't hold up chat and commands
	count
};

enum class NetMsgType : uint8_t {
	Hello = 1, // client -> host
	Welcome, // host -> client
	Reject, // host -> client, followed by a disconnect
	Chat, // both ways, the host relays
	ActorTable, // host -> client, network IDs of the actors and objects in a transferred save
	Command, // client -> host, one actor action
	ActorState, // host -> client, unreliable delta snapshot
	SaveBegin, // host -> client, a save transfer starts: size, hash
	SaveChunk, // host -> client, the next piece of it
	Ready, // client -> host, the save was loaded and the actor table applied
	ResyncRequest, // client -> host, please send a fresh save
	Spawn, // host -> client, a creature appeared in the replicated area
	Despawn, // host -> client, a creature left it
	Projectile, // host -> client, a projectile was launched
	Vfx, // host -> client, a visual effect started or ended
	Sound, // host -> client, the simulation played a sound
	Text, // host -> client, feedback (message window) or overhead text
	ObjectState, // host -> client, a door, container or region changed
	Pause, // both ways, the host relays
	AreaChange, // host -> client, the party moved; a resync follows
	Ownership, // host -> client, who controls which party member
	count
};

// what a table entry or object event refers to
enum class NetObjectKind : uint8_t {
	Actor,
	Door,
	Container,
	Region,
	count
};

class GEM_EXPORT NetWriter {
	std::vector<uint8_t> buffer;

public:
	explicit NetWriter(NetMsgType type);

	void U8(uint8_t value);
	void U16(uint16_t value);
	void U32(uint32_t value);
	void I32(int32_t value);
	void U64(uint64_t value);
	// truncates to NET_MAX_STRING bytes
	void Str(const std::string& value);
	// a u32 byte count and the raw bytes
	void Bytes(const uint8_t* data, size_t size);

	const std::vector<uint8_t>& Data() const { return buffer; }
};

class GEM_EXPORT NetReader {
	const uint8_t* data;
	size_t size;
	size_t pos = 0;
	bool ok = true;

	bool Need(size_t bytes);

public:
	NetReader(const uint8_t* data, size_t size);

	// validates magic and version; false for anything we can't parse
	bool Header(NetMsgType& type);
	uint8_t U8();
	uint16_t U16();
	uint32_t U32();
	int32_t I32();
	uint64_t U64();
	std::string Str();
	// fails if the stored count is above maxSize
	std::vector<uint8_t> Bytes(size_t maxSize);
	// marks the packet bad, eg. for an out of range value
	void Fail() { ok = false; }

	bool Ok() const { return ok; }
	bool AtEnd() const { return ok && pos == size; }
};

// returns false if the packet isn't ours or is from an incompatible version
GEM_EXPORT bool NetPeekType(const uint8_t* data, size_t size, NetMsgType& type);

struct GEM_EXPORT NetHello {
	static constexpr NetMsgType Type = NetMsgType::Hello;
	std::string name;
	std::string gameType;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetWelcome {
	static constexpr NetMsgType Type = NetMsgType::Welcome;
	uint8_t slot = 0;
	std::string hostName;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetReject {
	static constexpr NetMsgType Type = NetMsgType::Reject;
	std::string reason;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetChat {
	static constexpr NetMsgType Type = NetMsgType::Chat;
	uint8_t slot = 0;
	std::string name;
	std::string text;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// a network ID is the host's global ID of the actor (or door, container, region); clients map it to their own
struct NetActorTableEntry {
	uint32_t netID = 0;
	uint8_t partySlot = 0; // 1-based, 0 for actors outside the party (and all objects)
	std::string area;
	std::string scriptName;
	int32_t x = 0;
	int32_t y = 0;
	NetObjectKind kind = NetObjectKind::Actor;
};

struct GEM_EXPORT NetActorTable {
	static constexpr NetMsgType Type = NetMsgType::ActorTable;
	uint32_t transferID = 0;
	std::vector<NetActorTableEntry> actors;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// one GameScript object of an action, eg. the target of an attack
struct NetActionObject {
	std::array<int32_t, NET_OBJECT_FIELDS> fields {};
	std::array<int32_t, NET_OBJECT_FILTERS> filters {};
	std::array<int32_t, 4> rect {}; // x, y, w, h
	std::string name;
};

// a player order: a GameScript action by name with all its parameters, so the host can rebuild it
struct GEM_EXPORT NetCommand {
	static constexpr NetMsgType Type = NetMsgType::Command;
	// how the host queues it, mirroring GameControl
	enum Flags : uint8_t {
		NONE = 0, // CommandActor: drop what the actor was doing
		STOP = 1, // Stop() first
		QUEUE = 2, // AddAction instead of CommandActor
		KEEP_PATH = 4 // CommandActor without clearing the path (waypoints)
	};
	uint32_t actorID = 0;
	uint8_t flags = NONE;
	std::string action; // the action's name in action.ids, eg. "MoveToPoint"; "UseExit" is a pseudo action
	int32_t int0 = 0;
	int32_t int1 = 0;
	int32_t int2 = 0;
	int32_t x = -1;
	int32_t y = -1;
	std::string string0;
	std::string string1;
	uint32_t actionFlags = 0;
	uint8_t objectMask = 0; // bit i set: objects[i] is present
	std::array<NetActionObject, NET_ACTION_OBJECTS> objects;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// which fields of an actor a snapshot entry carries
enum NetActorField : uint8_t {
	NAF_POS = 1,
	NAF_ORIENT = 2,
	NAF_STANCE = 4,
	NAF_HP = 8,
	NAF_STATE = 16,
	NAF_ALL = 31
};

struct NetActorStateEntry {
	uint32_t netID = 0;
	uint8_t fields = NAF_ALL;
	int32_t x = 0;
	int32_t y = 0;
	uint8_t orientation = 0;
	uint8_t stance = 0;
	int32_t hp = 0;
	int32_t maxHP = 0;
	uint32_t state = 0; // IE_STATE_ID
};

struct GEM_EXPORT NetActorState {
	static constexpr NetMsgType Type = NetMsgType::ActorState;
	enum Flags : uint8_t {
		KEYFRAME = 1, // every actor in the area, with all fields
		CHECKSUM = 2 // carries the host's state checksum
	};
	uint32_t tick = 0; // snapshot sequence, so clients can drop stale snapshots
	uint8_t flags = 0;
	std::string area;
	uint64_t checksum = 0;
	std::vector<NetActorStateEntry> actors;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetSaveBegin {
	static constexpr NetMsgType Type = NetMsgType::SaveBegin;
	uint32_t transferID = 0;
	uint32_t size = 0; // of the packed save
	uint64_t hash = 0; // NetHash of the packed save
	std::string area; // where the party is, for the log

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetSaveChunk {
	static constexpr NetMsgType Type = NetMsgType::SaveChunk;
	uint32_t transferID = 0;
	uint32_t offset = 0;
	std::vector<uint8_t> data; // at most NET_SAVE_CHUNK bytes

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetReady {
	static constexpr NetMsgType Type = NetMsgType::Ready;
	uint32_t transferID = 0;
	uint16_t mapped = 0; // how many actors of the table were matched
	std::vector<uint32_t> unmatched; // host actors we couldn't find, the host sends them as spawns

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetResyncRequest {
	static constexpr NetMsgType Type = NetMsgType::ResyncRequest;
	enum Reason : uint8_t {
		MANUAL = 0,
		DESYNC = 1 // the state checksums differed
	};
	uint8_t reason = MANUAL;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetSpawn {
	static constexpr NetMsgType Type = NetMsgType::Spawn;
	uint32_t netID = 0;
	std::string area;
	std::string scriptName; // runtime names (CreateCreature's) aren't in the CRE
	int32_t x = 0;
	int32_t y = 0;
	uint8_t orientation = 0;
	std::vector<uint8_t> creature; // the actor as a CRE file

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetDespawn {
	static constexpr NetMsgType Type = NetMsgType::Despawn;
	uint32_t netID = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetProjectile {
	static constexpr NetMsgType Type = NetMsgType::Projectile;
	enum Flags : uint8_t {
		AT_POINT = 1, // flies to (tx, ty) instead of following targetID
		FAKE_TARGET = 2 // targetID is aimed at, but not followed
	};
	std::string name; // projectile resource
	uint16_t index = 0; // projectl.ids index, 0 if unknown
	uint8_t flags = 0;
	uint32_t casterID = 0;
	uint32_t targetID = 0;
	int32_t x = 0; // source
	int32_t y = 0;
	int32_t tx = 0; // destination
	int32_t ty = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetVfx {
	static constexpr NetMsgType Type = NetMsgType::Vfx;
	enum Flags : uint8_t {
		END = 1, // stop the effect with this key; the rest of the message is unused
		TWIN = 2, // has a front and back half
		ON_AREA = 4, // placed on the map at (x, y), not on actorID
		LOOPING = 8, // runs until an END arrives
		COMPOSITE = 16 // a VEF, not a single VVC/BAM
	};
	uint32_t key = 0; // host assigned, for END
	uint8_t flags = 0;
	uint32_t actorID = 0;
	std::string resource;
	int32_t x = 0;
	int32_t y = 0;
	int32_t xOffset = 0;
	int32_t yOffset = 0;
	int32_t zOffset = 0;
	uint32_t tint = 0xffffffff; // RGBA
	uint32_t transparency = 0;
	uint32_t sequenceFlags = 0;
	uint32_t duration = 0xffffffff; // ticks left, or forever
	uint8_t orientation = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetSound {
	static constexpr NetMsgType Type = NetMsgType::Sound;
	enum Flags : uint8_t {
		SPATIAL = 1, // at (x, y, z)
		SPEECH = 2 // on the speech source (one voice at a time)
	};
	std::string resource;
	uint8_t flags = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint8_t masterVolume = 100;
	uint8_t channelVolume = 100;
	uint16_t muteDistance = 1;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetText {
	static constexpr NetMsgType Type = NetMsgType::Text;
	enum Kind : uint8_t {
		FEEDBACK = 0, // markup for the message window (combat log)
		OVERHEAD = 1 // text over an actor or object
	};
	uint8_t kind = FEEDBACK;
	uint32_t ownerID = 0; // OVERHEAD only
	uint32_t color = 0; // RGBA, OVERHEAD only
	std::string text; // UTF-8

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetObjectState {
	static constexpr NetMsgType Type = NetMsgType::ObjectState;
	uint32_t netID = 0;
	NetObjectKind kind = NetObjectKind::Door;
	uint32_t flags = 0; // the object's Flags (DOOR_OPEN, DOOR_LOCKED, CONT_LOCKED, TRAP_DEACTIVATED ...)
	uint8_t trapDetected = 0;
	uint8_t trapped = 0;
	uint16_t items = 0; // containers: item count, for the log until inventories replicate

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetPause {
	static constexpr NetMsgType Type = NetMsgType::Pause;
	uint8_t on = 0;
	uint8_t slot = 0; // who asked; the host fills it in

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetAreaChange {
	static constexpr NetMsgType Type = NetMsgType::AreaChange;
	std::string area;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetOwnershipEntry {
	uint32_t netID = 0;
	uint8_t partySlot = 0;
	uint8_t owner = 0; // player slot
};

struct GEM_EXPORT NetOwnership {
	static constexpr NetMsgType Type = NetMsgType::Ownership;
	std::vector<NetOwnershipEntry> entries;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

template<typename MSG>
std::vector<uint8_t> NetEncode(const MSG& msg)
{
	NetWriter w(MSG::Type);
	msg.Write(w);
	return w.Data();
}

template<typename MSG>
bool NetDecode(const uint8_t* data, size_t size, MSG& msg)
{
	NetReader r(data, size);
	NetMsgType type;
	if (!r.Header(type) || type != MSG::Type) return false;
	return msg.Read(r) && r.AtEnd();
}

}

#endif
