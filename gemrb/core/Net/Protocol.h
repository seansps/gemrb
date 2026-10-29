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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

constexpr uint8_t NET_MAGIC = 0xE7;
constexpr uint8_t NET_PROTOCOL_VERSION = 1;
constexpr uint16_t NET_DEFAULT_PORT = 27015;
constexpr size_t NET_HEADER_SIZE = 3;
constexpr size_t NET_MAX_STRING = 1024;
constexpr size_t NET_MAX_ENTRIES = 256;
// slot 0 is always the host, clients get 1..NET_MAX_PEERS
constexpr uint8_t NET_MAX_PEERS = 3;
constexpr uint8_t NET_HOST_SLOT = 0;

enum class NetChannel : uint8_t {
	Reliable = 0, // ordered and reliable: handshake, chat, commands, events
	State = 1, // unreliable but sequenced: periodic actor snapshots
	count
};

enum class NetMsgType : uint8_t {
	Hello = 1, // client -> host
	Welcome, // host -> client
	Reject, // host -> client, followed by a disconnect
	Chat, // both ways, the host relays
	PartyManifest, // host -> client, global IDs of the party it simulates
	Command, // client -> host, one actor action
	ActorState, // host -> client, unreliable snapshot
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
	// truncates to NET_MAX_STRING bytes
	void Str(const std::string& value);

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
	std::string Str();

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

struct NetManifestEntry {
	uint32_t globalID = 0;
	uint8_t partySlot = 0;
	std::string scriptName;
};

struct GEM_EXPORT NetPartyManifest {
	static constexpr NetMsgType Type = NetMsgType::PartyManifest;
	std::string area;
	std::vector<NetManifestEntry> actors;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetCommand {
	static constexpr NetMsgType Type = NetMsgType::Command;
	enum Flags : uint8_t {
		NONE = 0,
		APPEND = 1 // queue after the current action instead of replacing it
	};
	uint32_t actorID = 0;
	uint8_t flags = NONE;
	std::string action; // a GameScript action string, eg. "MoveToPoint([100.200])"

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetActorStateEntry {
	uint32_t globalID = 0;
	int32_t x = 0;
	int32_t y = 0;
	uint8_t orientation = 0;
	uint8_t stance = 0;
};

struct GEM_EXPORT NetActorState {
	static constexpr NetMsgType Type = NetMsgType::ActorState;
	uint32_t tick = 0; // host game ticks, so clients can drop stale snapshots
	std::string area;
	std::vector<NetActorStateEntry> actors;

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
