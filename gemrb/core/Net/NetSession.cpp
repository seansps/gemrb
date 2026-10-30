// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/NetSession.h"

#include "config.h"
#include "ie_stats.h"

#include "ActorMgr.h"
#include "DisplayMessage.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "InterfaceConfig.h"
#include "Map.h"
#include "PluginMgr.h"
#include "Projectile.h"
#include "ProjectileServer.h"
#include "SaveGame.h"
#include "SaveGameIterator.h"
#include "ScriptedAnimation.h"
#include "TileMap.h"
#include "VEFObject.h"

#include "Audio/Playback.h"
#include "Audio/PlaybackConfig.h"
#include "GUI/GameControl.h"
#include "GameScript/GSUtils.h"
#include "GameScript/GameScript.h"
#include "Logging/Logging.h"
#include "Net/Protocol.h"
#include "Net/Replication.h"
#include "Net/Resync.h"
#include "Scriptable/Actor.h"
#include "Scriptable/Container.h"
#include "Scriptable/Door.h"
#include "Scriptable/InfoPoint.h"
#include "Streams/FileStream.h"
#include "Streams/MemoryStream.h"
#include "Strings/StringConversion.h"
#include "System/VFS.h"

#ifdef HAVE_ENET
	#include <enet/enet.h>
#endif

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace GemRB {

#ifdef HAVE_ENET

static_assert(NET_OBJECT_FIELDS == MAX_OBJECT_FIELDS && NET_OBJECT_FILTERS == MAX_NESTING, "GameScript objects changed shape");

// how often (in game loop ticks) the host sends actor snapshots; ticks run at ~15 Hz
static constexpr unsigned int SNAPSHOT_INTERVAL = 1;
// every this many snapshots: everyone in full, with the state checksum
static constexpr uint32_t KEYFRAME_INTERVAL = 15;
// a changed field rides along this many snapshots, so single lost packets heal before the next keyframe
static constexpr uint32_t DELTA_REDUNDANCY = 3;
// client: how long to wait between connection attempts
static constexpr tick_t RECONNECT_DELAY = 2000;
// test hooks wait this many ticks after syncing (or the host after starting), so both sides are settled
static constexpr unsigned int TEST_HOOK_DELAY = 30;
// host: save chunks sent per peer and tick, so a transfer doesn't flood the send queue
static constexpr unsigned int CHUNKS_PER_TICK = 8;
// NetTestSlowTransfer: one tiny chunk per tick, so a transfer can be interrupted by hand
static constexpr size_t SLOW_CHUNK = 64;
// client: how far an actor may have been nudged by loading and still match its host counterpart
static constexpr int MATCH_DISTANCE = 128;
// keep the per-actor join logs readable in big areas
static constexpr size_t MAX_LOGGED_ACTORS = 40;
// both sides log every actor of every snapshot with a multiple of this sequence number, for comparing logs
static constexpr uint32_t SYNC_CHECK_INTERVAL = 75;
// host: once the party changed areas, wait this many ticks for stragglers before resyncing
static constexpr int AREA_SETTLE_TICKS = 8;
// host: events held back for a peer that is loading a save; beyond that it gets a fresh save anyway
static constexpr size_t MAX_PENDING_EVENTS = 4096;
// client: jumps longer than this aren't smoothed
static constexpr int SNAP_DISTANCE = 128;
// client: consecutive checksum mismatches before asking for a resync, and the pause between such requests
static constexpr int DESYNC_MISMATCHES = 2;
static constexpr tick_t DESYNC_COOLDOWN = 20000;

// one written save, ready to stream to every peer that asked for it in this tick
struct NetSnapshot {
	std::vector<uint8_t> packed;
	NetActorTable table;
	std::string area;
};

struct NetPeerInfo {
	ENetPeer* peer = nullptr;
	uint8_t slot = 0;
	std::string name;
	bool welcomed = false;
	// joined or asked for a resync: gets a save at the next safe point
	bool wantsSave = false;
	std::shared_ptr<NetSaveSender> transfer;
	NetActorTable table; // goes out after the last chunk
	// loaded the latest save and reported Ready: gets snapshots and every event
	bool live = false;
	// the save it is loading; state changing events since its snapshot wait in pending
	uint32_t awaitedTransfer = 0;
	std::vector<std::vector<uint8_t>> pending;
};

// a visual effect the host has told the clients about
struct HostVfx {
	uint32_t key = 0;
	ResRef name;
	ieDword actorID = 0;
};

// and the client's copy of it
struct ClientVfx {
	ieDword actorID = 0;
	const ScriptedAnimation* vvc = nullptr;
};

// strips [tags] from message window markup, for the log
static std::string PlainText(const std::string& markup)
{
	std::string text;
	bool inTag = false;
	for (char ch : markup) {
		if (ch == '[') {
			inTag = true;
		} else if (ch == ']') {
			inTag = false;
		} else if (!inTag) {
			text += ch;
		}
	}
	return text;
}

static std::string Utf8(const String& text)
{
	return RecodedStringFromWideStringBytes(text.data(), text.size() * sizeof(char16_t), "UTF-8");
}

static const char* KindName(NetObjectKind kind)
{
	switch (kind) {
		case NetObjectKind::Actor:
			return "actor";
		case NetObjectKind::Door:
			return "door";
		case NetObjectKind::Container:
			return "container";
		case NetObjectKind::Region:
			return "region";
		default:
			return "?";
	}
}

static Map* FindLoadedMap(const std::string& area)
{
	const Game* game = core->GetGame();
	if (!game) return nullptr;
	int idx = game->FindMap(ResRef(area.c_str()));
	return idx < 0 ? nullptr : game->GetMap(unsigned(idx));
}

// any actor, door, container or region in a loaded area
static Scriptable* FindScriptable(ieDword globalID)
{
	const Game* game = core->GetGame();
	if (!game || !globalID) return nullptr;
	for (size_t idx = 0; idx < game->GetLoadedMapCount(); ++idx) {
		Scriptable* scr = game->GetMap(unsigned(idx))->GetScriptableByGlobalID(globalID);
		if (scr) return scr;
	}
	return nullptr;
}

// action names travel in lower case, the way the IDS importer stores them
static std::string LowerCase(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return char(tolower(ch)); });
	return text;
}

// the action's name as action.ids spells it, without the signature
static std::string ActionName(unsigned short actionID)
{
	std::string signature;
	if (overrideActionsTable && overrideActionsTable->FindValue(actionID) >= 0) {
		signature = overrideActionsTable->GetValue(actionID);
	} else if (actionsTable) {
		signature = actionsTable->GetValue(actionID);
	}
	return LowerCase(signature.substr(0, signature.find('(')));
}

static int ActionID(const std::string& name)
{
	if (name.empty() || name.find('(') != std::string::npos) return -1;
	std::string key = name + "(";
	StringView view(key);
	if (overrideActionsTable) {
		int i = overrideActionsTable->FindString(view);
		if (i >= 0) return overrideActionsTable->GetValueIndex(i);
	}
	if (!actionsTable) return -1;
	int i = actionsTable->FindString(view);
	return i < 0 ? -1 : actionsTable->GetValueIndex(i);
}

// the actor as a CRE file, the way areas embed them in saves
static std::vector<uint8_t> WriteCreature(const Actor* actor)
{
	auto am = GetImporter<ActorMgr>(IE_CRE_CLASS_ID);
	if (!am) return {};
	int size = am->GetStoredFileSize(actor);
	if (size <= 0 || size_t(size) > NET_MAX_CREATURE_SIZE) return {};
	void* buffer = malloc(size);
	MemoryStream stream("netspawn.cre", buffer, size); // owns the buffer
	if (am->PutActor(&stream, actor) < 0) return {};
	const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
	return std::vector<uint8_t>(bytes, bytes + size);
}

static Actor* ReadCreature(const std::vector<uint8_t>& data)
{
	if (data.empty()) return nullptr;
	void* buffer = malloc(data.size());
	memcpy(buffer, data.data(), data.size());
	auto am = GetImporter<ActorMgr>(IE_CRE_CLASS_ID, new MemoryStream("netspawn.cre", buffer, data.size()));
	return am ? am->GetActor(0) : nullptr;
}

// restart a stance's animation, as the host's own HandleActorStance does once one ends
static void RestartStance(Actor* actor)
{
	CharAnimations* anims = actor->GetAnims();
	if (!anims) return;
	const auto* parts = anims->GetAnimation(actor->GetStance(), actor->GetOrientation());
	if (!parts) return;
	for (const auto& anim : *parts) {
		if (!anim) continue;
		anim->endReached = false;
		anim->SetFrame(0);
	}
}

struct NetSession::Impl {
	enum class ClientState : uint8_t {
		Connecting, // no welcome yet
		Joining, // welcomed, waiting for or receiving the save
		Loading, // the save is in, the engine loads it
		InSync // loaded and mapped, snapshots and commands flow
	};

	Mode mode;
	std::string name;
	std::string remoteHost;
	uint16_t port;
	ENetHost* host = nullptr;
	// scratch space for save transfers, next to the cache dir (which loading wipes)
	path_t transferDir;

	// host side
	std::vector<NetPeerInfo> peers;
	uint32_t snapshotSeq = 0;
	uint32_t nextTransferID = 1;
	unsigned int hostGameTicks = 0;
	// between PreTick and PostTick: what the simulation shows and plays is replicated
	bool capturing = false;
	// the area everyone sees (the first PC's) and what the clients know about it
	std::string replArea;
	const Map* replMap = nullptr;
	NetDeltaTracker delta { DELTA_REDUNDANCY };
	std::set<ieDword> knownActors;
	std::map<ieDword, NetObjectState> objectStates;
	std::map<const ScriptedAnimation*, HostVfx> hostVfx;
	uint32_t nextVfxKey = 1;
	// party member -> area, to notice area changes
	std::map<ieDword, std::string> partyAreas;
	int areaSettle = 0;
	// who controls which party member
	std::map<uint8_t, uint8_t> ownerOverride; // party slot -> player slot
	NetOwnership owners;
	uint8_t pauseRequester = NET_HOST_SLOT;
	std::string lastSound;
	unsigned int lastSoundTick = 0;

	// client side
	ENetPeer* server = nullptr;
	tick_t nextConnectAttempt = 0;
	bool stopped = false; // gave up (rejected or broken transfer), no more reconnects
	ClientState state = ClientState::Connecting;
	uint8_t slot = NET_HOST_SLOT;
	uint32_t lastSnapshot = 0;
	NetSaveReceiver receiver;
	uint32_t loadingTransfer = 0; // the save being loaded, its actor table must match
	bool haveTable = false;
	NetActorTable table;
	NetIDMap ids;
	// position smoothing, by local ID
	std::map<ieDword, NetLerp> lerps;
	tick_t lastStateAt = 0;
	uint32_t stateInterval = 1000 / 15;
	std::map<uint32_t, uint8_t> myOwners; // net ID -> player slot
	std::map<uint32_t, ClientVfx> clientVfx;
	bool applyingPause = false;
	int mismatches = 0;
	tick_t lastDesyncResync = 0;
	unsigned int checksumsOk = 0;
	// smoothing statistics for the log
	unsigned int smoothedFrames = 0;
	unsigned int smoothedMoves = 0;

	// test hooks
	std::string testChat;
	std::string testMove;
	std::string testVar;
	std::string testOrders;
	int testResync = 0;
	bool slowTransfer = false;
	unsigned int syncedTicks = 0;
	unsigned int testTicks = 0; // never reset by resyncs, so test orders run once
	ieDword testSpawned = 0;

	unsigned int ticks = 0;
	std::map<uint32_t, Point> lastLoggedPos;

	Impl(Mode mode, const CoreSettings& config)
		: mode(mode), name(config.NetName), remoteHost(config.NetHost), port(uint16_t(config.NetPort)),
		  testChat(config.NetTestChat), testMove(config.NetTestMove), testVar(config.NetTestVar),
		  testOrders(config.NetTestOrders), testResync(config.NetTestResync), slowTransfer(config.NetTestSlowTransfer != 0)
	{
		path_t cache = config.CachePath;
		while (cache.size() > 1 && cache.back() == PathDelimiter) {
			cache.pop_back();
		}
		transferDir = cache + "-net";
	}

	~Impl()
	{
		if (!host) return;
		if (server) {
			enet_peer_disconnect_now(server, 0);
		}
		for (const auto& info : peers) {
			enet_peer_disconnect_now(info.peer, 0);
		}
		enet_host_flush(host);
		enet_host_destroy(host);
		enet_deinitialize();
	}

	bool Start()
	{
		if (enet_initialize() != 0) {
			Log(ERROR, "Net", "Failed to initialize ENet!");
			return false;
		}

		if (mode == Mode::Host) {
			ENetAddress address;
			address.host = ENET_HOST_ANY;
			address.port = port;
			host = enet_host_create(&address, NET_MAX_PEERS, size_t(NetChannel::count), 0, 0);
			if (!host) {
				Log(ERROR, "Net", "Failed to listen on UDP port {}!", port);
				enet_deinitialize();
				return false;
			}
			Log(MESSAGE, "Net", "Hosting as '{}' on UDP port {} (protocol v{}).", name, port, NET_PROTOCOL_VERSION);
		} else {
			host = enet_host_create(nullptr, 1, size_t(NetChannel::count), 0, 0);
			if (!host) {
				Log(ERROR, "Net", "Failed to create the client socket!");
				enet_deinitialize();
				return false;
			}
			Connect();
		}
		return true;
	}

