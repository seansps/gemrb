// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Replication.h
 * Live replication pieces with no engine or ENet dependency.
 *
 * The host turns the actors of the replicated area into delta snapshots
 * (NetDeltaTracker) and a cheap checksum of the slow-changing state; the
 * client maps network IDs to its own (NetIDMap), smooths positions
 * between snapshots (NetLerp) and compares checksums to detect a desync.
 */

#ifndef NET_REPLICATION_H
#define NET_REPLICATION_H

#include "exports.h"

#include "Net/Protocol.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GemRB {

// network ID (the host's global ID) <-> the client's own global ID
class GEM_EXPORT NetIDMap {
	std::map<uint32_t, uint32_t> toLocal;
	std::map<uint32_t, uint32_t> toNet;

public:
	// replaces any older pairing of either ID
	void Add(uint32_t netID, uint32_t localID);
	void RemoveNet(uint32_t netID);
	void RemoveLocal(uint32_t localID);
	// 0 if unknown (global IDs start at 10000, so 0 is never valid)
	uint32_t Local(uint32_t netID) const;
	uint32_t Net(uint32_t localID) const;
	size_t Size() const { return toLocal.size(); }
	void Clear();
	const std::map<uint32_t, uint32_t>& All() const { return toLocal; }
};

/**
 * Host side of delta snapshots. Build gets every actor of the area (all fields set) and
 * returns what to send: on a keyframe everyone with all fields, otherwise only the fields
 * that changed in the last `redundancy` snapshots, so a lost packet or two heals itself
 * before the next keyframe does. Actors no longer listed are forgotten.
 */
class GEM_EXPORT NetDeltaTracker {
	struct Track {
		NetActorStateEntry last;
		std::array<uint32_t, 5> changedAt {}; // per NetActorField bit
	};
	std::map<uint32_t, Track> tracks;
	uint32_t redundancy;

public:
	explicit NetDeltaTracker(uint32_t redundancy = 3)
		: redundancy(redundancy) {}

	std::vector<NetActorStateEntry> Build(uint32_t seq, bool keyframe, const std::vector<NetActorStateEntry>& current);
	void Reset() { tracks.clear(); }
	size_t Size() const { return tracks.size(); }
};

// one actor or object in the state checksum; only state that doesn't need interpolating
struct NetCheckEntry {
	uint32_t netID = 0; // 0 for local things the client couldn't map
	NetObjectKind kind = NetObjectKind::Actor;
	int32_t a = 0; // actors: hit points; objects: flags
	uint32_t b = 0; // actors: max hit points; objects: trap detected | trapped << 8
	uint32_t c = 0; // actors: state flags
};

// order independent; both sides list the same things the same way
GEM_EXPORT uint64_t NetStateChecksum(const std::string& area, std::vector<NetCheckEntry> entries);

// client side smoothing of a position between two snapshots
struct NetLerp {
	int32_t fromX = 0;
	int32_t fromY = 0;
	int32_t toX = 0;
	int32_t toY = 0;
	uint64_t start = 0; // ms
	uint32_t duration = 0; // ms
};

// returns true once the target is reached
GEM_EXPORT bool NetInterpolate(const NetLerp& lerp, uint64_t now, int32_t& x, int32_t& y);

// default control: party member n goes to the (n-1)th player round robin, so a lone
// host keeps everyone and two players alternate; players lists the slots in play (host first)
GEM_EXPORT uint8_t NetDefaultOwner(uint8_t partySlot, const std::vector<uint8_t>& players);

// a player the host knows of in this session; one who left keeps its slot for a rejoin
struct NetPlayer {
	uint8_t slot = 0;
	std::string name;
	uint64_t token = 0; // what the client shows to get its slot back
	bool connected = false;
	bool ready = false;
};

/**
 * The host's players: itself in slot 0 and clients in 1..maxSlot. A client is known by
 * the token it got in its welcome, so when it comes back (after a drop or a timeout) it
 * gets the same slot and with it its characters. Players who left stay remembered until
 * their slot is needed for someone new.
 */
class GEM_EXPORT NetRoster {
	std::vector<NetPlayer> players; // by slot
	uint8_t maxSlot;

public:
	explicit NetRoster(uint8_t maxSlot = NET_MAX_PEERS);

	void SetHost(const std::string& name);
	// a hello: a known token gets its old slot back (rejoined), anyone else a free slot (and
	// newToken), or the slot of the player who left first; 0 when everyone is connected
	uint8_t Join(const std::string& name, uint64_t token, uint64_t newToken, bool& rejoined);
	// the player left (or timed out); not ready any more
	void Leave(uint8_t slot);
	void SetReady(uint8_t slot, bool ready);
	void ClearReady();

