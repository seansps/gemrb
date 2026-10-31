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

#include "Net/Prompt.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

constexpr uint8_t NET_MAGIC = 0xE7;
constexpr uint8_t NET_PROTOCOL_VERSION = 10;
constexpr uint16_t NET_DEFAULT_PORT = 27015;
constexpr size_t NET_HEADER_SIZE = 3;
constexpr size_t NET_MAX_STRING = 1024;
constexpr size_t NET_MAX_ENTRIES = 256;
// actor stats: the IE ones and the DCC ones past them (Actor.h MAX_STATS)
constexpr size_t NET_MAX_STATS = 512;
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
// a GUI call carries at most this many integer arguments
constexpr size_t NET_GUI_ARGS = 8;
// BG style parties: six characters, each assigned to a player in the lobby
constexpr uint8_t NET_PARTY_SLOTS = 6;
// a lighter area change carries the area as an ARE file
constexpr size_t NET_MAX_AREA_SIZE = 16 * 1024 * 1024;
// chat lines a player keeps, and gets from the host on joining (lobby and game alike)
constexpr size_t NET_CHAT_HISTORY = 40;

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
	Dialog, // host -> client, a conversation's lines, replies and end
	DialogChoice, // client -> host, the conversation's owner picked a reply
	Inventory, // host -> client, everything in an actor, container or ground pile
	ActorStats, // host -> client, changed base and modified stats of an actor
	PartyState, // host -> client, party gold and reputation
	Spellbook, // host -> client, an actor's known and memorized spells
	GuiCall, // client -> host, a state changing GemRB.* call from the GUI
	StoreState, // host -> client, a store's stock; may open or close its window
	ContainerOpen, // host -> client, a party member opened a container for its owner
	Heartbeat, // both ways, unreliable: keeps the connection alive and measures the round trip
	LobbyState, // host -> client, players, who controls which party slot, permissions
	LobbyReady, // client -> host, the player's ready toggle
	AreaData, // host -> client, the party moved: the new area as an ARE, instead of a whole save
	Globals, // host -> client, global variables that changed
	ChatHistory, // host -> client, the recent chat a joining player missed
	Prompt, // host -> client, the open prompt ("ask the owning player") as everyone sees it, or its outcome
	PromptAnswer, // client -> host, the asked player's answer
	count
};