	void Connect()
	{
		ENetAddress address;
		if (enet_address_set_host(&address, remoteHost.c_str()) != 0) {
			Log(ERROR, "Net", "Unable to resolve host '{}'.", remoteHost);
			nextConnectAttempt = GetMilliseconds() + RECONNECT_DELAY;
			return;
		}
		address.port = port;
		server = enet_host_connect(host, &address, size_t(NetChannel::count), NET_PROTOCOL_VERSION);
		Log(MESSAGE, "Net", "Connecting to {}:{} as '{}'...", remoteHost, port, name);
	}

	static void SendRaw(ENetPeer* peer, const std::vector<uint8_t>& data, NetChannel channel)
	{
		enet_uint32 flags = channel == NetChannel::State ? 0 : ENET_PACKET_FLAG_RELIABLE;
		ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
		enet_peer_send(peer, enet_uint8(channel), packet);
	}

	template<typename MSG>
	static void Send(ENetPeer* peer, const MSG& msg, NetChannel channel = NetChannel::Reliable)
	{
		SendRaw(peer, NetEncode(msg), channel);
	}

	template<typename MSG>
	void Broadcast(const MSG& msg, NetChannel channel = NetChannel::Reliable, const ENetPeer* except = nullptr)
	{
		for (const auto& info : peers) {
			if (info.welcomed && info.peer != except) {
				Send(info.peer, msg, channel);
			}
		}
	}

	// world events go to every peer in sync; peers loading a save get the state
	// changing ones (keep) once they report Ready, in order, and nothing else
	template<typename MSG>
	void SendEvent(const MSG& msg, bool keep)
	{
		std::vector<uint8_t> data;
		for (auto& info : peers) {
			if (!info.welcomed) continue;
			if (!info.live && (!keep || !info.awaitedTransfer)) continue;
			if (data.empty()) {
				data = NetEncode(msg);
			}
			if (info.live) {
				SendRaw(info.peer, data, NetChannel::Reliable);
			} else if (info.pending.size() < MAX_PENDING_EVENTS) {
				info.pending.push_back(data);
			}
		}
	}

	bool AnyPeer() const
	{
		for (const auto& info : peers) {
			if (info.welcomed && (info.live || info.awaitedTransfer)) return true;
		}
		return false;
	}

	// host, inside the simulation, with someone to tell
	bool Capturing() const
	{
		return mode == Mode::Host && capturing && AnyPeer();
	}

	NetPeerInfo* FindPeer(const ENetPeer* peer)
	{
		for (auto& info : peers) {
			if (info.peer == peer) return &info;
		}
		return nullptr;
	}

	static std::string PeerAddress(const ENetPeer* peer)
	{
		char ip[64] = "?";
		enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
		return fmt::format("{}:{}", ip, peer->address.port);
	}

	static void ShowChat(const NetChat& chat)
	{
		Log(MESSAGE, "Net", "Chat <{}#{}>: {}", chat.name, chat.slot, chat.text);
		if (core->GetGame()) {
			displaymsg->DisplayMarkupString(fmt::format(u"[p][color=80C0FFFF]{}:[/color] {}[/p]", StringFromUtf8(chat.name.c_str()), StringFromUtf8(chat.text.c_str())));
		}
	}

	void SendChat(const std::string& text)
	{
		NetChat chat;
		chat.slot = mode == Mode::Host ? NET_HOST_SLOT : slot;
		chat.name = name;
		chat.text = text;
		if (mode == Mode::Host) {
			Broadcast(chat);
			ShowChat(chat);
		} else if (server && state != ClientState::Connecting) {
			Send(server, chat);
		} else {
			Log(WARNING, "Net", "Not connected, chat dropped: {}", text);
		}
	}

	// a game with a party in a loaded area, not in the middle of being replaced
	static const Actor* GameReady()
	{
		if (core->QuitFlag & (QF_QUITGAME | QF_EXITGAME | QF_LOADGAME | QF_ENTERGAME)) return nullptr;
		const Game* game = core->GetGame();
		if (!game || !core->GetGameControl() || game->GetPartySize(false) == 0) return nullptr;
		const Actor* first = game->FindPC(1);
		if (!first || !first->GetCurrentArea()) return nullptr;
		return first;
	}

	// TODO: M1.5 ground piles come and go with dropped items, so they replicate with inventories
	static bool IsPile(const Container* container)
	{
		return container->containerType == IE_CONTAINER_PILE;
	}

	static NetActorTableEntry ObjectEntry(const Scriptable* scr, NetObjectKind kind, const std::string& area)
	{
		NetActorTableEntry entry;
		entry.netID = scr->GetGlobalID();
		entry.kind = kind;
		entry.area = area;
		entry.scriptName = std::string(scr->GetScriptName());
		entry.x = scr->Pos.x;
		entry.y = scr->Pos.y;
		return entry;
	}

	// every actor, door, container and region in every loaded area; netID is the local global ID
	static std::vector<NetActorTableEntry> ListActors()
	{
		std::vector<NetActorTableEntry> list;
		const Game* game = core->GetGame();
		for (size_t idx = 0; idx < game->GetLoadedMapCount(); ++idx) {
			const Map* map = game->GetMap(unsigned(idx));
			std::string area(map->GetScriptRef());
			for (const Actor* actor : map->GetAllActors()) {
				NetActorTableEntry entry = ObjectEntry(actor, NetObjectKind::Actor, area);
				entry.partySlot = actor->InParty;
				list.push_back(std::move(entry));
			}
			const TileMap* tmap = map->GetTileMap();
			for (const auto& door : tmap->GetDoors()) {
				list.push_back(ObjectEntry(door.get(), NetObjectKind::Door, area));
			}
			for (const auto& container : tmap->GetContainers()) {
				if (IsPile(container.get())) continue;
				list.push_back(ObjectEntry(container.get(), NetObjectKind::Container, area));
			}
			for (size_t i = 0; const InfoPoint* ip = tmap->GetInfoPoint(i); ++i) {
				list.push_back(ObjectEntry(ip, NetObjectKind::Region, area));
			}
		}
		return list;
	}

	static NetObjectState ObjectState(const Scriptable* scr)
	{
		NetObjectState state;
		state.netID = scr->GetGlobalID();
		if (const Door* door = Scriptable::As<Door>(scr)) {
			state.kind = NetObjectKind::Door;
			state.flags = door->Flags;
			state.trapDetected = uint8_t(door->TrapDetected);
			state.trapped = uint8_t(door->Trapped);
		} else if (const Container* container = Scriptable::As<Container>(scr)) {
			state.kind = NetObjectKind::Container;
			state.flags = container->Flags;
			state.trapDetected = uint8_t(container->TrapDetected);
			state.trapped = uint8_t(container->Trapped);
			state.items = uint16_t(container->inventory.GetSlotCount());
		} else if (const InfoPoint* ip = Scriptable::As<InfoPoint>(scr)) {
			state.kind = NetObjectKind::Region;
			state.flags = ip->Flags;
			state.trapDetected = uint8_t(ip->TrapDetected);
			state.trapped = uint8_t(ip->Trapped);
		}
		return state;
	}

	static std::vector<const Scriptable*> ListObjects(const Map* map)
	{
		std::vector<const Scriptable*> list;
		const TileMap* tmap = map->GetTileMap();
		for (const auto& door : tmap->GetDoors()) {
			list.push_back(door.get());
		}
		for (const auto& container : tmap->GetContainers()) {
			if (IsPile(container.get())) continue;
			list.push_back(container.get());
		}
		for (size_t i = 0; const InfoPoint* ip = tmap->GetInfoPoint(i); ++i) {
			list.push_back(ip);
		}
		return list;
	}

	static NetActorStateEntry ActorState(const Actor* actor)
	{
		NetActorStateEntry entry;
		entry.netID = actor->GetGlobalID();
		entry.x = actor->Pos.x;
		entry.y = actor->Pos.y;
		entry.orientation = uint8_t(actor->GetOrientation());
		entry.stance = actor->GetStance();
		entry.hp = int32_t(actor->GetStat(IE_HITPOINTS));
		entry.maxHP = int32_t(actor->GetStat(IE_MAXHITPOINTS));
		entry.state = actor->GetStat(IE_STATE_ID);
		return entry;
	}

	static NetCheckEntry CheckEntry(uint32_t netID, const Actor* actor)
	{
		NetCheckEntry entry;
		entry.netID = netID;
		entry.a = int32_t(actor->GetStat(IE_HITPOINTS));
		entry.b = actor->GetStat(IE_MAXHITPOINTS);
		entry.c = actor->GetStat(IE_STATE_ID);
		return entry;
	}

	static NetCheckEntry CheckEntry(uint32_t netID, const NetObjectState& state)
	{
		NetCheckEntry entry;
		entry.netID = netID;
		entry.kind = state.kind;
		entry.a = int32_t(state.flags);
		entry.b = uint32_t(state.trapDetected) | uint32_t(state.trapped) << 8;
		return entry;
	}

	// what both sides hash: everyone in the area plus its objects, under network IDs
	// (the host's are its own; the client translates and lists what it can't map as 0)
	uint64_t Checksum(const Map* map, size_t& count) const
	{
		std::vector<NetCheckEntry> entries;
		for (const Actor* actor : map->GetAllActors()) {
			uint32_t netID = mode == Mode::Host ? actor->GetGlobalID() : ids.Net(actor->GetGlobalID());
			entries.push_back(CheckEntry(netID, actor));
		}
		for (const Scriptable* obj : ListObjects(map)) {
			uint32_t netID = mode == Mode::Host ? obj->GetGlobalID() : ids.Net(obj->GetGlobalID());
			entries.push_back(CheckEntry(netID, ObjectState(obj)));
		}
		count = entries.size();
		return NetStateChecksum(std::string(map->GetScriptRef()), std::move(entries));
	}

	// NetTestVar: report the test global, so logs of both sides can be compared
	void ReportTestVar(const char* side) const
	{
		if (testVar.empty() || !core->GetGame()) return;
		std::string var = testVar.substr(0, testVar.find('='));
		ieDword value = CheckVariable(core->GetGame(), StringParam(var), "GLOBAL");
		Log(MESSAGE, "Net", "{}: global {} = {}", side, var, value);
	}

	// the event loop -------------------------------------------------------

	void Poll()
	{
		if (mode == Mode::Client && !server && !stopped && GetMilliseconds() >= nextConnectAttempt) {
			Connect();
		}

		ENetEvent event;
		while (host && enet_host_service(host, &event, 0) > 0) {
			switch (event.type) {
				case ENET_EVENT_TYPE_CONNECT:
					OnConnect(event.peer);
					break;
				case ENET_EVENT_TYPE_RECEIVE:
					OnReceive(event.peer, event.packet->data, event.packet->dataLength);
					enet_packet_destroy(event.packet);
					break;
				case ENET_EVENT_TYPE_DISCONNECT:
					OnDisconnect(event.peer);
					break;
				default:
					break;
			}
		}
	}

	void OnConnect(ENetPeer* peer)
	{
		if (mode == Mode::Client) {
			Log(MESSAGE, "Net", "Connected to host {}, saying hello.", PeerAddress(peer));
			NetHello hello;
			hello.name = name;
			hello.gameType = core->config.GameType;
			Send(peer, hello);
			return;
		}

		uint8_t slot = 1;
		while (FindSlot(slot)) ++slot;
		NetPeerInfo info;
		info.peer = peer;
		info.slot = slot;
		peers.push_back(std::move(info));
		Log(MESSAGE, "Net", "Peer {} connected, assigned slot {}.", PeerAddress(peer), slot);
	}

	bool FindSlot(uint8_t slot) const
	{
		for (const auto& info : peers) {
			if (info.slot == slot) return true;
		}
		return false;
	}

	void OnDisconnect(ENetPeer* peer)
	{
		if (mode == Mode::Client) {
			server = nullptr;
			if (state != ClientState::Connecting) {
				LeaveGame("Lost the connection to the host.");
			} else if (!stopped) {
				Log(MESSAGE, "Net", "Could not reach the host, retrying in {}s.", RECONNECT_DELAY / 1000);
			}
			nextConnectAttempt = GetMilliseconds() + RECONNECT_DELAY;
			return;
		}

		for (auto it = peers.begin(); it != peers.end(); ++it) {
			if (it->peer == peer) {
				if (it->transfer) {
					Log(MESSAGE, "Net", "Dropped the save transfer to slot {} after {} of {} bytes.", it->slot, it->transfer->Sent(), it->transfer->Begin().size);
				}
				Log(MESSAGE, "Net", "Player '{}' (slot {}) disconnected.", it->name, it->slot);
				peers.erase(it);
				break;
			}
		}
	}

	void OnReceive(ENetPeer* peer, const uint8_t* data, size_t size)
	{
		NetMsgType type;
		if (!NetPeekType(data, size, type)) {
			Log(WARNING, "Net", "Dropping malformed or incompatible packet ({} bytes) from {}.", size, PeerAddress(peer));
			return;
		}

		bool ok = mode == Mode::Host ? HostReceive(peer, type, data, size) : ClientReceive(type, data, size);
		if (!ok) {
			Log(WARNING, "Net", "Dropping unexpected or malformed message {} from {}.", int(type), PeerAddress(peer));
		}
	}

	// host -----------------------------------------------------------------