	const NetPlayer* Find(uint8_t slot) const;
	bool Connected(uint8_t slot) const;
	// everyone remembered, connected or not, host first
	std::vector<uint8_t> Slots() const;
	// every connected player (the host included) is ready
	bool AllReady() const;
	const std::vector<NetPlayer>& All() const { return players; }
};

// who a party slot belongs to: the player the host assigned it to, else the round robin
// default over the remembered players; and who controls it now: the owner, or the host
// while the owner is away (BG: a leaving player's characters go to the host)
GEM_EXPORT uint8_t NetSlotOwner(uint8_t partySlot, const std::map<uint8_t, uint8_t>& assigned, const NetRoster& roster);
GEM_EXPORT uint8_t NetSlotController(uint8_t partySlot, const std::map<uint8_t, uint8_t>& assigned, const NetRoster& roster);

// what the host lets a player do, modelled on the BG/IWD multiplayer permissions screen;
// Control is the per-actor ownership rule, the rest are per player
enum class NetPermission : uint8_t {
	Control, // order a party member (it must be the player's own)
	Purchase, // buy, sell and spend party gold
	AreaTransition,
	Dialog, // start conversations and pick replies
	CharacterRecords, // change inventories, spellbooks and records
	Pause,
	Leader,
	count
};

GEM_EXPORT const char* NetPermissionName(NetPermission perm);

// per player slot; the host (slot 0) may always do everything
class GEM_EXPORT NetPermissions {
	std::map<uint8_t, uint32_t> denied; // slot -> bit per NetPermission

public:
	void Set(uint8_t slot, NetPermission perm, bool allowed);
	// for Control, actorOwner is the slot that owns the actor in question
	bool Allowed(uint8_t slot, NetPermission perm, uint8_t actorOwner = 0) const;
	// the per player permissions as bits, as the lobby sends them (Control isn't one)
	uint32_t Mask(uint8_t slot) const;
	void SetMask(uint8_t slot, uint32_t allowed);
};

// the permission named as NetPermissionName says it (or with _ for spaces); count if unknown
GEM_EXPORT NetPermission NetPermissionByName(const std::string& name);

// who answers a conversation: the owner of the party member in it (the speaker,
// else the target), so the one who started it, or the PC an NPC talks to; -1 = no PC
GEM_EXPORT uint8_t NetDialogOwner(int speakerOwner, int targetOwner, uint8_t fallback);

// the stats that changed between two copies of an actor's base and modified stats,
// leaving out the ones snapshots carry (skip)
GEM_EXPORT std::vector<NetStatEntry> NetStatsDiff(const std::vector<NetStatEntry>& before, const std::vector<NetStatEntry>& now, const std::vector<uint16_t>& skip);

// what each GUI call refers to and needs
struct NetGuiOpInfo {
	int actorArg = -1; // index of the actor argument, -1 for none
	size_t minArgs = 0;
	NetPermission permission = NetPermission::Control;
	const char* name = "?"; // the GemRB function
	bool hostOnly = false; // simulation: the client doesn't run it locally, the result comes back
	bool text = false; // takes the call's text as its last argument
};

GEM_EXPORT NetGuiOpInfo NetGuiInfo(NetGuiOp op);

// how a player follows the party to another area: the area alone (lighter) when it is in
// sync and only the area changed, else a whole save (party changes, a split party,
// areas that aren't saved, players still loading)
enum class NetAreaFollow : uint8_t {
	Area,
	Save
};

struct NetAreaChangeInfo {
	bool lightEnabled = true; // NetLightAreas
	bool partyChanged = false; // members joined, left or changed places
	bool oneArea = true; // the whole party is in the new area
	bool noSave = false; // the area is AF_NOSAVE, the client would load a fresh one
	bool peerLive = true; // the player is in sync, not loading or about to
};

GEM_EXPORT NetAreaFollow NetAreaChangeKind(const NetAreaChangeInfo& info);
// the actor table that goes with the area: its actors and objects plus the party
GEM_EXPORT std::vector<NetActorTableEntry> NetAreaTable(const std::vector<NetActorTableEntry>& all, const std::string& area);

// the variables that are new or changed since before
GEM_EXPORT std::vector<NetVariable> NetVariablesDiff(const std::map<std::string, int32_t>& before, const std::map<std::string, int32_t>& now);

// change detection for state sent as a whole (inventories, spellbooks, stores)
GEM_EXPORT uint64_t NetMessageHash(const NetInventory& msg);
GEM_EXPORT uint64_t NetMessageHash(const NetSpellbook& msg);
GEM_EXPORT uint64_t NetMessageHash(const NetStoreState& msg);

}

#endif