// before the host starts the game everyone is in the lobby; afterwards joins load the running game
enum class NetPhase : uint8_t {
	Lobby,
	Game,
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
	uint64_t token = 0; // from an earlier welcome in this session, so a returning player gets its slot back

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetWelcome {
	static constexpr NetMsgType Type = NetMsgType::Welcome;
	enum Flags : uint8_t {
		REJOINED = 1, // the token was known: same slot and characters as before
		IN_GAME = 2 // the game runs already, a save follows; otherwise wait in the lobby
	};
	uint8_t slot = 0;
	std::string hostName;
	uint64_t token = 0; // the client's identity for this host session
	uint8_t flags = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// Its layout is fixed for good (since v1): the header, then the reason as a string, nothing
// else. So a host can turn away a client of any other version in words that client reads:
// it stamps the client's version into the header (NetEncodeReject), and from v7 on clients
// read a Reject of any version (NetDecodeAnyReject).
struct GEM_EXPORT NetReject {
	static constexpr NetMsgType Type = NetMsgType::Reject;
	std::string reason;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// a Reject for a peer that speaks another protocol version: its header carries that version
GEM_EXPORT std::vector<uint8_t> NetEncodeReject(const NetReject& reject, uint8_t version);
// a Reject from a host of any version; version is the one in its header
GEM_EXPORT bool NetDecodeAnyReject(const uint8_t* data, size_t size, NetReject& reject, uint8_t& version);
// a packet with our magic but another version: which version and message type it says it is
GEM_EXPORT bool NetPeekForeign(const uint8_t* data, size_t size, uint8_t& version, uint8_t& type);

// a chat line; clients send text (and a whisper's target), the host fills in who said it
struct GEM_EXPORT NetChat {
	static constexpr NetMsgType Type = NetMsgType::Chat;
	enum Flags : uint8_t {
		WHISPER = 1, // to the target only; the host routes it and relays it to nobody else
		SYSTEM = 2 // the host says so: someone joined, left or rejoined (slot and name are theirs)
	};
	uint8_t slot = 0;
	uint8_t flags = 0;
	uint8_t target = 0; // whispers
	std::string name;
	std::string targetName; // whispers, filled in by the host
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

// a conversation, as the host's DialogHandler runs it
struct NetDialogOption {
	uint16_t index = 0; // transition index, what the owner sends back
	std::string text;
};

struct GEM_EXPORT NetDialog {
	static constexpr NetMsgType Type = NetMsgType::Dialog;
	enum Kind : uint8_t {
		LINE = 0, // someone said text (speakerID)
		STATE, // the replies of a new node; the owner picks one
		END, // the conversation is over
		count
	};
	enum Flags : uint8_t {
		FREEZE = 1, // the world is frozen meanwhile (DF_FREEZE_SCRIPTS)
		CONTINUE = 2, // no replies, just a continue button (continueIndex)
		END_BUTTON = 4, // no replies, just an end button (continueIndex)
		PARTY = 8 // LINE: a party member's reply, not the NPC's text
	};
	uint8_t kind = LINE;
	uint32_t seq = 0; // STATE: node counter, so late choices are ignored
	uint8_t owner = 0; // player slot that picks the replies
	uint8_t flags = 0;
	uint32_t speakerID = 0;
	uint32_t targetID = 0;
	uint16_t continueIndex = 0;
	std::string text; // LINE: UTF-8
	std::vector<NetDialogOption> options; // STATE

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetDialogChoice {
	static constexpr NetMsgType Type = NetMsgType::DialogChoice;
	uint32_t seq = 0; // the STATE this answers
	uint16_t index = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// one stack in an inventory
struct NetItem {
	uint16_t slot = 0;
	std::string resref;
	std::array<uint16_t, 3> usages {};
	uint32_t flags = 0;
	uint16_t expired = 0;

	bool operator==(const NetItem& other) const
	{
		return slot == other.slot && resref == other.resref && usages == other.usages && flags == other.flags && expired == other.expired;
	}
};

// full contents, sent whenever anything in it changed; ground piles have no network
// ID (they come and go) and are found by area and position instead, like Map::GetPile
struct GEM_EXPORT NetInventory {
	static constexpr NetMsgType Type = NetMsgType::Inventory;
	uint32_t netID = 0; // 0 for a pile
	NetObjectKind kind = NetObjectKind::Actor; // Actor or Container
	std::string area; // piles
	int32_t x = 0;
	int32_t y = 0;
	int16_t equipped = 0; // actors: Inventory::Equipped and its header
	uint16_t equippedHeader = 0;
	std::vector<NetItem> items;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetStatEntry {
	uint16_t stat = 0;
	uint32_t base = 0;
	uint32_t modified = 0;
};

struct GEM_EXPORT NetActorStats {
	static constexpr NetMsgType Type = NetMsgType::ActorStats;
	uint32_t netID = 0;
	std::vector<NetStatEntry> stats;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetPartyState {
	static constexpr NetMsgType Type = NetMsgType::PartyState;
	uint32_t gold = 0;
	uint32_t reputation = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetMemorizedSpell {
	std::string resref;
	uint32_t flags = 0; // 1 = still castable
};

// one type and level of a spellbook (CRESpellMemorization)
struct NetSpellPage {
	uint8_t type = 0;
	uint8_t level = 0;
	uint16_t slots = 0;
	uint16_t slotsWithBonus = 0;
	std::vector<std::string> known;
	std::vector<NetMemorizedSpell> memorized;
};

struct GEM_EXPORT NetSpellbook {
	static constexpr NetMsgType Type = NetMsgType::Spellbook;
	uint32_t netID = 0;
	std::vector<NetSpellPage> pages;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// the GemRB.* GUI functions a client may ask the host to run for it
enum class NetGuiOp : uint8_t {
	DragItem = 1, // actor, slot, count, type (1: from the pile at its feet)
	DropDraggedItem, // actor, slot
	ChangeContainerItem, // actor, slot, action (actor 0: the opened container)
	LeaveContainer,
	ChangeStoreItem, // actor, slot, action
	SetPurchasedAmount, // index, amount, type
	LeaveStore,
	AddPartyGold, // amount (GameSetPartyGold, as a difference)
	SetEquippedQuickSlot, // actor, slot, ability
	UseItem, // actor, slot, header, target (items used without a target)
	MemorizeSpell, // actor, type, level, index, enabled
	UnmemorizeSpell, // actor, type, level, index, flags
	ChangeItemFlag, // actor, slot, flags, mode
	RestParty, // checks, dream, hp
	LeaveParty, // actor, dialog
	SwapPCs, // party slot, party slot
	SetFormation, // formation, which
	SetModalState, // actor, state (text: the modal spell)
	SetupQuickSlot, // actor, slot, action
	SetupQuickSpell, // actor, slot, index, type
	SetupQuickItemSlot, // actor, slot, inventory slot, header, translate
	SetJournalEntry, // strref (-1: text), section, chapter, feedback (text: a new entry)
	SetMapnote, // x, y, color (text: the note, empty removes it)
	DCCSetLuckPolicy, // actor, policy, threshold, flags (v9)
	DCCSetDeeds, // actor, signature deed, one-off deed (v9)
	DCCSetCombat, // actor, combat options, battle rage points (v10)
	DCCRecoverBody, // dead party slot, ally party slot (v10; host only)
	count
};

struct GEM_EXPORT NetGuiCall {
	static constexpr NetMsgType Type = NetMsgType::GuiCall;
	NetGuiOp op = NetGuiOp::DragItem;
	uint8_t selected = 0; // the client's GameGetSelectedPCSingle
	std::vector<int32_t> args; // actors as network IDs (or party slots below 1000)
	std::string text; // UTF-8, for the calls that take one

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetStoreItem {
	std::string resref;
	std::array<uint16_t, 3> usages {};
	uint32_t flags = 0;
	uint32_t amount = 0;
	int32_t infinite = 0;
	uint16_t purchased = 0;
};

struct GEM_EXPORT NetStoreState {
	static constexpr NetMsgType Type = NetMsgType::StoreState;
	enum Flags : uint8_t {
		OPEN = 1, // open the store window for actorID
		CLOSE = 2 // the host closed it (the owner left the area or the game)
	};
	uint8_t flags = 0;
	std::string store;
	uint32_t ownerID = 0; // the merchant
	uint32_t actorID = 0; // the party member shopping
	std::vector<NetStoreItem> items;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetContainerOpen {
	static constexpr NetMsgType Type = NetMsgType::ContainerOpen;
	uint32_t actorID = 0;
	uint32_t containerID = 0; // 0 for a pile at (x, y)
	int32_t x = 0;
	int32_t y = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetHeartbeat {
	static constexpr NetMsgType Type = NetMsgType::Heartbeat;
	uint32_t seq = 0;
	uint32_t time = 0; // sender's clock, ms
	uint32_t echo = 0; // the time of the last heartbeat we got from the other side, for the round trip
	uint16_t rtt = 0; // what the sender measured, ms (the host tells each client its own)

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetLobbyPlayer {
	enum Flags : uint8_t {
		CONNECTED = 1, // remembered players who left keep their slot (and characters) for a rejoin
		READY = 2
	};
	uint8_t slot = 0;
	uint8_t flags = 0;
	uint16_t ping = 0; // ms
	std::string name;
	uint32_t allowed = 0; // bit per NetPermission
};

struct NetLobbySlot {
	uint8_t partySlot = 0; // 1-based
	uint8_t owner = 0; // the player it is assigned to
	uint8_t controller = 0; // who controls it now: the owner, or the host while the owner is away
	std::string name; // empty until the game runs
};

struct GEM_EXPORT NetLobbyState {
	static constexpr NetMsgType Type = NetMsgType::LobbyState;
	NetPhase phase = NetPhase::Lobby;
	std::vector<NetLobbyPlayer> players;
	std::vector<NetLobbySlot> party;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetLobbyReady {
	static constexpr NetMsgType Type = NetMsgType::LobbyReady;
	uint8_t ready = 0;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetAreaPartyEntry {
	uint32_t netID = 0;
	uint8_t partySlot = 0;
	int32_t x = 0;
	int32_t y = 0;
	uint8_t orientation = 0;
};

// the party moved to another area: the host's copy of it and where everyone stands; the
// actor table with the same transfer ID follows, then the client reports Ready
struct GEM_EXPORT NetAreaData {
	static constexpr NetMsgType Type = NetMsgType::AreaData;
	uint32_t transferID = 0;
	std::string area;
	std::vector<NetAreaPartyEntry> party;
	std::vector<uint8_t> are; // the ARE file

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct NetVariable {
	std::string name;
	int32_t value = 0;

	bool operator==(const NetVariable& other) const { return name == other.name && value == other.value; }
};

struct GEM_EXPORT NetGlobals {
	static constexpr NetMsgType Type = NetMsgType::Globals;
	std::vector<NetVariable> vars; // at most NET_MAX_ACTORS

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// what a joining player missed: the last NET_CHAT_HISTORY lines it may see, oldest first;
// replaces what it had (a rejoin)
struct GEM_EXPORT NetChatHistory {
	static constexpr NetMsgType Type = NetMsgType::ChatHistory;
	std::vector<NetChat> lines;

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

// someone a prompt asks, as everyone sees it
struct NetPromptVoter {
	uint8_t slot = 0; // asked
	uint8_t by = 0; // answers now (the host while an owner is away)
	uint8_t state = 0; // PromptVoter::State
	uint8_t away = 0;
	std::string name; // the asked player's
	std::string byName;
};

// the open prompt, the same for everyone: the asked player gets the window to answer it,
// the others "Waiting for X to decide"; CLOSED carries how it ended, for everyone's message window
struct GEM_EXPORT NetPrompt {
	static constexpr NetMsgType Type = NetMsgType::Prompt;
	enum State : uint8_t {
		OPEN,
		CLOSED
	};
	uint32_t id = 0;
	uint8_t state = OPEN;
	uint32_t remaining = 0; // ms, as the host sent it
	PromptQuestion question; // actorID as a network ID
	std::vector<NetPromptVoter> voters; // at most PROMPT_MAX_VOTERS
	uint8_t outcome = 0; // CLOSED: PromptOutcome
	std::vector<int32_t> values; // CLOSED: the answer
	std::string outcomeText; // CLOSED: "client-one decided: Yes."

	void Write(NetWriter& w) const;
	bool Read(NetReader& r);
};

struct GEM_EXPORT NetPromptAnswer {
	static constexpr NetMsgType Type = NetMsgType::PromptAnswer;
	uint32_t id = 0;
	std::vector<int32_t> values; // at most PROMPT_MAX_FIELDS

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