	bool HostReceive(ENetPeer* peer, NetMsgType type, const uint8_t* data, size_t size)
	{
		NetPeerInfo* info = FindPeer(peer);
		if (!info) return false;

		switch (type) {
			case NetMsgType::Hello:
				{
					NetHello hello;
					if (info->welcomed || !NetDecode(data, size, hello)) return false;
					if (hello.gameType != core->config.GameType) {
						NetReject reject;
						reject.reason = fmt::format("game type mismatch: host runs '{}'", core->config.GameType);
						Send(peer, reject);
						enet_peer_disconnect_later(peer, 0);
						Log(WARNING, "Net", "Rejected '{}': {}", hello.name, reject.reason);
						return true;
					}
					info->name = hello.name;
					info->welcomed = true;
					info->wantsSave = true;
					NetWelcome welcome;
					welcome.slot = info->slot;
					welcome.hostName = name;
					Send(peer, welcome);
					Log(MESSAGE, "Net", "Player '{}' joined in slot {}.", info->name, info->slot);
					if (!testChat.empty()) {
						SendChat(testChat);
						testChat.clear();
					}
					return true;
				}
			case NetMsgType::Chat:
				{
					NetChat chat;
					if (!info->welcomed || !NetDecode(data, size, chat)) return false;
					// trust the session for identity, not the packet
					chat.slot = info->slot;
					chat.name = info->name;
					ShowChat(chat);
					Broadcast(chat, NetChannel::Reliable, peer);
					return true;
				}
			case NetMsgType::Command:
				{
					NetCommand cmd;
					if (!info->welcomed || !NetDecode(data, size, cmd)) return false;
					HandleCommand(*info, cmd);
					return true;
				}
			case NetMsgType::Ready:
				{
					NetReady ready;
					if (!info->welcomed || !NetDecode(data, size, ready)) return false;
					HandleReady(*info, ready);
					return true;
				}
			case NetMsgType::ResyncRequest:
				{
					NetResyncRequest request;
					if (!info->welcomed || !NetDecode(data, size, request)) return false;
					if (!info->transfer) {
						info->wantsSave = true;
						Log(MESSAGE, "Net", "Slot {} ({}) asked for a resync{}.", info->slot, info->name, request.reason == NetResyncRequest::DESYNC ? " after a checksum mismatch" : "");
					}
					return true;
				}
			case NetMsgType::Pause:
				{
					NetPause pause;
					if (!info->welcomed || !NetDecode(data, size, pause)) return false;
					HandlePause(*info, pause);
					return true;
				}
			default:
				return false;
		}
	}

	void HandleReady(NetPeerInfo& info, const NetReady& ready)
	{
		if (ready.transferID != info.awaitedTransfer) {
			Log(MESSAGE, "Net", "Slot {} loaded save #{}, but #{} is on its way; still waiting.", info.slot, ready.transferID, info.awaitedTransfer);
			return;
		}
		Log(MESSAGE, "Net", "Slot {} ({}) loaded save #{} and mapped {} actors and objects; replaying {} events.", info.slot, info.name, ready.transferID, ready.mapped, info.pending.size());
		info.live = true;
		info.awaitedTransfer = 0;
		for (const auto& event : info.pending) {
			SendRaw(info.peer, event, NetChannel::Reliable);
		}
		info.pending.clear();

		// what a save doesn't carry
		NetPause pause;
		pause.on = IsPaused();
		Send(info.peer, pause);
		Send(info.peer, owners);
		const Game* game = core->GetGame();
		for (uint32_t netID : ready.unmatched) {
			const Actor* actor = game ? game->GetActorByGlobalID(netID) : nullptr;
			if (!actor || actor->InParty || !actor->GetCurrentArea()) continue;
			NetSpawn spawn;
			if (!SpawnMessage(actor, spawn)) continue;
			Send(info.peer, spawn);
			Log(MESSAGE, "Net", "Slot {} has no net id {} '{}' in {}, sent it as a spawn.", info.slot, netID, spawn.scriptName, spawn.area);
		}
	}

	static bool IsPaused()
	{
		const GameControl* gc = core->GetGameControl();
		return gc && (gc->GetDialogueFlags() & DF_FREEZE_SCRIPTS);
	}

	void HandlePause(const NetPeerInfo& info, const NetPause& pause)
	{
		Log(MESSAGE, "Net", "Slot {} ({}) wants the game {}.", info.slot, info.name, pause.on ? "paused" : "running");
		pauseRequester = info.slot;
		bool changed = core->SetPause(pause.on ? PauseState::On : PauseState::Off);
		pauseRequester = NET_HOST_SLOT;
		if (!changed) {
			// refused (cutscene) or already so: tell the asker how things are
			NetPause reply;
			reply.on = IsPaused();
			reply.slot = NET_HOST_SLOT;
			Send(info.peer, reply);
		}
	}

	uint8_t OwnerOf(const Actor* actor) const
	{
		for (const auto& entry : owners.entries) {
			if (entry.netID == actor->GetGlobalID()) return entry.owner;
		}
		return NET_HOST_SLOT;
	}

	// tells one player something in its message window
	static void Notify(const NetPeerInfo& info, const std::string& text)
	{
		NetText msg;
		msg.text = fmt::format("[p][color=FF8080FF]{}[/color][/p]", text);
		Send(info.peer, msg);
	}

	// a GameScript object target: the -1 marker means "this global ID" (GenerateActionDirect)
	static bool IsDirectTarget(const NetActionObject& object)
	{
		return object.fields[0] == -1;
	}

	// which actions a client may order, and what they refer to; anything else is refused
	static bool ValidateCommand(const Actor* actor, const std::string& name, const NetCommand& cmd, std::string& why)
	{
		static const std::set<std::string> movement = { "movetopoint", "runtopoint", "addwaypoint", "nidspecial2" };
		static const std::set<std::string> targeted = {
			"nidspecial3", // attack
			"nidspecial4", // defend
			"nidspecial5", // use an item on
			"nidspecial6", // cast a memorized spell on
			"nidspecial7", // cast a known spell or ability on
			"pickpockets", "picklock", "removetraps", "triggerwalkto", "bashdoor"
		};
		static const std::set<std::string> atPoint = { "spellpoint", "spellpointnodec", "useitempoint" };

		if (!movement.count(name) && !targeted.count(name) && !atPoint.count(name) && name != "nidspecial9") {
			why = "not an order clients can give (yet)";
			return false;
		}
		const Map* area = actor->GetCurrentArea();
		if (cmd.objectMask & 2 && IsDirectTarget(cmd.objects[1])) {
			const Scriptable* target = area ? const_cast<Map*>(area)->GetScriptableByGlobalID(ieDword(cmd.objects[1].fields[1])) : nullptr;
			if (!target) {
				why = fmt::format("target {} isn't in {}'s area", cmd.objects[1].fields[1], fmt::WideToChar { actor->GetShortName() });
				return false;
			}
		}
		if (name == "nidspecial9" && (!area || !area->GetDoorByGlobalID(ieDword(cmd.int0)))) {
			why = fmt::format("door {} isn't in the actor's area", cmd.int0);
			return false;
		}
		ResRef spell(cmd.string0.c_str());
		if (name == "nidspecial6" || name == "spellpoint") {
			if (!const_cast<Actor*>(actor)->spellbook.HaveSpell(spell, 0)) {
				why = fmt::format("no {} memorized", cmd.string0);
				return false;
			}
		} else if (name == "nidspecial7" || name == "spellpointnodec") {
			if (!actor->spellbook.KnowSpell(spell) && !const_cast<Actor*>(actor)->spellbook.HaveSpell(spell, 0)) {
				why = fmt::format("{} isn't known", cmd.string0);
				return false;
			}
		} else if ((name == "nidspecial5" || name == "useitempoint") && !actor->inventory.GetSlotItem(ieDword(cmd.int0))) {
			why = fmt::format("nothing in slot {}", cmd.int0);
			return false;
		}
		return true;
	}

	static Holder<Action> BuildAction(int actionID, const NetCommand& cmd)
	{
		Holder<Action> action = Action::MakeAction();
		action->actionID = static_cast<unsigned short>(actionID);
		action->int0Parameter = cmd.int0;
		action->int1Parameter = cmd.int1;
		action->int2Parameter = cmd.int2;
		action->pointParameter = Point(cmd.x, cmd.y);
		action->string0Parameter = StringParam(cmd.string0);
		action->string1Parameter = StringParam(cmd.string1);
		action->flags = cmd.actionFlags;
		for (size_t i = 0; i < NET_ACTION_OBJECTS; ++i) {
			if (!(cmd.objectMask & (1 << i))) continue;
			const NetActionObject& src = cmd.objects[i];
			auto object = MakeHolder<Object>();
			std::copy(src.fields.begin(), src.fields.end(), object->objectFields);
			std::copy(src.filters.begin(), src.filters.end(), object->objectFilters);
			object->objectRect = Region(src.rect[0], src.rect[1], src.rect[2], src.rect[3]);
			object->objectName = StringParam(src.name);
			action->objects[i] = std::move(object);
		}
		return action;
	}

	void HandleCommand(const NetPeerInfo& info, const NetCommand& cmd)
	{
		Game* game = core->GetGame();
		// network IDs are our own global IDs
		Actor* actor = game ? game->GetActorByGlobalID(cmd.actorID) : nullptr;
		if (!actor || !actor->InParty) {
			Log(WARNING, "Net", "Slot {}: rejected '{}' for unknown or non-party actor {}.", info.slot, cmd.action, cmd.actorID);
			return;
		}
		uint8_t owner = OwnerOf(actor);
		if (owner != info.slot) {
			Log(WARNING, "Net", "Slot {} ({}): rejected '{}' for {} (id {}), who belongs to slot {}.", info.slot, info.name, cmd.action, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, owner);
			Notify(info, fmt::format("{} is controlled by player {}.", fmt::WideToChar { actor->GetShortName() }, owner));
			return;
		}

		std::string name = LowerCase(cmd.action);
		if (name == "useexit") {
			const Map* area = actor->GetCurrentArea();
			const InfoPoint* exit = area ? area->GetInfoPointByGlobalID(ieDword(cmd.int0)) : nullptr;
			if (!exit || exit->Type != ST_TRAVEL) {
				Log(WARNING, "Net", "Slot {}: rejected UseExit({}) for {}: no travel region.", info.slot, cmd.int0, cmd.actorID);
				return;
			}
			actor->UseExit(exit->GetGlobalID());
			Log(MESSAGE, "Net", "Slot {} ({}): {} (id {}) may use exit '{}'.", info.slot, info.name, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, exit->GetScriptName());
			return;
		}

		std::string why;
		if (!ValidateCommand(actor, name, cmd, why)) {
			Log(WARNING, "Net", "Slot {}: rejected '{}' for {}: {}.", info.slot, cmd.action, cmd.actorID, why);
			Notify(info, fmt::format("The host refused that order: {}.", why));
			return;
		}
		int actionID = ActionID(name);
		if (actionID < 0) {
			Log(WARNING, "Net", "Slot {}: rejected unknown action '{}'.", info.slot, cmd.action);
			return;
		}
		Holder<Action> action = BuildAction(actionID, cmd);

		// mirror GameControl: the flags say which of its calls made the order
		if (cmd.flags & NetCommand::STOP) {
			actor->Stop();
		}
		if (cmd.flags & NetCommand::QUEUE) {
			actor->AddAction(std::move(action));
		} else {
			actor->CommandActor(std::move(action), !(cmd.flags & NetCommand::KEEP_PATH));
		}
		std::string target;
		if (cmd.objectMask & 2 && IsDirectTarget(cmd.objects[1])) {
			const Scriptable* tgt = FindScriptable(ieDword(cmd.objects[1].fields[1]));
			target = fmt::format(" on {} '{}'", cmd.objects[1].fields[1], tgt ? std::string(tgt->GetScriptName()) : "?");
		} else if (name == "nidspecial9") {
			const Scriptable* door = FindScriptable(ieDword(cmd.int0));
			target = fmt::format(" on door {} '{}'", cmd.int0, door ? std::string(door->GetScriptName()) : "?");
		} else if (cmd.x >= 0) {
			target = fmt::format(" at [{}.{}]", cmd.x, cmd.y);
		}
		std::string spell = cmd.string0.empty() ? "" : fmt::format(" ({})", cmd.string0);
		Log(MESSAGE, "Net", "Slot {} ({}): injected '{}'{}{} for {} (id {}) at {}.", info.slot, info.name, cmd.action, spell, target, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, actor->Pos);
	}

	static bool ReadFiles(const path_t& dir, std::vector<NetFile>& files)
	{
		DirectoryIterator it(dir);
		it.SetFlags(DirectoryIterator::Files);
		if (!it) return false;
		do {
			NetFile file;
			file.name = it.GetName();
			FileStream fs;
			if (!fs.Open(it.GetFullPath())) return false;
			file.data.resize(fs.Size());
			if (fs.Read(file.data.data(), file.data.size()) != strret_t(file.data.size())) return false;
			files.push_back(std::move(file));
		} while (++it);
		std::sort(files.begin(), files.end(), [](const NetFile& a, const NetFile& b) { return a.name < b.name; });
		return !files.empty();
	}

	// write the running game as a save and note who is who in it; runs between ticks
	bool TakeSnapshot(NetSnapshot& out)
	{
		const Actor* first = GameReady();
		if (!first) return false;

		tick_t start = GetMilliseconds();
		path_t dir = PathJoin(transferDir, "out");
		if (!MakeDirectories(dir)) {
			Log(ERROR, "Net", "Unable to create the transfer directory '{}'.", dir);
			return false;
		}
		DelTree(dir, false);
		std::vector<NetFile> files;
		if (!SaveGameIterator::WriteGameFiles(dir) || !ReadFiles(dir, files)) {
			Log(ERROR, "Net", "Unable to write the game for transfer into '{}'.", dir);
			return false;
		}
		out.packed = NetPackFiles(files);
		out.area = std::string(first->GetCurrentArea()->GetScriptRef());
		out.table.actors = ListActors();

		std::string fileList;
		for (const auto& file : files) {
			fileList += fmt::format(" {}({})", file.name, file.data.size());
		}
		Log(MESSAGE, "Net", "Snapshot of the game written in {} ms: {} files,{}; {} actors and objects.", GetMilliseconds() - start, files.size(), fileList, out.table.actors.size());
		size_t logged = 0;
		for (const auto& entry : out.table.actors) {
			if (entry.kind != NetObjectKind::Actor || logged++ == MAX_LOGGED_ACTORS) continue;
			Log(MESSAGE, "Net", "Snapshot actor: net id {} '{}' party slot {} in {} at ({}, {})", entry.netID, entry.scriptName, entry.partySlot, entry.area, entry.x, entry.y);
		}
		ReportTestVar("host snapshot");
		return true;
	}

	// serve pending joins and resyncs, then push the transfers along
	void HostTransfers()
	{
		bool anyWaiting = false;
		for (const auto& info : peers) {
			anyWaiting |= info.welcomed && info.wantsSave && !info.transfer;
		}
		NetSnapshot snapshot;
		if (anyWaiting && TakeSnapshot(snapshot)) {
			for (auto& info : peers) {
				if (!info.welcomed || !info.wantsSave || info.transfer) continue;
				info.wantsSave = false;
				info.transfer = std::make_shared<NetSaveSender>(nextTransferID++, snapshot.packed, snapshot.area);
				info.table = snapshot.table;
				info.table.transferID = info.transfer->Begin().transferID;
				// from here on the peer catches up through its save and then the pending events
				info.live = false;
				info.awaitedTransfer = info.table.transferID;
				info.pending.clear();
				const NetSaveBegin& begin = info.transfer->Begin();
				Send(info.peer, begin, NetChannel::Transfer);
				Log(MESSAGE, "Net", "Sending save #{} to slot {}: {} bytes, hash {:016x}, party in {}.", begin.transferID, info.slot, begin.size, begin.hash, begin.area);
			}
		}

		for (auto& info : peers) {
			if (!info.transfer) continue;
			unsigned int budget = slowTransfer ? 1 : CHUNKS_PER_TICK;
			while (budget-- && !info.transfer->Done()) {
				Send(info.peer, info.transfer->NextChunk(slowTransfer ? SLOW_CHUNK : NET_SAVE_CHUNK), NetChannel::Transfer);
			}
			if (!info.transfer->Done()) continue;
			// same channel, so it arrives right after the last chunk
			Send(info.peer, info.table, NetChannel::Transfer);
			Log(MESSAGE, "Net", "Sent save #{} and its table of {} actors and objects to slot {}.", info.table.transferID, info.table.actors.size(), info.slot);
			info.transfer.reset();
			info.table = NetActorTable();
		}
	}

	// the default control of party members, unless the host reassigned one
	void UpdateOwnership()
	{
		const Game* game = core->GetGame();
		std::vector<uint8_t> players = { NET_HOST_SLOT };
		for (const auto& info : peers) {
			if (info.welcomed) players.push_back(info.slot);
		}
		std::sort(players.begin(), players.end());

		NetOwnership now;
		for (int slot = 1; slot <= game->GetPartySize(false); ++slot) {
			const Actor* pc = game->FindPC(slot);
			if (!pc) continue;
			NetOwnershipEntry entry;
			entry.netID = pc->GetGlobalID();
			entry.partySlot = uint8_t(slot);
			entry.owner = NetDefaultOwner(entry.partySlot, players);
			auto it = ownerOverride.find(entry.partySlot);
			if (it != ownerOverride.end() && std::find(players.begin(), players.end(), it->second) != players.end()) {
				entry.owner = it->second;
			}
			now.entries.push_back(entry);
		}

		bool same = now.entries.size() == owners.entries.size();
		for (size_t i = 0; same && i < now.entries.size(); ++i) {
			same = now.entries[i].netID == owners.entries[i].netID && now.entries[i].owner == owners.entries[i].owner;
		}
		if (same) return;
		owners = now;
		for (const auto& entry : owners.entries) {
			const Actor* pc = game->GetActorByGlobalID(entry.netID);
			Log(MESSAGE, "Net", "Ownership: party slot {} {} (net id {}) -> player slot {}.", entry.partySlot, pc ? fmt::format("{}", fmt::WideToChar { pc->GetShortName() }) : "?", entry.netID, entry.owner);
		}
		SendEvent(owners, false);
	}

	bool SpawnMessage(const Actor* actor, NetSpawn& spawn) const
	{
		spawn.netID = actor->GetGlobalID();
		spawn.area = std::string(actor->GetCurrentArea()->GetScriptRef());
		spawn.scriptName = std::string(actor->GetScriptName());
		spawn.x = actor->Pos.x;
		spawn.y = actor->Pos.y;
		spawn.orientation = uint8_t(actor->GetOrientation());
		spawn.creature = WriteCreature(actor);
		if (spawn.creature.empty()) {
			Log(ERROR, "Net", "Unable to serialize net id {} for a spawn.", spawn.netID);
			return false;
		}
		return true;
	}

	// forget what the clients were told about the old area; their next save has the new one
	void ResetWorld(const Map* map)
	{
		replMap = map;
		replArea = std::string(map->GetScriptRef());
		delta.Reset();
		knownActors.clear();
		for (const Actor* actor : map->GetAllActors()) {
			knownActors.insert(actor->GetGlobalID());
		}
		objectStates.clear();
		for (const Scriptable* obj : ListObjects(map)) {
			objectStates[obj->GetGlobalID()] = ObjectState(obj);
		}
		hostVfx.clear();
		for (const Actor* actor : map->GetAllActors()) {
			for (const auto& vvc : actor->vfxQueue) {
				hostVfx[vvc.get()] = HostVfx { nextVfxKey++, vvc->ResName, actor->GetGlobalID() };
			}
		}
	}

	// party members changing areas need a fresh save (all loaded areas, new IDs on the client)
	void TrackParty(const Map* map)
	{
		const Game* game = core->GetGame();
		std::map<ieDword, std::string> party;
		for (int slot = 1; slot <= game->GetPartySize(false); ++slot) {
			const Actor* pc = game->FindPC(slot);
			if (pc && pc->GetCurrentArea()) {
				party[pc->GetGlobalID()] = std::string(pc->GetCurrentArea()->GetScriptRef());
			}
		}
		if (!partyAreas.empty() && party != partyAreas) {
			if (!areaSettle) {
				Log(MESSAGE, "Net", "The party changed (areas or members); the clients follow once it settles.");
			}
			areaSettle = AREA_SETTLE_TICKS;
			std::string area(map->GetScriptRef());
			if (area != replArea) {
				NetAreaChange change;
				change.area = area;
				SendEvent(change, false);
				Log(MESSAGE, "Net", "The party is now in {} (was {}).", area, replArea);
			}
		}
		partyAreas = std::move(party);

		if (areaSettle && --areaSettle == 0) {
			size_t count = 0;
			for (auto& info : peers) {
				if (!info.welcomed) continue;
				info.wantsSave = true;
				count++;
			}
			Log(MESSAGE, "Net", "Party settled in {}: resyncing {} player(s).", map->GetScriptRef(), count);
		}
	}

	// spawns, despawns, door/container/region changes and actor effects in the replicated area
	void TrackWorld()
	{
		const Actor* first = GameReady();
		if (!first) return;
		const Map* map = first->GetCurrentArea();
		TrackParty(map);
		if (map != replMap || std::string(map->GetScriptRef()) != replArea) {
			ResetWorld(map);
			return;
		}

		std::set<ieDword> current;
		for (const Actor* actor : map->GetAllActors()) {
			ieDword id = actor->GetGlobalID();
			current.insert(id);
			if (knownActors.count(id) || actor->InParty) continue;
			NetSpawn spawn;
			if (!SpawnMessage(actor, spawn)) continue;
			SendEvent(spawn, true);
			Log(MESSAGE, "Net", "Event: spawn of net id {} '{}' ({}) in {} at ({}, {}), {} byte creature.", id, actor->GetScriptName(), fmt::WideToChar { actor->GetShortName() }, spawn.area, spawn.x, spawn.y, spawn.creature.size());
		}
		for (ieDword id : knownActors) {
			// party members leaving are area changes, handled above
			if (current.count(id) || partyAreas.count(id)) continue;
			NetDespawn despawn;
			despawn.netID = id;
			SendEvent(despawn, true);
			Log(MESSAGE, "Net", "Event: despawn of net id {}.", id);
		}
		knownActors = std::move(current);

		for (const Scriptable* obj : ListObjects(map)) {
			NetObjectState now = ObjectState(obj);
			NetObjectState& was = objectStates[now.netID];
			if (was.flags == now.flags && was.trapDetected == now.trapDetected && was.trapped == now.trapped && was.items == now.items) continue;
			Log(MESSAGE, "Net", "Event: {} '{}' (net id {}) flags {:#x} -> {:#x}, trap detected {} armed {}, {} items.", KindName(now.kind), obj->GetScriptName(), now.netID, was.flags, now.flags, now.trapDetected, now.trapped, now.items);
			was = now;
			SendEvent(now, true);
		}

		TrackVfx(map);
	}

	// effects on actors come and go with the effects that own them, so they are diffed per tick
	void TrackVfx(const Map* map)
	{
		std::map<const ScriptedAnimation*, HostVfx> current;
		for (const Actor* actor : map->GetAllActors()) {
			for (const auto& vvc : actor->vfxQueue) {
				const ScriptedAnimation* sca = vvc.get();
				auto it = hostVfx.find(sca);
				if (it != hostVfx.end() && it->second.name == sca->ResName && it->second.actorID == actor->GetGlobalID()) {
					current[sca] = it->second;
					continue;
				}
				HostVfx info { nextVfxKey++, sca->ResName, actor->GetGlobalID() };
				current[sca] = info;
				NetVfx msg = VfxMessage(*sca, info.key);
				msg.actorID = actor->GetGlobalID();
				SendEvent(msg, false);
				Log(MESSAGE, "Net", "Event: vfx #{} '{}' on net id {}{}.", info.key, sca->ResName, msg.actorID, msg.flags & NetVfx::LOOPING ? ", looping" : "");
			}
		}
		for (const auto& old : hostVfx) {
			if (current.count(old.first)) continue;
			NetVfx end;
			end.key = old.second.key;
			end.flags = NetVfx::END;
			SendEvent(end, false);
			Log(MESSAGE, "Net", "Event: vfx #{} '{}' on net id {} ended.", old.second.key, old.second.name, old.second.actorID);
		}
		hostVfx = std::move(current);
	}

	static NetVfx VfxMessage(const ScriptedAnimation& sca, uint32_t key)
	{
		NetVfx msg;
		msg.key = key;
		msg.resource = std::string(sca.ResName);
		msg.x = sca.Pos.x;
		msg.y = sca.Pos.y;
		msg.xOffset = sca.XOffset;
		msg.yOffset = sca.YOffset;
		msg.zOffset = sca.ZOffset;
		msg.tint = sca.Tint.Packed();
		msg.transparency = sca.Transparency;
		msg.sequenceFlags = sca.SequenceFlags;
		msg.orientation = uint8_t(sca.Orientation);
		// still relative until its first update, absolute game time afterwards
		msg.duration = sca.Duration;
		if (!sca.justCreated && sca.Duration != 0xffffffff) {
			ieDword now = core->GetGame()->GameTime;
			msg.duration = sca.Duration > now ? sca.Duration - now : 0;
		}
		if (sca.twin) msg.flags |= NetVfx::TWIN;
		if (sca.effect_owned && sca.Duration == 0xffffffff && (sca.SequenceFlags & IE_VVC_LOOP)) {
			msg.flags |= NetVfx::LOOPING;
		}
		return msg;
	}

	void HostPostTick()
	{
		if (GameReady()) {
			hostGameTicks++;
			testTicks++;
			if (hostGameTicks == TEST_HOOK_DELAY) {
				RunTestVar();
				RunTestMove();
			}
			UpdateOwnership();
			// before any snapshot is taken, so a peer's save and its pending events don't overlap
			TrackWorld();
		}

		bool anyWelcomed = false;
		for (const auto& info : peers) {
			anyWelcomed |= info.welcomed;
		}
		if (!anyWelcomed) return;
		HostTransfers();

		if (ticks % SNAPSHOT_INTERVAL) return;
		const Actor* first = GameReady();
		if (!first) return;
		const Map* area = first->GetCurrentArea();

		// everyone in the party's area, clients ignore the ones they can't map
		NetActorState state;
		state.tick = ++snapshotSeq;
		state.area = std::string(area->GetScriptRef());
		bool keyframe = state.tick % KEYFRAME_INTERVAL == 0;
		std::vector<NetActorStateEntry> current;
		for (const Actor* actor : area->GetAllActors()) {
			if (current.size() == NET_MAX_ENTRIES) break;
			NetActorStateEntry entry = ActorState(actor);
			current.push_back(entry);
			LogMovement("host", entry);
			if (state.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Sync check #{}: net id {} at ({}, {}) stance {} hp {}/{} state {:#x}", state.tick, entry.netID, entry.x, entry.y, entry.stance, entry.hp, entry.maxHP, entry.state);
			}
		}
		state.actors = delta.Build(state.tick, keyframe, current);
		if (keyframe) {
			size_t count = 0;
			state.flags = NetActorState::KEYFRAME | NetActorState::CHECKSUM;
			state.checksum = Checksum(area, count);
			if (state.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Checksum #{} of {}: {:016x} over {} actors and objects.", state.tick, state.area, state.checksum, count);
			}
		}
		const auto data = NetEncode(state);
		for (const auto& info : peers) {
			if (info.welcomed && info.live) {
				SendRaw(info.peer, data, NetChannel::State);
			}
		}
	}

	// log an actor's position about once a second while it moves
	void LogMovement(const char* side, const NetActorStateEntry& entry)
	{
		Point pos(entry.x, entry.y);
		auto it = lastLoggedPos.find(entry.netID);
		bool moved = it == lastLoggedPos.end() || it->second != pos;
		if (!moved || (it != lastLoggedPos.end() && ticks % 15)) return;
		lastLoggedPos[entry.netID] = pos;
		Log(MESSAGE, "Net", "{} state: actor {} at {} orient {} stance {}", side, entry.netID, pos, entry.orientation, entry.stance);
	}

	// host event capture ----------------------------------------------------

	bool InReplicatedArea(const Scriptable& scr) const
	{
		return replMap && scr.GetCurrentArea() == replMap;
	}

	void OnFeedback(const String& markup)
	{
		if (!Capturing()) return;
		NetText msg;
		msg.kind = NetText::FEEDBACK;
		msg.text = Utf8(markup);
		SendEvent(msg, true);
		Log(MESSAGE, "Net", "Event: feedback '{}'", PlainText(msg.text));
	}

	void OnOverheadText(const Scriptable& owner, const String& text, const Color& color)
	{
		if (!Capturing() || text.empty() || !InReplicatedArea(owner)) return;
		NetText msg;
		msg.kind = NetText::OVERHEAD;
		msg.ownerID = owner.GetGlobalID();
		msg.color = color.Packed();
		msg.text = Utf8(text);
		SendEvent(msg, false);
		Log(MESSAGE, "Net", "Event: overhead text on net id {}: '{}'", msg.ownerID, msg.text);
	}

	void OnSound(StringView resource, const AudioPlaybackConfig& config, bool speech)
	{
		// looping sounds belong to something that stops them later (projectiles, casting); the clients' copies do that
		if (!Capturing() || resource.empty() || config.loop) return;
		NetSound msg;
		msg.resource = std::string(resource.c_str(), resource.length());
		msg.flags = (config.spatial ? NetSound::SPATIAL : 0) | (speech ? NetSound::SPEECH : 0);
		msg.x = config.position.x;
		msg.y = config.position.y;
		msg.z = config.position.z;
		msg.masterVolume = uint8_t(Clamp(config.masterVolume, 0, 255));
		msg.channelVolume = uint8_t(Clamp(config.channelVolume, 0, 255));
		msg.muteDistance = config.muteDistance;
		SendEvent(msg, false);
		// footsteps and the like repeat a lot; log each sound once a second at most
		if (msg.resource != lastSound || ticks - lastSoundTick >= 15) {
			Log(MESSAGE, "Net", "Event: sound '{}'{} at ({}, {}).", msg.resource, speech ? " (speech)" : "", msg.x, msg.y);
			lastSound = msg.resource;
			lastSoundTick = ticks;
		}
	}

	void OnProjectile(const Map& map, const Projectile& pro, uint32_t targetID, bool fake)
	{
		if (!Capturing() || &map != replMap) return;
		NetProjectile msg;
		msg.name = std::string(pro.GetName());
		msg.index = pro.GetType();
		msg.casterID = pro.GetCaster();
		msg.targetID = targetID;
		msg.flags = targetID ? (fake ? NetProjectile::FAKE_TARGET : 0) : NetProjectile::AT_POINT;
		Point src = pro.GetPos();
		Point dest = pro.GetDestination();
		msg.x = src.x;
		msg.y = src.y;
		msg.tx = dest.x;
		msg.ty = dest.y;
		SendEvent(msg, false);
		Log(MESSAGE, "Net", "Event: projectile '{}' (#{}) from net id {} at {} to {} {}.", msg.name, msg.index, msg.casterID, src, targetID ? fmt::format("net id {}", targetID) : "point", dest);
	}

	void OnAreaVfx(const Map& map, VEFObject& vef)
	{
		if (!Capturing() || &map != replMap) return;
		NetVfx msg;
		const ScriptedAnimation* sca = vef.GetSingleObject();
		if (sca) {
			msg = VfxMessage(*sca, nextVfxKey++);
		} else {
			msg.key = nextVfxKey++;
			msg.flags |= NetVfx::COMPOSITE;
		}
		msg.flags |= NetVfx::ON_AREA;
		msg.resource = std::string(vef.ResName);
		msg.x = vef.Pos.x;
		msg.y = vef.Pos.y;
		SendEvent(msg, false);
		Log(MESSAGE, "Net", "Event: vfx #{} '{}' on the area at ({}, {}).", msg.key, msg.resource, msg.x, msg.y);
	}

	void OnPause(bool paused)
	{
		if (mode == Mode::Host) {
			NetPause msg;
			msg.on = paused;
			msg.slot = pauseRequester;
			SendEvent(msg, false);
			Log(MESSAGE, "Net", "Game {} (asked by slot {}), told everyone.", paused ? "paused" : "unpaused", pauseRequester);
			return;
		}
		if (applyingPause || !server || state != ClientState::InSync) return;
		NetPause msg;
		msg.on = paused;
		Send(server, msg);
		Log(MESSAGE, "Net", "Asked the host to {} the game.", paused ? "pause" : "unpause");
	}

	// client ---------------------------------------------------------------

	bool ClientReceive(NetMsgType type, const uint8_t* data, size_t size)
	{
		switch (type) {
			case NetMsgType::Welcome:
				{
					NetWelcome welcome;
					if (!NetDecode(data, size, welcome)) return false;
					state = ClientState::Joining;
					slot = welcome.slot;
					Log(MESSAGE, "Net", "Welcomed by host '{}', we are slot {}. Waiting for the game...", welcome.hostName, slot);
					if (!testChat.empty()) {
						SendChat(testChat);
						testChat.clear();
					}
					return true;
				}
			case NetMsgType::Reject:
				{
					NetReject reject;
					if (!NetDecode(data, size, reject)) return false;
					stopped = true;
					LeaveGame(fmt::format("The host rejected us: {}", reject.reason));
					return true;
				}
			case NetMsgType::Chat:
				{
					NetChat chat;
					if (!NetDecode(data, size, chat)) return false;
					ShowChat(chat);
					return true;
				}
			case NetMsgType::SaveBegin:
				{
					NetSaveBegin begin;
					if (state == ClientState::Connecting || !NetDecode(data, size, begin)) return false;
					Log(MESSAGE, "Net", "Receiving save #{}: {} bytes, hash {:016x}, party in {}.", begin.transferID, begin.size, begin.hash, begin.area);
					if (receiver.Begin(begin) == NetSaveReceiver::Status::Failed) {
						FailTransfer();
					}
					return true;
				}
			case NetMsgType::SaveChunk:
				{
					NetSaveChunk chunk;
					if (!NetDecode(data, size, chunk)) return false;
					size_t before = receiver.Received();
					auto status = receiver.Add(chunk);
					if (status == NetSaveReceiver::Status::Failed) {
						FailTransfer();
					} else if (status == NetSaveReceiver::Status::Complete) {
						LoadReceivedSave();
					} else if (receiver.Info().size >= 10 * NET_SAVE_CHUNK || slowTransfer || testResync) {
						// progress for big (or deliberately slow) transfers, every 25%
						size_t total = receiver.Info().size;
						if (before * 4 / total != receiver.Received() * 4 / total) {
							Log(MESSAGE, "Net", "Save #{}: {}% ({} of {} bytes)", receiver.Info().transferID, receiver.Received() * 100 / total, receiver.Received(), total);
						}
					}
					return true;
				}
			case NetMsgType::ActorTable:
				{
					NetActorTable msg;
					if (!NetDecode(data, size, msg)) return false;
					if (msg.transferID != loadingTransfer) {
						Log(WARNING, "Net", "Ignoring the actor table of save #{}, we are loading #{}.", msg.transferID, loadingTransfer);
						return true;
					}
					table = std::move(msg);
					haveTable = true;
					return true;
				}
			case NetMsgType::ActorState:
				{
					NetActorState snapshot;
					if (!NetDecode(data, size, snapshot)) return false;
					ApplyState(snapshot);
					return true;
				}
			default:
				break;
		}
		// world events only make sense for the game we mirror
		bool live = state == ClientState::InSync && core->GetGame();
		switch (type) {
			case NetMsgType::Spawn:
				return ClientEvent<NetSpawn>(data, size, live, [this](const NetSpawn& msg) { ApplySpawn(msg); });
			case NetMsgType::Despawn:
				return ClientEvent<NetDespawn>(data, size, live, [this](const NetDespawn& msg) { ApplyDespawn(msg); });
			case NetMsgType::Projectile:
				return ClientEvent<NetProjectile>(data, size, live, [this](const NetProjectile& msg) { ApplyProjectile(msg); });
			case NetMsgType::Vfx:
				return ClientEvent<NetVfx>(data, size, live, [this](const NetVfx& msg) { ApplyVfx(msg); });
			case NetMsgType::Sound:
				return ClientEvent<NetSound>(data, size, live, [](const NetSound& msg) { ApplySound(msg); });
			case NetMsgType::Text:
				return ClientEvent<NetText>(data, size, live, [this](const NetText& msg) { ApplyText(msg); });
			case NetMsgType::ObjectState:
				return ClientEvent<NetObjectState>(data, size, live, [this](const NetObjectState& msg) { ApplyObjectState(msg); });
			case NetMsgType::Pause:
				return ClientEvent<NetPause>(data, size, live, [this](const NetPause& msg) { ApplyPause(msg); });
			case NetMsgType::AreaChange:
				return ClientEvent<NetAreaChange>(data, size, true, [](const NetAreaChange& msg) {
					Log(MESSAGE, "Net", "The host's party moved to {}; the new area follows as a save.", msg.area);
				});
			case NetMsgType::Ownership:
				return ClientEvent<NetOwnership>(data, size, true, [this](const NetOwnership& msg) { ApplyOwnership(msg); });
			default:
				return false;
		}
	}

	template<typename MSG, typename FUNC>
	static bool ClientEvent(const uint8_t* data, size_t size, bool live, FUNC apply)
	{
		MSG msg;
		if (!NetDecode(data, size, msg)) return false;
		if (live) apply(msg);
		return true;
	}

	// give up on this connection: the save can't be trusted, and retrying would most likely repeat that
	void FailTransfer()
	{
		Log(ERROR, "Net", "Save transfer #{} failed: {}.", receiver.Info().transferID, receiver.Error());
		stopped = true;
		if (server) {
			enet_peer_disconnect(server, 0);
		}
		LeaveGame("Giving up on the host.");
	}

	void ForgetMappings()
	{
		ids.Clear();
		lerps.clear();
		clientVfx.clear();
		mismatches = 0;
	}

	// back to the main menu, so nothing keeps running locally without the host
	void LeaveGame(const std::string& why)
	{
		Log(stopped ? ERROR : MESSAGE, "Net", "{}", why);
		bool hadGame = core->GetGame() != nullptr;
		// a load that hasn't happened yet is called off
		core->QuitFlag &= ~(QF_LOADGAME | QF_ENTERGAME);
		if (hadGame) {
			core->QuitFlag |= QF_QUITGAME;
			core->SetNextScript("Start");
			Log(MESSAGE, "Net", "Leaving the game, back to the main menu.");
		}
		state = ClientState::Connecting;
		receiver.Reset();
		loadingTransfer = 0;
		haveTable = false;
		table = NetActorTable();
		ForgetMappings();
		if (!stopped) {
			Log(MESSAGE, "Net", "Will keep trying to reach the host.");
		}
	}

	path_t WriteReceivedSave(uint32_t transferID, const std::vector<NetFile>& files) const
	{
		// alternate between two dirs, as the running game may still read the previous save
		path_t dir = PathJoin(transferDir, transferID % 2 ? "in1" : "in0");
		if (!MakeDirectories(dir)) {
			Log(ERROR, "Net", "Unable to create the transfer directory '{}'.", dir);
			return {};
		}
		DelTree(dir, false);
		for (const auto& file : files) {
			FileStream fs;
			if (!fs.Create(PathJoin(dir, file.name)) || fs.Write(file.data.data(), file.data.size()) != strret_t(file.data.size())) {
				Log(ERROR, "Net", "Unable to write '{}' into '{}'.", file.name, dir);
				return {};
			}
		}
		return dir;
	}

	void LoadReceivedSave()
	{
		const NetSaveBegin& info = receiver.Info();
		Log(MESSAGE, "Net", "Received save #{}: {} bytes, hash {:016x} verified.", info.transferID, receiver.Received(), info.hash);

		std::vector<NetFile> files;
		if (!NetUnpackFiles(receiver.Data().data(), receiver.Data().size(), files)) {
			Log(ERROR, "Net", "The save bundle is corrupt.");
			FailTransfer();
			return;
		}
		path_t dir = WriteReceivedSave(info.transferID, files);
		if (dir.empty()) {
			FailTransfer();
			return;
		}

		// the same steps as loading from the menu: drop the running game, load, enter
		// local scripts never run in between, since clients don't simulate
		Log(MESSAGE, "Net", "Loading the host's game ({} files from '{}')...", files.size(), dir);
		auto save = MakeHolder<SaveGame>(dir, "network", core->GameNameResRef, "network", 0, 0);
		if (core->GetGame()) {
			core->QuitFlag |= QF_QUITGAME;
		}
		core->SetupLoadGame(save, GAMVersion::GemRB);
		core->QuitFlag |= QF_ENTERGAME;

		loadingTransfer = info.transferID;
		haveTable = false;
		ForgetMappings();
		state = ClientState::Loading;
		receiver.Reset();
	}

	// once the engine has loaded and entered the save, pair our actors and objects with the host's
	void FinishLoad()
	{
		if (!haveTable || !GameReady()) return;

		std::vector<NetActorTableEntry> local = ListActors();
		auto matches = NetMatchActors(table.actors, local, MATCH_DISTANCE);
		for (const auto& match : matches) {
			ids.Add(match.first, match.second);
		}

		std::map<ieDword, const NetActorTableEntry*> localByID;
		for (const auto& entry : local) {
			localByID[entry.netID] = &entry;
		}
		NetReady ready;
		size_t logged = 0;
		size_t objects = 0;
		for (const auto& entry : table.actors) {
			uint32_t localID = ids.Local(entry.netID);
			if (entry.kind != NetObjectKind::Actor) {
				if (localID) {
					objects++;
				} else {
					Log(WARNING, "Net", "Host {} {} '{}' in {} has no local match.", KindName(entry.kind), entry.netID, entry.scriptName, entry.area);
				}
				continue;
			}
			if (!localID) {
				Log(WARNING, "Net", "Host actor {} '{}' in {} at ({}, {}) has no local match, asking for it.", entry.netID, entry.scriptName, entry.area, entry.x, entry.y);
				if (ready.unmatched.size() < NET_MAX_ENTRIES && !entry.partySlot) {
					ready.unmatched.push_back(entry.netID);
				}
				continue;
			}
			if (logged++ >= MAX_LOGGED_ACTORS) continue;
			const NetActorTableEntry* mine = localByID[localID];
			Log(MESSAGE, "Net", "Mapped net id {} '{}' party slot {} -> local id {} at ({}, {}), host had ({}, {})", entry.netID, entry.scriptName, entry.partySlot, localID, mine->x, mine->y, entry.x, entry.y);
		}
		// whatever the host doesn't have, doesn't exist
		for (const auto& entry : local) {
			if (entry.kind != NetObjectKind::Actor || entry.partySlot || ids.Net(entry.netID)) continue;
			Actor* extra = core->GetGame()->GetActorByGlobalID(entry.netID);
			if (!extra) continue;
			Log(WARNING, "Net", "Local actor {} '{}' in {} has no host counterpart, removing it.", entry.netID, entry.scriptName, entry.area);
			extra->DestroySelf();
		}
		Log(MESSAGE, "Net", "Mapped {} of {} host actors and objects ({} objects; {} local).", matches.size(), table.actors.size(), objects, local.size());

		ready.transferID = table.transferID;
		ready.mapped = uint16_t(std::min<size_t>(matches.size(), 0xffff));
		Send(server, ready);

		state = ClientState::InSync;
		syncedTicks = 0;
		haveTable = false;
		table = NetActorTable();
		Log(MESSAGE, "Net", "In sync with the host: save #{} loaded, replica mode on.", ready.transferID);
		ReportTestVar("client after load");
	}

	void ClientPreTick()
	{
		if (state == ClientState::Loading) {
			FinishLoad();
		}
		if (state != ClientState::InSync) return;
		syncedTicks++;
		testTicks++;
		if (syncedTicks == TEST_HOOK_DELAY) {
			RunTestMove();
		}
		RunTestOrders();
		if (testResync && syncedTicks == unsigned(testResync)) {
			testResync = 0;
			Log(MESSAGE, "Net", "Test hook: asking the host for a resync.");
			RequestResync(NetResyncRequest::MANUAL);
		}
	}

	bool RequestResync(uint8_t reason)
	{
		if (mode != Mode::Client || !server || state != ClientState::InSync) return false;
		NetResyncRequest request;
		request.reason = reason;
		Send(server, request);
		// stay in sync until the new save arrives, so the game keeps mirroring the host meanwhile
		return true;
	}

	void ApplyState(const NetActorState& snapshot)
	{
		// the channel is sequenced already, but resets (reconnects) can replay old numbers
		if (snapshot.tick <= lastSnapshot && lastSnapshot - snapshot.tick < 1000) return;
		lastSnapshot = snapshot.tick;
		if (state != ClientState::InSync) return;

		// how far apart snapshots arrive, to stretch the smoothing over
		tick_t now = GetMilliseconds();
		if (lastStateAt) {
			uint32_t gap = uint32_t(std::min<tick_t>(now - lastStateAt, 250));
			stateInterval = (stateInterval * 7 + gap) / 8;
		}
		lastStateAt = now;

		Map* map = FindLoadedMap(snapshot.area);
		if (!map) return;
		for (const auto& entry : snapshot.actors) {
			ieDword localID = ids.Local(entry.netID);
			Actor* actor = localID ? map->GetActorByGlobalID(localID) : nullptr;
			if (!actor) continue;
			ApplyActor(actor, entry, now);
			if (snapshot.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Sync check #{}: net id {} (local {}) at {} stance {} hp {}/{} state {:#x}", snapshot.tick, entry.netID, localID, actor->Pos, actor->GetStance(), actor->GetStat(IE_HITPOINTS), actor->GetStat(IE_MAXHITPOINTS), actor->GetStat(IE_STATE_ID));
			}
		}
		if (snapshot.flags & NetActorState::CHECKSUM) {
			CompareChecksum(map, snapshot);
		}
		if (snapshot.tick % SYNC_CHECK_INTERVAL == 0 && smoothedFrames) {
			Log(MESSAGE, "Net", "Smoothing: {} actor steps over {} drawn frames since the last check, snapshots every ~{} ms.", smoothedMoves, smoothedFrames, stateInterval);
			smoothedMoves = smoothedFrames = 0;
		}
	}

	void ApplyActor(Actor* actor, const NetActorStateEntry& entry, tick_t now)
	{
		ieDword localID = actor->GetGlobalID();
		if (entry.fields & NAF_POS) {
			Point pos(entry.x, entry.y);
			auto it = lerps.find(localID);
			Point target = it == lerps.end() ? actor->Pos : Point(it->second.toX, it->second.toY);
			if (pos != target) {
				if (Distance(actor->Pos, pos) > SNAP_DISTANCE) {
					lerps.erase(localID);
					actor->MoveTo(pos);
				} else {
					NetLerp lerp;
					lerp.fromX = actor->Pos.x;
					lerp.fromY = actor->Pos.y;
					lerp.toX = pos.x;
					lerp.toY = pos.y;
					lerp.start = now;
					lerp.duration = stateInterval;
					lerps[localID] = lerp;
				}
			}
			LogMovement("client", entry);
		}
		if (entry.fields & NAF_ORIENT) {
			actor->SetOrientation(orient_t(entry.orientation & (MAX_ORIENT - 1)), false);
		}
		if (entry.fields & NAF_STANCE && actor->GetStance() != entry.stance && entry.stance < MAX_ANIMS) {
			// direct, so the client doesn't pick its own random swing or play armour sounds
			unsigned char old = actor->GetStance();
			actor->SetStanceDirect(entry.stance);
			RestartStance(actor);
			// the ones that matter in a fight: swings, shots, casting, hits, deaths
			static const std::set<unsigned char> notable = { IE_ANI_ATTACK, IE_ANI_CAST, IE_ANI_CONJURE, IE_ANI_DAMAGE, IE_ANI_DIE, IE_ANI_SHOOT, IE_ANI_TWITCH, IE_ANI_ATTACK_SLASH, IE_ANI_ATTACK_BACKSLASH, IE_ANI_ATTACK_JAB };
			if (notable.count(entry.stance)) {
				Log(MESSAGE, "Net", "Net id {} ({}) stance {} -> {}.", entry.netID, fmt::WideToChar { actor->GetShortName() }, old, entry.stance);
			}
		}
		if (entry.fields & NAF_HP) {
			ieDword oldHP = actor->GetStat(IE_HITPOINTS);
			actor->SetStat(IE_MAXHITPOINTS, ieDword(entry.maxHP), 0);
			// no post change function: that would let the client decide a death
			actor->SetBaseNoPCF(IE_HITPOINTS, ieDword(entry.hp));
			actor->SetStat(IE_HITPOINTS, ieDword(entry.hp), 0);
			if (oldHP != ieDword(entry.hp)) {
				if (actor->InParty) core->SetEventFlag(EF_PORTRAIT);
				Log(MESSAGE, "Net", "Net id {} hit points {} -> {} of {}.", entry.netID, int(oldHP), entry.hp, entry.maxHP);
			}
		}
		if (entry.fields & NAF_STATE) {
			ieDword oldState = actor->GetStat(IE_STATE_ID);
			actor->SetBaseNoPCF(IE_STATE_ID, entry.state);
			actor->SetStat(IE_STATE_ID, entry.state, 0);
			if ((entry.state & STATE_DEAD) && !(oldState & STATE_DEAD)) {
				Log(MESSAGE, "Net", "Net id {} ({}) died.", entry.netID, fmt::WideToChar { actor->GetShortName() });
				core->GetGame()->SelectActor(actor, false, SELECT_NORMAL);
			} else if (!(entry.state & STATE_DEAD) && (oldState & STATE_DEAD)) {
				Log(MESSAGE, "Net", "Net id {} ({}) lives again.", entry.netID, fmt::WideToChar { actor->GetShortName() });
			}
			if (oldState != entry.state && actor->InParty) core->SetEventFlag(EF_PORTRAIT);
		}
	}

	void CompareChecksum(const Map* map, const NetActorState& snapshot)
	{
		size_t count = 0;
		uint64_t mine = Checksum(map, count);
		if (mine == snapshot.checksum) {
			if (mismatches || snapshot.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Checksum #{} of {}: {:016x} over {} actors and objects, matches the host.", snapshot.tick, snapshot.area, mine, count);
			}
			mismatches = 0;
			checksumsOk++;
			return;
		}
		mismatches++;
		Log(WARNING, "Net", "Checksum #{} of {}: ours {:016x} over {} actors and objects, the host's {:016x} ({} mismatch(es) in a row).", snapshot.tick, snapshot.area, mine, count, snapshot.checksum, mismatches);
		if (mismatches < DESYNC_MISMATCHES) return;
		tick_t now = GetMilliseconds();
		if (lastDesyncResync && now - lastDesyncResync < DESYNC_COOLDOWN) return;
		lastDesyncResync = now;
		mismatches = 0;
		Log(WARNING, "Net", "Out of sync with the host, asking for a resync.");
		RequestResync(NetResyncRequest::DESYNC);
	}

	// per drawn frame: walk actors along to their latest snapshot position
	void Frame()
	{
		if (mode != Mode::Client || state != ClientState::InSync || lerps.empty()) return;
		const Game* game = core->GetGame();
		if (!game) return;
		tick_t now = GetMilliseconds();
		for (auto it = lerps.begin(); it != lerps.end();) {
			Actor* actor = game->GetActorByGlobalID(it->first);
			if (!actor || !actor->GetCurrentArea()) {
				it = lerps.erase(it);
				continue;
			}
			Point pos;
			bool done = NetInterpolate(it->second, now, pos.x, pos.y);
			if (pos != actor->Pos) {
				actor->MoveTo(pos);
				smoothedMoves++;
			}
			it = done ? lerps.erase(it) : std::next(it);
		}
		smoothedFrames++;
	}

	void ApplySpawn(const NetSpawn& msg)
	{
		if (ids.Local(msg.netID)) {
			Log(MESSAGE, "Net", "Spawn of net id {}: already here.", msg.netID);
			return;
		}
		Map* map = FindLoadedMap(msg.area);
		if (!map) {
			Log(WARNING, "Net", "Spawn of net id {} in {}, which isn't loaded here.", msg.netID, msg.area);
			return;
		}
		Actor* actor = ReadCreature(msg.creature);
		if (!actor) {
			Log(ERROR, "Net", "Spawn of net id {}: unreadable creature ({} bytes).", msg.netID, msg.creature.size());
			return;
		}
		// the steps of CreateCreature
		if (!msg.scriptName.empty()) {
			actor->SetScriptName(ieVariable(msg.scriptName.c_str()));
		}
		map->AddActor(actor, true);
		actor->SetPosition(Point(msg.x, msg.y), false);
		actor->SetOrientation(orient_t(msg.orientation & (MAX_ORIENT - 1)), false);
		ids.Add(msg.netID, actor->GetGlobalID());
		Log(MESSAGE, "Net", "Spawned net id {} '{}' ({}) in {} at {} as local id {}.", msg.netID, actor->GetScriptName(), fmt::WideToChar { actor->GetShortName() }, msg.area, actor->Pos, actor->GetGlobalID());
	}

	void ApplyDespawn(const NetDespawn& msg)
	{
		ieDword localID = ids.Local(msg.netID);
		Actor* actor = localID ? core->GetGame()->GetActorByGlobalID(localID) : nullptr;
		ids.RemoveNet(msg.netID);
		lerps.erase(localID);
		if (!actor) {
			Log(MESSAGE, "Net", "Despawn of net id {}: not here.", msg.netID);
			return;
		}
		// the engine's own removal path, at the next queue rebuild
		actor->DestroySelf();
		Log(MESSAGE, "Net", "Despawned net id {} (local id {}).", msg.netID, localID);
	}

	Actor* LocalActor(uint32_t netID) const
	{
		ieDword localID = ids.Local(netID);
		return localID ? core->GetGame()->GetActorByGlobalID(localID) : nullptr;
	}

	void ApplyProjectile(const NetProjectile& msg)
	{
		const Actor* caster = LocalActor(msg.casterID);
		Map* map = caster ? caster->GetCurrentArea() : core->GetGame()->GetCurrentArea();
		if (!map) return;
		auto pro = core->GetProjectileServer()->GetProjectileByIndex(msg.index);
		if (!pro) {
			Log(WARNING, "Net", "Projectile '{}' (#{}) is unknown here.", msg.name, msg.index);
			return;
		}
		// purely a show: the host applies the payload, and sends explosions, secondary
		// projectiles, effects and its sounds as events of their own
		pro->Extension = nullptr;
		pro->FiringSound.Reset();
		pro->ArrivalSound.Reset();
		pro->successSpell.Reset();
		pro->failureSpell.Reset();
		pro->StrRef = ieStrRef::INVALID;
		pro->SetCaster(caster ? caster->GetGlobalID() : 0, 0);

		Point src(msg.x, msg.y);
		const Actor* target = msg.flags & NetProjectile::AT_POINT ? nullptr : LocalActor(msg.targetID);
		if (target) {
			map->AddProjectile(std::move(pro), src, target->GetGlobalID(), msg.flags & NetProjectile::FAKE_TARGET);
		} else {
			map->AddProjectile(std::move(pro), src, Point(msg.tx, msg.ty));
		}
		Log(MESSAGE, "Net", "Projectile '{}' from net id {} to {} at ({}, {}).", msg.name, msg.casterID, target ? fmt::format("net id {}", msg.targetID) : "point", msg.tx, msg.ty);
	}

	void ApplyVfx(const NetVfx& msg)
	{
		if (msg.flags & NetVfx::END) {
			auto it = clientVfx.find(msg.key);
			if (it == clientVfx.end()) return;
			Actor* actor = core->GetGame()->GetActorByGlobalID(it->second.actorID);
			// it may have ended on its own already, so only touch it if it's still there
			if (actor) {
				for (const auto& vvc : actor->vfxQueue) {
					if (vvc.get() == it->second.vvc) {
						vvc->active = false;
						Log(MESSAGE, "Net", "Vfx #{} '{}' ended.", msg.key, vvc->ResName);
						break;
					}
				}
			}
			clientVfx.erase(it);
			return;
		}

		ResRef resource(msg.resource.c_str());
		if (msg.flags & NetVfx::COMPOSITE) {
			auto vef = gamedata->GetVEFObject(resource, false);
			Map* map = core->GetGame()->GetCurrentArea();
			if (!vef || !map) return;
			vef->Pos = Point(msg.x, msg.y);
			map->AddVVCell(std::move(vef));
			Log(MESSAGE, "Net", "Vfx #{} '{}' (composite) on the area at ({}, {}).", msg.key, msg.resource, msg.x, msg.y);
			return;
		}
		auto sca = gamedata->GetScriptedAnimation(resource, msg.flags & NetVfx::TWIN);
		if (!sca) {
			Log(WARNING, "Net", "Vfx '{}' is unknown here.", msg.resource);
			return;
		}
		sca->Tint = Color(msg.tint);
		sca->Transparency = msg.transparency;
		sca->SequenceFlags = msg.sequenceFlags;
		sca->XOffset = msg.xOffset;
		sca->YOffset = msg.yOffset;
		sca->ZOffset = msg.zOffset;
		sca->Duration = msg.duration;
		sca->SetOrientation(orient_t(msg.orientation & (MAX_ORIENT - 1)));
		// play-once effects on the host had their loop cleared; the file says otherwise
		if (!(msg.sequenceFlags & IE_VVC_LOOP)) {
			sca->PlayOnce();
		}
		if (msg.flags & NetVfx::ON_AREA) {
			Map* map = core->GetGame()->GetCurrentArea();
			if (!map) return;
			sca->Pos = Point(msg.x, msg.y);
			map->AddVVCell(std::move(sca));
			Log(MESSAGE, "Net", "Vfx #{} '{}' on the area at ({}, {}).", msg.key, msg.resource, msg.x, msg.y);
			return;
		}
		Actor* actor = LocalActor(msg.actorID);
		if (!actor) return;
		clientVfx[msg.key] = ClientVfx { actor->GetGlobalID(), sca.get() };
		actor->AddVVCell(std::move(sca));
		Log(MESSAGE, "Net", "Vfx #{} '{}' on net id {}{}.", msg.key, msg.resource, msg.actorID, msg.flags & NetVfx::LOOPING ? ", looping" : "");
	}

	static void ApplySound(const NetSound& msg)
	{
		AudioPlaybackConfig config;
		config.spatial = msg.flags & NetSound::SPATIAL;
		config.position = AudioPoint(msg.x, msg.y, msg.z);
		config.masterVolume = msg.masterVolume;
		config.channelVolume = msg.channelVolume;
		config.muteDistance = msg.muteDistance;
		StringView resource(msg.resource);
		if (msg.flags & NetSound::SPEECH) {
			core->GetAudioPlayback().PlaySpeech(resource, config);
		} else {
			core->GetAudioPlayback().Play(resource, config);
		}
	}

	void ApplyText(const NetText& msg)
	{
		String text = StringFromUtf8(msg.text.c_str());
		if (msg.kind == NetText::FEEDBACK) {
			displaymsg->DisplayMarkupString(std::move(text));
			Log(MESSAGE, "Net", "Feedback: '{}'", PlainText(msg.text));
			return;
		}
		Scriptable* owner = FindScriptable(ids.Local(msg.ownerID));
		if (!owner) return;
		owner->overHead.SetText(std::move(text), true, true, Color(msg.color));
		Log(MESSAGE, "Net", "Overhead text on net id {}: '{}'", msg.ownerID, msg.text);
	}

	void ApplyObjectState(const NetObjectState& msg)
	{
		Scriptable* obj = FindScriptable(ids.Local(msg.netID));
		if (!obj) {
			Log(WARNING, "Net", "State of {} net id {}: not here.", KindName(msg.kind), msg.netID);
			return;
		}
		uint32_t before = 0;
		if (Door* door = Scriptable::As<Door>(obj)) {
			before = door->Flags;
			bool open = bool(msg.flags & DOOR_OPEN) != core->HasFeature(GFFlags::REVERSE_DOOR);
			if (bool(door->IsOpen()) != open) {
				// no sound (the host's comes as an event), no triggers (no scripts run here)
				door->SetDoorOpen(open, false, 0, false);
			}
			door->Flags = msg.flags;
			door->TrapDetected = msg.trapDetected;
			door->Trapped = msg.trapped;
		} else if (Container* container = Scriptable::As<Container>(obj)) {
			before = container->Flags;
			container->Flags = msg.flags;
			container->TrapDetected = msg.trapDetected;
			container->Trapped = msg.trapped;
		} else if (InfoPoint* ip = Scriptable::As<InfoPoint>(obj)) {
			before = ip->Flags;
			ip->Flags = msg.flags;
			ip->TrapDetected = msg.trapDetected;
			ip->Trapped = msg.trapped;
		}
		Log(MESSAGE, "Net", "{} '{}' (net id {}) flags {:#x} -> {:#x}, trap detected {} armed {}, host has {} items.", KindName(msg.kind), obj->GetScriptName(), msg.netID, before, msg.flags, msg.trapDetected, msg.trapped, msg.items);
	}

	void ApplyPause(const NetPause& msg)
	{
		applyingPause = true;
		core->SetPause(msg.on ? PauseState::On : PauseState::Off);
		applyingPause = false;
		Log(MESSAGE, "Net", "The host {} the game (asked by slot {}).", msg.on ? "paused" : "unpaused", msg.slot);
	}

	void ApplyOwnership(const NetOwnership& msg)
	{
		myOwners.clear();
		std::string mine;
		for (const auto& entry : msg.entries) {
			myOwners[entry.netID] = entry.owner;
			if (entry.owner == slot) {
				mine += fmt::format(" {}(net id {})", entry.partySlot, entry.netID);
			}
		}
		Log(MESSAGE, "Net", "We control party slot(s):{}.", mine.empty() ? " none" : mine);
	}

	// orders ----------------------------------------------------------------

	bool TranslateObject(NetActionObject& object) const
	{
		if (!IsDirectTarget(object)) return true;
		uint32_t netID = ids.Net(ieDword(object.fields[1]));
		if (!netID) return false;
		object.fields[1] = int32_t(netID);
		return true;
	}

	bool ForwardAction(const Actor& actor, const Action& action, uint8_t flags)
	{
		if (state != ClientState::InSync || !server) return false;
		NetCommand cmd;
		cmd.actorID = ids.Net(actor.GetGlobalID());
		if (!cmd.actorID) {
			Log(WARNING, "Net", "Not forwarding an order: actor {} has no network ID.", actor.GetGlobalID());
			return false;
		}
		cmd.flags = flags;
		cmd.action = ActionName(action.actionID);
		cmd.int0 = action.int0Parameter;
		cmd.int1 = action.int1Parameter;
		cmd.int2 = action.int2Parameter;
		cmd.x = action.pointParameter.x;
		cmd.y = action.pointParameter.y;
		cmd.string0 = action.string0Parameter.c_str();
		cmd.string1 = action.string1Parameter.c_str();
		cmd.actionFlags = action.flags;
		for (size_t i = 0; i < NET_ACTION_OBJECTS; ++i) {
			const Holder<Object>& src = action.objects[i];
			if (!src) continue;
			NetActionObject& object = cmd.objects[i];
			std::copy(std::begin(src->objectFields), std::end(src->objectFields), object.fields.begin());
			std::copy(std::begin(src->objectFilters), std::end(src->objectFilters), object.filters.begin());
			object.rect = { src->objectRect.x, src->objectRect.y, src->objectRect.w, src->objectRect.h };
			object.name = src->objectName.c_str();
			if (!TranslateObject(object)) {
				Log(WARNING, "Net", "Not forwarding '{}': its target {} has no network ID.", cmd.action, src->objectFields[1]);
				return false;
			}
			cmd.objectMask |= uint8_t(1 << i);
		}
		// the door toggle carries the door's ID as its parameter
		if (cmd.action == "nidspecial9") {
			cmd.int0 = int32_t(ids.Net(ieDword(cmd.int0)));
			if (!cmd.int0) return false;
		}
		Send(server, cmd);
		std::string target = cmd.objectMask & 2 && IsDirectTarget(cmd.objects[1]) ? fmt::format(" on net id {}", cmd.objects[1].fields[1]) : "";
		Log(MESSAGE, "Net", "Forwarded '{}'{}{} for net id {}.", cmd.action, cmd.string0.empty() ? "" : fmt::format(" ({})", cmd.string0), target, cmd.actorID);
		return true;
	}

	bool ForwardUseExit(const Actor& actor, const Scriptable& exit)
	{
		if (state != ClientState::InSync || !server) return false;
		NetCommand cmd;
		cmd.actorID = ids.Net(actor.GetGlobalID());
		cmd.action = "useexit";
		cmd.int0 = int32_t(ids.Net(exit.GetGlobalID()));
		if (!cmd.actorID || !cmd.int0) return false;
		Send(server, cmd);
		Log(MESSAGE, "Net", "Forwarded UseExit('{}') for net id {}.", exit.GetScriptName(), cmd.actorID);
		return true;
	}

	// test hooks -------------------------------------------------------------

	// NetTestMove=x.y[;x.y...]: issue one party move through GameControl, like a click would;
	// clients use the next point after each (re)sync
	void RunTestMove()
	{
		if (testMove.empty()) return;
		size_t end = testMove.find(';');
		std::string spec = testMove.substr(0, end);
		testMove.erase(0, end == std::string::npos ? end : end + 1);

		Point target;
		if (sscanf(spec.c_str(), "%d.%d", &target.x, &target.y) != 2) {
			Log(WARNING, "Net", "Ignoring malformed NetTestMove '{}'.", spec);
			return;
		}
		Game* game = core->GetGame();
		GameControl* gc = core->GetGameControl();
		if (!game || !gc) return;
		if (mode == Mode::Client) {
			// only our own party members take orders from us
			SelectMine();
		} else if (game->selected.empty()) {
			game->SelectActor(game->FindPC(1), true, SELECT_NORMAL);
		}
		Log(MESSAGE, "Net", "Test hook: ordering the selected party to {}.", target);
		gc->CommandSelectedMovement(target);
	}

	// NetTestVar=NAME=value: the host changes a global before anyone joins
	void RunTestVar() const
	{
		size_t eq = testVar.find('=');
		if (eq == std::string::npos) return;
		std::string var = testVar.substr(0, eq);
		ieDword value = ieDword(atoi(testVar.c_str() + eq + 1));
		SetVariable(core->GetGame(), StringParam(var), value, "GLOBAL");
		Log(MESSAGE, "Net", "Test hook: set global {} to {}.", var, value);
	}

	static Actor* FindActorNamed(const std::string& name)
	{
		const Game* game = core->GetGame();
		Map* map = game ? game->GetCurrentArea() : nullptr;
		return map ? map->GetActorByScriptName(ieVariable(name.c_str())) : nullptr;
	}

	Actor* FirstSelected() const
	{
		const Game* game = core->GetGame();
		return game->selected.empty() ? nullptr : game->selected[0];
	}

	void SelectMine() const
	{
		Game* game = core->GetGame();
		SelectOnly(nullptr);
		for (const auto& entry : myOwners) {
			Actor* mine = entry.second == slot ? LocalActor(entry.first) : nullptr;
			if (mine) game->SelectActor(mine, true, SELECT_NORMAL);
		}
	}

	void SelectOnly(Actor* actor) const
	{
		Game* game = core->GetGame();
		game->SelectActor(nullptr, false, SELECT_NORMAL);
		if (actor) game->SelectActor(actor, true, SELECT_NORMAL);
	}

	// NetTestOrders=T:order;T:order...: at tick T (since syncing, or since the host's game started)
	// give an order through the same GameControl calls a click makes. Orders:
	// select N | selectall | selectmine | move X.Y | attack NAME | cast SPELL NAME | door NAME | pick NAME |
	// travel REGION | pause | unpause | desync | resync | chat TEXT | spawn CRE X.Y [NAME] | remove NAME|spawned |
	// jump N X.Y | say NAME TEXT | toggledoor NAME | owner N SLOT
	void RunTestOrders()
	{
		size_t start = 0;
		while (start < testOrders.size()) {
			size_t end = testOrders.find(';', start);
			if (end == std::string::npos) end = testOrders.size();
			std::string spec = testOrders.substr(start, end - start);
			start = end + 1;
			size_t colon = spec.find(':');
			if (colon == std::string::npos || unsigned(atoi(spec.c_str())) != testTicks) continue;
			RunTestOrder(spec.substr(colon + 1));
		}
	}

	void RunTestOrder(const std::string& order)
	{
		Game* game = core->GetGame();
		GameControl* gc = core->GetGameControl();
		if (!game || !gc) return;
		std::string verb = order.substr(0, order.find(' '));
		std::string arg = order.size() > verb.size() ? order.substr(verb.size() + 1) : "";
		std::string arg2 = arg.substr(arg.find(' ') == std::string::npos ? arg.size() : arg.find(' ') + 1);
		std::string arg1 = arg.substr(0, arg.find(' '));
		Log(MESSAGE, "Net", "Test order at tick {}: {}", testTicks, order);

		Actor* selected = FirstSelected();
		if (verb == "select") {
			SelectOnly(game->FindPC(unsigned(atoi(arg.c_str()))));
		} else if (verb == "selectall") {
			game->SelectActor(nullptr, true, SELECT_NORMAL);
		} else if (verb == "selectmine") {
			SelectMine();
		} else if (verb == "move") {
			Point p;
			if (sscanf(arg.c_str(), "%d.%d", &p.x, &p.y) == 2) gc->CommandSelectedMovement(p);
		} else if (verb == "attack") {
			const Actor* target = FindActorNamed(arg);
			if (selected && target) gc->TryToAttack(selected, target);
		} else if (verb == "cast") {
			TestCast(gc, selected, ResRef(arg1.c_str()), FindActorNamed(arg2));
		} else if (verb == "door") {
			Door* door = game->GetCurrentArea()->GetTileMap()->GetDoor(ieVariable(arg.c_str()));
			if (selected && door) gc->HandleDoor(door, selected);
		} else if (verb == "pick") {
			const Scriptable* target = game->GetCurrentArea()->GetTileMap()->GetDoor(ieVariable(arg.c_str()));
			if (!target) target = game->GetCurrentArea()->GetTileMap()->GetContainer(ieVariable(arg.c_str()));
			if (selected && target) gc->TryToPick(selected, target);
		} else if (verb == "travel") {
			TestTravel(gc, arg);
		} else if (verb == "pause" || verb == "unpause") {
			core->SetPause(verb == "pause" ? PauseState::On : PauseState::Off);
		} else if (verb == "desync") {
			TestDesync();
		} else if (verb == "resync") {
			RequestResync(NetResyncRequest::MANUAL);
		} else if (verb == "chat") {
			SendChat(arg);
		} else if (verb == "spawn") {
			// spawn CRE X.Y [NAME], what CreateCreature does
			Point p;
			Actor* actor = gamedata->GetCreature(ResRef(arg1.c_str()));
			if (!actor || sscanf(arg2.c_str(), "%d.%d", &p.x, &p.y) != 2) return;
			game->GetCurrentArea()->AddActor(actor, true);
			actor->SetPosition(p, false);
			size_t space = arg2.find(' ');
			if (space != std::string::npos) actor->SetScriptName(ieVariable(arg2.substr(space + 1).c_str()));
			testSpawned = actor->GetGlobalID();
		} else if (verb == "remove") {
			Actor* actor = arg == "spawned" ? game->GetActorByGlobalID(testSpawned) : FindActorNamed(arg);
			if (actor) actor->DestroySelf();
		} else if (verb == "toggledoor") {
			// a state change made by the simulation itself, eg. a script's OpenDoor
			Door* door = game->GetCurrentArea()->GetTileMap()->GetDoor(ieVariable(arg.c_str()));
			if (!door) return;
			door->SetDoorLocked(false, true);
			door->SetDoorOpen(!door->IsOpen(), true, 0);
		} else if (verb == "jump") {
			// JumpToPoint, for actors that can't walk (the demo's CHMB1 animation has no walk cycle)
			Actor* actor = game->FindPC(unsigned(atoi(arg1.c_str())));
			Point p;
			if (actor && sscanf(arg2.c_str(), "%d.%d", &p.x, &p.y) == 2) actor->SetPosition(p, true);
		} else if (verb == "say") {
			// what DisplayStringHead ends up doing
			Actor* actor = FindActorNamed(arg1);
			if (actor) actor->overHead.SetText(StringFromUtf8(arg2.c_str()), true, true, ColorWhite);
		} else if (verb == "owner") {
			ownerOverride[uint8_t(atoi(arg1.c_str()))] = uint8_t(atoi(arg2.c_str()));
		} else {
			Log(WARNING, "Net", "Unknown test order '{}'.", order);
		}
	}

	static void TestCast(GameControl* gc, Actor* caster, const ResRef& spell, const Actor* target)
	{
		if (!caster || !target) return;
		for (int type = 0; type < caster->spellbook.GetTypes(); ++type) {
			for (unsigned int level = 0; level < caster->spellbook.GetSpellLevelCount(type); ++level) {
				unsigned int count = caster->spellbook.GetMemorizedSpellsCount(type, level, false);
				for (unsigned int idx = 0; idx < count; ++idx) {
					const CREMemorizedSpell* memorized = caster->spellbook.GetMemorizedSpell(type, level, idx);
					if (!memorized || memorized->SpellResRef != spell || !memorized->Flags) continue;
					// what the spell window does for a click on the icon, then the click on the target
					gc->SetupCasting(spell, type, int(level), int(idx), GA_NO_DEAD, 1);
					gc->TryToCast(caster, target);
					return;
				}
			}
		}
		Log(WARNING, "Net", "Test cast: {} has no {} ready.", fmt::WideToChar { caster->GetShortName() }, spell);
	}

	// a click on a travel region: everyone selected may use it, and walks there
	void TestTravel(GameControl* gc, const std::string& region)
	{
		Game* game = core->GetGame();
		const InfoPoint* exit = game->GetCurrentArea()->GetTileMap()->GetInfoPoint(ieVariable(region.c_str()));
		if (!exit) return;
		for (Actor* actor : game->selected) {
			if (mode == Mode::Client) {
				ForwardUseExit(*actor, *exit);
			} else {
				actor->UseExit(exit->GetGlobalID());
			}
		}
		gc->CommandSelectedMovement(exit->Pos);
	}

	// break the local state behind the host's back: a door locks or unlocks only here
	void TestDesync() const
	{
		const Map* map = core->GetGame()->GetCurrentArea();
		for (const auto& door : map->GetTileMap()->GetDoors()) {
			door->Flags ^= DOOR_LOCKED;
			Log(MESSAGE, "Net", "Test hook: flipped the lock of door '{}' locally, flags now {:#x}.", door->GetScriptName(), door->Flags);
			return;
		}
	}
};

std::unique_ptr<NetSession> NetSession::Create(const CoreSettings& config)
{
	if (config.NetMode.empty()) return nullptr;

	Mode mode;
	if (config.NetMode == "host") {
		mode = Mode::Host;
	} else if (config.NetMode == "client") {
		mode = Mode::Client;
	} else {
		Log(ERROR, "Net", "Unknown NetMode '{}', expected host or client. Networking stays off.", config.NetMode);
		return nullptr;
	}
	if (config.NetPort <= 0 || config.NetPort > 65535) {
		Log(ERROR, "Net", "Invalid NetPort {}. Networking stays off.", config.NetPort);
		return nullptr;
	}

	auto impl = std::make_unique<Impl>(mode, config);
	if (!impl->Start()) return nullptr;
	return std::unique_ptr<NetSession>(new NetSession(mode, std::move(impl)));
}

bool NetSession::IsReplica() const
{
	return mode == Mode::Client;
}

bool NetSession::IsInSync() const
{
	return mode == Mode::Client && impl->state == Impl::ClientState::InSync;
}

void NetSession::PreTick()
{
	impl->ticks++;
	impl->Poll();
	if (mode == Mode::Client) {
		impl->ClientPreTick();
	} else {
		// from here until PostTick the simulation runs, and what it shows is replicated
		impl->capturing = true;
		if (impl->GameReady()) {
			// they stand in for player input, so they count as part of the simulation
			impl->RunTestOrders();
		}
	}
}

void NetSession::PostTick()
{
	if (mode == Mode::Host) {
		impl->capturing = false;
		impl->HostPostTick();
	}
	enet_host_flush(impl->host);
}

void NetSession::Frame()
{
	impl->Frame();
}

void NetSession::SendChat(const std::string& text)
{
	impl->SendChat(text);
}

bool NetSession::ForwardAction(const Actor& actor, const Action& action, uint8_t flags)
{
	return impl->ForwardAction(actor, action, flags);
}

bool NetSession::ForwardUseExit(const Actor& actor, const Scriptable& exit)
{
	return impl->ForwardUseExit(actor, exit);
}

bool NetSession::RequestResync()
{
	return impl->RequestResync(NetResyncRequest::MANUAL);
}

void NetSession::OnFeedback(const String& markup)
{
	impl->OnFeedback(markup);
}

void NetSession::OnOverheadText(const Scriptable& owner, const String& text, const Color& color)
{
	impl->OnOverheadText(owner, text, color);
}

void NetSession::OnSound(StringView resource, const AudioPlaybackConfig& config, bool speech)
{
	impl->OnSound(resource, config, speech);
}

void NetSession::OnProjectile(const Map& map, const Projectile& pro, uint32_t targetID, bool fake)
{
	impl->OnProjectile(map, pro, targetID, fake);
}

void NetSession::OnAreaVfx(const Map& map, VEFObject& vvc)
{
	impl->OnAreaVfx(map, vvc);
}

void NetSession::OnPause(bool paused)
{
	impl->OnPause(paused);
}

#else // HAVE_ENET

struct NetSession::Impl {};

std::unique_ptr<NetSession> NetSession::Create(const CoreSettings& config)
{
	if (!config.NetMode.empty()) {
		Log(ERROR, "Net", "NetMode is set, but this build has no networking (ENet wasn't found).");
	}
	return nullptr;
}

bool NetSession::IsReplica() const
{
	return false;
}

bool NetSession::IsInSync() const
{
	return false;
}

void NetSession::PreTick() {}
void NetSession::PostTick() {}
void NetSession::Frame() {}
void NetSession::SendChat(const std::string&) {}

bool NetSession::ForwardAction(const Actor&, const Action&, uint8_t)
{
	return false;
}

bool NetSession::ForwardUseExit(const Actor&, const Scriptable&)
{
	return false;
}

bool NetSession::RequestResync()
{
	return false;
}

void NetSession::OnFeedback(const String&) {}
void NetSession::OnOverheadText(const Scriptable&, const String&, const Color&) {}
void NetSession::OnSound(StringView, const AudioPlaybackConfig&, bool) {}
void NetSession::OnProjectile(const Map&, const Projectile&, uint32_t, bool) {}
void NetSession::OnAreaVfx(const Map&, VEFObject&) {}
void NetSession::OnPause(bool) {}

#endif // HAVE_ENET

NetSession::NetSession(Mode mode, std::unique_ptr<Impl> impl)
	: mode(mode), impl(std::move(impl))
{
}

NetSession::~NetSession() = default;

}
