// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/NetSession.h"

#include "config.h"
#include "ie_stats.h"

#include "ActorMgr.h"
#include "DialogHandler.h"
#include "DisplayMessage.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "InterfaceConfig.h"
#include "Item.h"
#include "Map.h"
#include "MapMgr.h"
#include "PluginMgr.h"
#include "Projectile.h"
#include "ProjectileServer.h"
#include "SaveGame.h"
#include "SaveGameIterator.h"
#include "ScriptEngine.h"
#include "ScriptedAnimation.h"
#include "Store.h"
#include "TileMap.h"
#include "VEFObject.h"

#include "Audio/Playback.h"
#include "Audio/PlaybackConfig.h"
#include "GUI/GameControl.h"
#include "GUI/TextArea.h"
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
#include <random>
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
// stats that snapshots carry already
static const std::vector<uint16_t> SNAPSHOT_STATS = { IE_HITPOINTS, IE_MAXHITPOINTS, IE_STATE_ID };
// dialog replies shown to players who don't own the conversation
static constexpr const char* READONLY_REPLY = "[p][color=A0A0A0FF]   {}. {}[/color][/p]";
// both sides send a heartbeat this often (ms); silence longer than NetTimeout drops the connection
static constexpr tick_t HEARTBEAT_INTERVAL = 1000;
// ENet's own timeout is a little longer than ours, as a backstop
static constexpr tick_t ENET_TIMEOUT_SLACK = 2000;
// lines of chat kept for the lobby window
static constexpr size_t CHAT_LOG_LINES = 50;
// the lobby goes out again this often anyway, for the pings (ms)
static constexpr tick_t LOBBY_REFRESH = 5000;
// "X is controlled by player N" at most this often per party member (ms)
static constexpr tick_t REFUSAL_THROTTLE = 2000;

// one written save, ready to stream to every peer that asked for it in this tick
struct NetSnapshot {
	std::vector<uint8_t> packed;
	NetActorTable table;
	std::string area;
};

struct NetPeerInfo {
	ENetPeer* peer = nullptr;
	uint8_t slot = 0; // from the roster, once it said hello
	std::string name;
	uint64_t token = 0;
	bool welcomed = false;
	// heartbeats: when we last heard from it, its clock for the echo, the round trip
	tick_t lastHeard = 0;
	uint32_t theirTime = 0;
	tick_t theirTimeAt = 0;
	uint16_t rtt = 0;
	// joined or asked for a resync: gets a save at the next safe point
	bool wantsSave = false;
	// the party moved: gets the new area (lighter than a save) at the next safe point
	bool wantsArea = false;
	tick_t areaChangeAt = 0; // for the log
	std::shared_ptr<NetSaveSender> transfer;
	NetActorTable table; // goes out after the last chunk
	// loaded the latest save and reported Ready: gets snapshots and every event
	bool live = false;
	// the save it is loading; state changing events since its snapshot wait in pending
	uint32_t awaitedTransfer = 0;
	std::vector<std::vector<uint8_t>> pending;
	// its GUI state on our side, swapped in while running its GUI calls
	std::unique_ptr<ItemDragOp> dragged;
	ieDword guiActor = 0; // the last party member its GUI calls were about
	ieDword container = 0; // opened container (host global IDs)
	ieDword containerActor = 0;
	ResRef store; // opened store
	ieDword storeActor = 0;
	ieDword storeOwner = 0;
};

// the host's view of the conversation in progress
struct HostDialog {
	bool active = false;
	uint8_t owner = NET_HOST_SLOT;
	uint32_t seq = 0;
	ieDword speakerID = 0;
	ieDword targetID = 0;
};

// and the client's
struct ClientDialog {
	bool active = false;
	bool ui = false; // we own it, so the full dialog GUI is up
	bool freeze = false;
	uint8_t owner = NET_HOST_SLOT;
	uint32_t seq = 0;
	std::vector<NetDialogOption> options;
	uint16_t continueIndex = 0;
	uint8_t buttons = 0; // NetDialog::CONTINUE or END_BUTTON
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
	// heartbeats and timeouts (ms)
	tick_t timeout;
	tick_t nextHeartbeat = 0;
	uint32_t heartbeatSeq = 0;
	// test hook: no polling or heartbeats until then, like a hung process
	tick_t hangUntil = 0;
	// both sides: the lobby as the host has it, and our chat
	NetPhase phase = NetPhase::Game;
	NetLobbyState lobby;
	uint32_t lobbyRevision = 1;
	std::vector<std::string> chatLog;
	std::string status;
	std::map<ieDword, tick_t> refusals; // when we last told our player an actor isn't theirs
	bool localMessage = false; // what we tell our own player isn't replicated
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
	// players by slot and token; who owns which party slot (the lobby's assignments)
	NetRoster roster;
	std::map<uint8_t, uint8_t> assigned; // party slot -> player slot
	NetOwnership owners; // who controls which party member now
	uint64_t lobbyHash = 0;
	tick_t lobbySentAt = 0;
	// party members in slot order, to tell area changes (lighter) from party changes (a save)
	std::vector<ieDword> partyMembers;
	bool partyChanged = false;
	bool lightAreas = true;
	// global variables the clients know of
	std::map<std::string, int32_t> knownGlobals;
	uint8_t pauseRequester = NET_HOST_SLOT;
	std::string lastSound;
	unsigned int lastSoundTick = 0;
	NetPermissions permissions;
	HostDialog dialog;
	// the last conversation, for the actions it runs once it is over (StartStore)
	HostDialog lastDialog;
	// its last NPC line and node, for players who join in the middle of it
	NetDialog dialogLine;
	NetDialog dialogState;
	bool haveDialogLine = false;
	bool haveDialogState = false;
	bool applyingChoice = false;
	// GUI calls of remote players, run in the simulation window of the tick they arrived in
	std::vector<std::pair<uint8_t, NetGuiCall>> guiCalls;
	const NetPeerInfo* guiCaller = nullptr;
	// what the clients know about inventories (actors and containers by global ID), stats and spellbooks
	std::map<ieDword, uint64_t> inventories;
	std::map<ieDword, Point> piles; // ground piles the clients know of, by global ID
	std::map<ieDword, std::vector<NetStatEntry>> stats;
	std::map<ieDword, uint64_t> spellbooks;
	NetPartyState party;
	bool partyDirty = false;
	std::map<std::string, uint64_t> storeHashes; // stores remote players have open

	// client side
	ENetPeer* server = nullptr;
	uint64_t token = 0; // our identity at this host, kept across reconnects
	tick_t lastHeard = 0;
	uint32_t hostTime = 0;
	tick_t hostTimeAt = 0;
	uint16_t rtt = 0;
	NetPermissions myPermissions; // as the lobby says, for the local checks
	tick_t areaChangeAt = 0; // when the host said the party moved, for the log
	bool lightLoad = false; // the area being loaded came as area data, not a save
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
	ClientDialog clientDialog;
	// the host's word on party members' stats, by local ID: stat -> base, modified
	std::map<ieDword, std::map<uint16_t, std::pair<ieDword, ieDword>>> hostStats;
	bool refreshGUI = false;
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

	Impl(Mode mode, const CoreSettings& config, bool inLobby)
		: mode(mode), name(config.NetName), remoteHost(config.NetHost), port(uint16_t(config.NetPort)),
		  timeout(tick_t(std::max(config.NetTimeout, 1000))), lightAreas(config.NetLightAreas != 0),
		  testChat(config.NetTestChat), testMove(config.NetTestMove), testVar(config.NetTestVar),
		  testOrders(config.NetTestOrders), testResync(config.NetTestResync), slowTransfer(config.NetTestSlowTransfer != 0)
	{
		roster.SetHost(name);
		phase = mode == Mode::Host && !inLobby ? NetPhase::Game : NetPhase::Lobby;
		status = mode == Mode::Host ? "Hosting" : "Connecting";
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
		lastHeard = GetMilliseconds();
		if (server) enet_peer_timeout(server, 0, enet_uint32(timeout + ENET_TIMEOUT_SLACK), enet_uint32(timeout + ENET_TIMEOUT_SLACK));
		SetStatus(fmt::format("Connecting to {}:{}...", remoteHost, port));
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

	void SetStatus(const std::string& text)
	{
		if (status == text) return;
		status = text;
		lobbyRevision++;
	}

	void ShowChat(const NetChat& chat)
	{
		Log(MESSAGE, "Net", "Chat <{}#{}>: {}", chat.name, chat.slot, chat.text);
		chatLog.push_back(fmt::format("{}: {}", chat.name, chat.text));
		if (chatLog.size() > CHAT_LOG_LINES) chatLog.erase(chatLog.begin());
		lobbyRevision++;
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
			ShowChat(chat); // the host relays it to everyone else
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
		if (Hanging()) return;
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

	bool Hanging() const
	{
		return hangUntil && GetMilliseconds() < hangUntil;
	}

	void OnConnect(ENetPeer* peer)
	{
		enet_peer_timeout(peer, 0, enet_uint32(timeout + ENET_TIMEOUT_SLACK), enet_uint32(timeout + ENET_TIMEOUT_SLACK));
		if (mode == Mode::Client) {
			Log(MESSAGE, "Net", "Connected to host {}, saying hello{}.", PeerAddress(peer), token ? " (we were here before)" : "");
			lastHeard = GetMilliseconds();
			NetHello hello;
			hello.name = name;
			hello.gameType = core->config.GameType;
			hello.token = token;
			Send(peer, hello);
			SetStatus("Connected, waiting for the host");
			return;
		}

		NetPeerInfo info;
		info.peer = peer;
		info.lastHeard = GetMilliseconds();
		peers.push_back(std::move(info));
		Log(MESSAGE, "Net", "Peer {} connected.", PeerAddress(peer));
	}

	void OnDisconnect(ENetPeer* peer)
	{
		if (mode == Mode::Client) {
			server = nullptr;
			if (state != ClientState::Connecting) {
				LeaveGame("Lost the connection to the host.");
			} else if (!stopped) {
				SetStatus("Could not reach the host, retrying");
				Log(MESSAGE, "Net", "Could not reach the host, retrying in {}s.", RECONNECT_DELAY / 1000);
			}
			nextConnectAttempt = GetMilliseconds() + RECONNECT_DELAY;
			return;
		}

		for (auto it = peers.begin(); it != peers.end(); ++it) {
			if (it->peer != peer) continue;
			RemovePeer(it, "disconnected");
			break;
		}
	}

	// a player is gone (left, dropped or timed out): it keeps its slot for a rejoin, its party
	// members go to the host meanwhile, and nothing it had open keeps the host waiting
	void RemovePeer(std::vector<NetPeerInfo>::iterator it, const char* how)
	{
		NetPeerInfo& info = *it;
		if (info.transfer) {
			Log(MESSAGE, "Net", "Dropped the save transfer to slot {} after {} of {} bytes.", info.slot, info.transfer->Sent(), info.transfer->Begin().size);
		}
		if (!info.welcomed) {
			Log(MESSAGE, "Net", "Peer {} {} before saying hello.", PeerAddress(info.peer), how);
			peers.erase(it);
			return;
		}
		Log(MESSAGE, "Net", "Player '{}' (slot {}) {}; its slot and characters are kept for a rejoin.", info.name, info.slot, how);
		DropDragged(info);
		if (!info.store.IsEmpty()) {
			Log(MESSAGE, "Net", "Slot {} was trading at {}: the store is closed for it.", info.slot, info.store);
		}
		if (info.container) {
			Log(MESSAGE, "Net", "Slot {} had container {} open: closed.", info.slot, info.container);
		}
		uint8_t slot = info.slot;
		roster.Leave(slot);
		peers.erase(it);
		if (dialog.active && dialog.owner == slot) {
			HandDialogToHost(slot);
		}
		if (core->GetGame()) {
			displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format("[p][color=FFC080FF]Player {} left; its characters are yours until it returns.[/color][/p]", slot).c_str()));
		}
	}

	// heartbeats both ways; silence longer than the timeout drops the connection
	void Heartbeats()
	{
		if (Hanging()) return;
		tick_t now = GetMilliseconds();
		if (mode == Mode::Host) {
			for (auto it = peers.begin(); it != peers.end();) {
				if (now - it->lastHeard <= timeout) {
					++it;
					continue;
				}
				Log(WARNING, "Net", "Player '{}' (slot {}) timed out: nothing for {} ms.", it->name, it->slot, now - it->lastHeard);
				ENetPeer* peer = it->peer;
				RemovePeer(it, "timed out");
				enet_peer_reset(peer);
				it = peers.begin();
			}
		} else if (server && now - lastHeard > timeout) {
			Log(WARNING, "Net", "The host timed out: nothing for {} ms.", now - lastHeard);
			enet_peer_reset(server);
			server = nullptr;
			if (state != ClientState::Connecting) {
				LeaveGame("Lost the connection to the host (timed out).");
			}
			SetStatus("The host timed out, retrying");
			nextConnectAttempt = now + RECONNECT_DELAY;
		}

		if (now < nextHeartbeat) return;
		nextHeartbeat = now + HEARTBEAT_INTERVAL;
		NetHeartbeat beat;
		beat.seq = ++heartbeatSeq;
		beat.time = uint32_t(now);
		if (mode == Mode::Host) {
			for (const auto& info : peers) {
				if (!info.welcomed) continue;
				// their clock plus how long we held it, so the round trip leaves that out
				beat.echo = info.theirTime ? info.theirTime + uint32_t(now - info.theirTimeAt) : 0;
				beat.rtt = info.rtt;
				Send(info.peer, beat, NetChannel::State);
			}
		} else if (server && state != ClientState::Connecting) {
			beat.echo = hostTime ? hostTime + uint32_t(now - hostTimeAt) : 0;
			beat.rtt = rtt;
			Send(server, beat, NetChannel::State);
		}
	}

	// the other side's heartbeat: its echo of our clock gives the round trip
	static uint16_t RoundTrip(const NetHeartbeat& beat)
	{
		if (!beat.echo) return 0;
		uint32_t now = uint32_t(GetMilliseconds());
		return uint16_t(std::min<uint32_t>(now - beat.echo, 0xffff));
	}

	void OnReceive(ENetPeer* peer, const uint8_t* data, size_t size)
	{
		NetMsgType type;
		if (!NetPeekType(data, size, type)) {
			Log(WARNING, "Net", "Dropping malformed or incompatible packet ({} bytes) from {}.", size, PeerAddress(peer));
			return;
		}
		if (mode == Mode::Client) {
			lastHeard = GetMilliseconds();
		} else if (NetPeerInfo* info = FindPeer(peer)) {
			info->lastHeard = GetMilliseconds();
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
					// a returning player whose old connection we haven't noticed is gone yet
					for (auto it = peers.begin(); hello.token && it != peers.end(); ++it) {
						if (it->peer == peer || !it->welcomed || it->token != hello.token) continue;
						Log(MESSAGE, "Net", "Slot {} is back from {}; dropping its old connection.", it->slot, PeerAddress(peer));
						ENetPeer* old = it->peer;
						RemovePeer(it, "reconnected");
						enet_peer_reset(old);
						info = FindPeer(peer);
						break;
					}
					bool rejoined = false;
					uint8_t slot = roster.Join(hello.name, hello.token, NewToken(), rejoined);
					if (!slot) {
						NetReject reject;
						reject.reason = "the game is full";
						Send(peer, reject);
						enet_peer_disconnect_later(peer, 0);
						Log(WARNING, "Net", "Rejected '{}': {}", hello.name, reject.reason);
						return true;
					}
					info->slot = slot;
					info->name = hello.name;
					info->token = roster.Find(slot)->token;
					info->welcomed = true;
					// in the lobby the game follows once the host starts it
					info->wantsSave = phase == NetPhase::Game;
					NetWelcome welcome;
					welcome.slot = info->slot;
					welcome.hostName = name;
					welcome.token = info->token;
					welcome.flags = (rejoined ? NetWelcome::REJOINED : 0) | (phase == NetPhase::Game ? NetWelcome::IN_GAME : 0);
					Send(peer, welcome);
					Log(MESSAGE, "Net", "Player '{}' {} slot {}{}.", info->name, rejoined ? "rejoined in its old" : "joined in", info->slot, phase == NetPhase::Game ? ", the game follows" : ", waiting in the lobby");
					SendLobby(true);
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
			case NetMsgType::DialogChoice:
				{
					NetDialogChoice choice;
					if (!info->live || !NetDecode(data, size, choice)) return false;
					HandleDialogChoice(*info, choice);
					return true;
				}
			case NetMsgType::GuiCall:
				{
					NetGuiCall call;
					if (!info->live || !NetDecode(data, size, call)) return false;
					guiCalls.emplace_back(info->slot, std::move(call));
					return true;
				}
			case NetMsgType::Heartbeat:
				{
					NetHeartbeat beat;
					if (!NetDecode(data, size, beat)) return false;
					info->theirTime = beat.time;
					info->theirTimeAt = GetMilliseconds();
					if (beat.echo) info->rtt = RoundTrip(beat);
					return true;
				}
			case NetMsgType::LobbyReady:
				{
					NetLobbyReady ready;
					if (!info->welcomed || !NetDecode(data, size, ready)) return false;
					roster.SetReady(info->slot, ready.ready);
					Log(MESSAGE, "Net", "Slot {} ({}) is {}.", info->slot, info->name, ready.ready ? "ready" : "not ready");
					return true;
				}
			default:
				return false;
		}
	}

	static uint64_t NewToken()
	{
		static std::mt19937_64 rng(std::random_device {}() ^ uint64_t(GetMilliseconds()));
		uint64_t value = 0;
		while (!value) value = rng();
		return value;
	}

	// the one place that decides what a player may do; Control is about one party member
	bool Allowed(uint8_t slot, NetPermission perm, const Actor* actor = nullptr) const
	{
		if (perm == NetPermission::Control && !actor) return slot == NET_HOST_SLOT;
		return permissions.Allowed(slot, perm, actor ? OwnerOf(actor) : NET_HOST_SLOT);
	}

	NetPeerInfo* FindSlotPeer(uint8_t slot)
	{
		for (auto& info : peers) {
			if (info.welcomed && info.slot == slot) return &info;
		}
		return nullptr;
	}

	// a player left while dragging an item: it lands at its party member's feet
	static void DropDragged(NetPeerInfo& info)
	{
		if (!info.dragged) return;
		CREItem* item = info.dragged->item;
		info.dragged.reset();
		const Game* game = core->GetGame();
		Actor* actor = game ? game->GetActorByGlobalID(info.guiActor) : nullptr;
		if (actor && actor->GetCurrentArea()) {
			actor->GetCurrentArea()->AddItemToLocation(actor->Pos, item);
			Log(MESSAGE, "Net", "Slot {} left while dragging {}; dropped it at {}'s feet.", info.slot, item->ItemResRef, fmt::WideToChar { actor->GetShortName() });
		} else {
			delete item;
		}
	}

	void HandleReady(NetPeerInfo& info, const NetReady& ready)
	{
		if (ready.transferID != info.awaitedTransfer) {
			Log(MESSAGE, "Net", "Slot {} loaded save #{}, but #{} is on its way; still waiting.", info.slot, ready.transferID, info.awaitedTransfer);
			return;
		}
		Log(MESSAGE, "Net", "Slot {} ({}) loaded save #{} and mapped {} actors and objects; replaying {} events.", info.slot, info.name, ready.transferID, ready.mapped, info.pending.size());
		if (info.areaChangeAt) {
			Log(MESSAGE, "Net", "Slot {} followed the party in {} ms after it settled.", info.slot, GetMilliseconds() - info.areaChangeAt);
			info.areaChangeAt = 0;
		}
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
		// a conversation in progress, read only unless it is the player's own
		if (dialog.active) {
			if (haveDialogLine) {
				NetDialog line = dialogLine;
				line.owner = dialog.owner;
				Send(info.peer, line);
			}
			if (haveDialogState) {
				NetDialog node = dialogState;
				node.owner = dialog.owner;
				Send(info.peer, node);
			}
			Log(MESSAGE, "Net", "Slot {} joined during dialog node #{} (player {} answers); sent it the conversation.", info.slot, dialog.seq, dialog.owner);
		}
		// what a save loads the client's own way (effects), as the host has it
		const Game* g = core->GetGame();
		for (int pcSlot = 1; g && pcSlot <= g->GetPartySize(false); ++pcSlot) {
			const Actor* pc = g->FindPC(pcSlot);
			if (!pc) continue;
			NetActorStats full;
			full.netID = pc->GetGlobalID();
			full.stats = NetStatsDiff({}, ActorStats(pc), SNAPSHOT_STATS);
			Send(info.peer, full);
			// the equipped slot is worked out again on load, not always the same way
			Send(info.peer, InventoryMessage(pc));
			Send(info.peer, SpellbookMessage(pc));
		}
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
		if (!Allowed(info.slot, NetPermission::Pause)) {
			Notify(info, "The host doesn't let you pause.");
			NetPause reply;
			reply.on = IsPaused();
			Send(info.peer, reply);
			return;
		}
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
		Log(MESSAGE, "Net", "Told slot {} ({}): {}", info.slot, info.name, text);
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
			"nidspecial1", // talk to
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
		if (!Allowed(info.slot, NetPermission::Control, actor)) {
			Log(WARNING, "Net", "Slot {} ({}): rejected '{}' for {} (id {}), who belongs to slot {}.", info.slot, info.name, cmd.action, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, owner);
			Notify(info, fmt::format("{} is controlled by player {}.", fmt::WideToChar { actor->GetShortName() }, owner));
			return;
		}

		std::string name = LowerCase(cmd.action);
		if (name == "usecontainer") {
			HandleUseContainer(info, actor, cmd);
			return;
		}
		if (name == "clickregion") {
			HandleRegionClick(info, actor, cmd);
			return;
		}
		if (name == "nidspecial1" && !Allowed(info.slot, NetPermission::Dialog)) {
			Notify(info, "The host doesn't let you start conversations.");
			return;
		}
		if (name == "useexit" && !Allowed(info.slot, NetPermission::AreaTransition)) {
			Notify(info, "The host doesn't let you change areas.");
			return;
		}
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

	// a click on a container: the actor walks there and opens it (UseContainer); the
	// container is named in the action, the global current container is our own GUI's
	void HandleUseContainer(const NetPeerInfo& info, Actor* actor, const NetCommand& cmd)
	{
		Map* area = actor->GetCurrentArea();
		Container* container = nullptr;
		if (area && cmd.int0) {
			container = Scriptable::As<Container>(area->GetScriptableByGlobalID(ieDword(cmd.int0)));
		} else if (area) {
			container = area->TMap->GetContainer(Point(cmd.x, cmd.y), IE_CONTAINER_PILE);
		}
		if (!container || (container->Flags & CONT_DISABLED)) {
			Log(WARNING, "Net", "Slot {}: rejected UseContainer for {}: no usable container {} at ({}, {}).", info.slot, cmd.actorID, cmd.int0, cmd.x, cmd.y);
			return;
		}
		Holder<Action> action = GenerateAction("UseContainer()");
		action->int0Parameter = int(container->GetGlobalID());
		container->AddTrigger(TriggerEntry(trigger_clicked, actor->GetGlobalID()));
		actor->CommandActor(std::move(action));
		Log(MESSAGE, "Net", "Slot {} ({}): {} (id {}) goes to open container '{}' (id {}).", info.slot, info.name, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, container->GetScriptName(), container->GetGlobalID());
	}

	// what GameControl::HandleActiveRegion does for an info point with a script
	void HandleRegionClick(const NetPeerInfo& info, Actor* actor, const NetCommand& cmd)
	{
		Map* area = actor->GetCurrentArea();
		InfoPoint* region = area ? Scriptable::As<InfoPoint>(area->GetScriptableByGlobalID(ieDword(cmd.int0))) : nullptr;
		if (!region || region->Type != ST_TRIGGER || !region->Scripts[0]) {
			Log(WARNING, "Net", "Slot {}: rejected a click on region {}: no info point with a script.", info.slot, cmd.int0);
			return;
		}
		if ((region->Flags & TRAP_DEACTIVATED) || IsPaused()) {
			Log(MESSAGE, "Net", "Slot {}: info point '{}' is deactivated or the game is frozen, ignoring the click.", info.slot, region->GetScriptName());
			return;
		}
		// it runs right away, so it counts as simulation: what it shows goes to everyone
		bool wasCapturing = capturing;
		capturing = true;
		region->AddTrigger(TriggerEntry(trigger_clicked, actor->GetGlobalID()));
		actor->objects.LastMarked = region->GetGlobalID();
		region->ExecuteScript(1);
		region->ProcessActions();
		capturing = wasCapturing;
		Log(MESSAGE, "Net", "Slot {} ({}): {} clicked info point '{}', ran its script here.", info.slot, info.name, fmt::WideToChar { actor->GetShortName() }, region->GetScriptName());
	}

	// UseContainer got there for a remote player's party member: its owner's screen opens it
	void OnContainerUsed(const Actor& actor, Container& container)
	{
		NetPeerInfo* info = FindSlotPeer(OwnerOf(&actor));
		if (mode != Mode::Host || !info) {
			core->SetCurrentContainer(&actor, &container, true);
			return;
		}
		info->container = container.GetGlobalID();
		info->containerActor = actor.GetGlobalID();
		NetContainerOpen msg;
		msg.actorID = actor.GetGlobalID();
		if (IsPile(&container)) {
			msg.x = container.Pos.x;
			msg.y = container.Pos.y;
		} else {
			msg.containerID = container.GetGlobalID();
		}
		// the contents first, so the window opens on the right items
		inventories.erase(container.GetGlobalID());
		TrackItems(true);
		Send(info->peer, msg);
		Log(MESSAGE, "Net", "Container '{}' (id {}) opened for slot {} by {}, {} items.", container.GetScriptName(), container.GetGlobalID(), info->slot, fmt::WideToChar { actor.GetShortName() }, container.inventory.GetSlotCount());
	}

	// dialog (host) ----------------------------------------------------------

	int PartyOwner(const Scriptable* scr) const
	{
		const Actor* actor = Scriptable::As<Actor>(scr);
		if (!actor || !actor->InParty) return -1;
		return OwnerOf(actor);
	}

	// the first line or node of a conversation decides who answers it
	void StartDialog()
	{
		if (dialog.active) return;
		const GameControl* gc = core->GetGameControl();
		const Scriptable* speaker = gc->dialoghandler->GetSpeaker();
		const Scriptable* target = gc->dialoghandler->GetTarget();
		dialog.active = true;
		dialog.speakerID = speaker ? speaker->GetGlobalID() : 0;
		dialog.targetID = target ? target->GetGlobalID() : 0;
		dialog.owner = NetDialogOwner(PartyOwner(speaker), PartyOwner(target), NET_HOST_SLOT);
		Log(MESSAGE, "Net", "Dialog between {} and {} starts, player slot {} answers.", dialog.speakerID, dialog.targetID, dialog.owner);
	}

	NetDialog DialogMessage(uint8_t kind) const
	{
		NetDialog msg;
		msg.kind = kind;
		msg.seq = dialog.seq;
		msg.owner = dialog.owner;
		msg.speakerID = dialog.speakerID;
		msg.targetID = dialog.targetID;
		const GameControl* gc = core->GetGameControl();
		if (gc && (gc->GetDialogueFlags() & DF_FREEZE_SCRIPTS)) msg.flags |= NetDialog::FREEZE;
		return msg;
	}

	void OnDialogLine(const Scriptable* speaker, const String& text, bool partyLine)
	{
		if (mode != Mode::Host) return;
		StartDialog();
		NetDialog msg = DialogMessage(NetDialog::LINE);
		if (partyLine) msg.flags |= NetDialog::PARTY;
		msg.speakerID = speaker ? speaker->GetGlobalID() : 0;
		msg.text = Utf8(text);
		SendEvent(msg, false);
		if (!partyLine) {
			dialogLine = msg;
			haveDialogLine = true;
		}
		Log(MESSAGE, "Net", "Event: dialog line of net id {}: '{}'", msg.speakerID, msg.text);
	}

	void OnDialogState(const std::vector<SelectOption>& options)
	{
		if (mode != Mode::Host) return;
		StartDialog();
		dialog.seq++;
		NetDialog msg = DialogMessage(NetDialog::STATE);
		unsigned int flags = core->GetGameControl()->GetDialogueFlags();
		if (flags & DF_OPENCONTINUEWINDOW) msg.flags |= NetDialog::CONTINUE;
		if (flags & DF_OPENENDWINDOW) msg.flags |= NetDialog::END_BUTTON;
		msg.continueIndex = uint16_t(core->GetDictionary().Get("DialogOption", 0));
		std::string list;
		for (const auto& option : options) {
			msg.options.push_back(NetDialogOption { uint16_t(option.first), Utf8(option.second) });
			list += fmt::format(" [{}] '{}'", option.first, msg.options.back().text);
		}
		SendEvent(msg, false);
		dialogState = msg;
		haveDialogState = true;
		Log(MESSAGE, "Net", "Event: dialog node #{} for slot {}{}:{}{}", msg.seq, dialog.owner, msg.flags & NetDialog::FREEZE ? " (world frozen)" : "", list.empty() ? " no replies" : list, msg.flags & (NetDialog::CONTINUE | NetDialog::END_BUTTON) ? fmt::format(", {} button (#{})", msg.flags & NetDialog::CONTINUE ? "continue" : "end", msg.continueIndex) : "");
	}

	void OnDialogEnd()
	{
		if (mode != Mode::Host || !dialog.active) return;
		NetDialog msg = DialogMessage(NetDialog::END);
		msg.flags &= ~NetDialog::FREEZE;
		SendEvent(msg, false);
		Log(MESSAGE, "Net", "Event: dialog #{} owned by slot {} ended.", dialog.seq, dialog.owner);
		lastDialog = dialog;
		dialog = HostDialog();
		haveDialogLine = haveDialogState = false;
	}

	// the player answering a conversation left: the host answers it from now on, so
	// nobody waits for a reply that won't come; the others see who is choosing now
	void HandDialogToHost(uint8_t slot)
	{
		dialog.owner = NET_HOST_SLOT;
		Log(MESSAGE, "Net", "Dialog node #{}: its owner (slot {}) left, the host answers it now.", dialog.seq, slot);
		displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format("[p][color=FFC080FF]Player {} left the conversation; you answer it now.[/color][/p]", slot).c_str()));
		if (haveDialogState) {
			dialogState.owner = NET_HOST_SLOT;
			SendEvent(dialogState, false);
		}
	}

	// the owner's reply, the same call a click on it makes on the host
	void HandleDialogChoice(const NetPeerInfo& info, const NetDialogChoice& choice)
	{
		if (!dialog.active || choice.seq != dialog.seq) {
			Log(MESSAGE, "Net", "Slot {} answered dialog node #{}, but we are at #{}; ignored.", info.slot, choice.seq, dialog.active ? dialog.seq : 0);
			return;
		}
		if (dialog.owner != info.slot || !Allowed(info.slot, NetPermission::Dialog)) {
			Log(WARNING, "Net", "Slot {} tried to answer slot {}'s dialog.", info.slot, dialog.owner);
			Notify(info, fmt::format("Player {} is doing the talking.", dialog.owner));
			return;
		}
		Log(MESSAGE, "Net", "Slot {} ({}) picked reply {} of dialog node #{}.", info.slot, info.name, choice.index, choice.seq);
		GameControl* gc = core->GetGameControl();
		applyingChoice = true;
		gc->dialoghandler->DialogChoose(choice.index);
		// what HandleGUIBehaviour does after a choice of our own
		if (gc->InDialog() && !(gc->GetDialogueFlags() & (DF_OPENCONTINUEWINDOW | DF_OPENENDWINDOW))) {
			core->GetGUIScriptEngine()->RunFunction("GUIWORLD", "NextDialogState");
		}
		applyingChoice = false;
	}

	bool HostChoiceHandled(int choice) const
	{
		if (applyingChoice || !dialog.active || dialog.owner == NET_HOST_SLOT) return false;
		Log(MESSAGE, "Net", "Ignoring our reply {}: player slot {} owns this dialog.", choice, dialog.owner);
		displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format("[p][color=FF8080FF]Player {} is doing the talking.[/color][/p]", dialog.owner).c_str()));
		return true;
	}

	// stores (host) ------------------------------------------------------------

	static NetStoreState StoreMessage(const Store* store)
	{
		NetStoreState msg;
		msg.store = std::string(store->Name.c_str());
		for (const STOItem* item : store->items) {
			NetStoreItem entry;
			entry.resref = std::string(item->ItemResRef.c_str());
			std::copy(item->Usages.begin(), item->Usages.end(), entry.usages.begin());
			entry.flags = item->Flags;
			entry.amount = item->AmountInStock;
			entry.infinite = item->InfiniteSupply;
			entry.purchased = item->PurchasedAmount;
			msg.items.push_back(entry);
		}
		return msg;
	}

	// StartStore from a conversation a remote player owns: the store opens on its screen
	// a store has one copy of its stock and selection, so one player trades at a time (BG
	// does the same); the shopper is a remote player, or -1 for our own GUI
	int StoreShopper(const ResRef& storeRef, int except) const
	{
		for (const auto& info : peers) {
			if (info.live && info.store == storeRef && info.slot != except) return info.slot;
		}
		const Store* mine = core->GetCurrentStore();
		if (except != NET_HOST_SLOT && mine && mine->Name == storeRef) return NET_HOST_SLOT;
		return -1;
	}

	bool OnStartStore(const ResRef& storeRef, const Scriptable& merchant)
	{
		if (mode != Mode::Host) return false;
		const HostDialog& talk = dialog.active ? dialog : lastDialog;
		ieDword merchantID = merchant.GetGlobalID();
		bool ours = talk.owner == NET_HOST_SLOT || (merchantID != talk.speakerID && merchantID != talk.targetID);
		NetPeerInfo* info = ours ? nullptr : FindSlotPeer(talk.owner);
		if (!ours && (!info || !info->live)) return false;
		int busy = StoreShopper(storeRef, info ? info->slot : NET_HOST_SLOT);
		if (busy >= 0) {
			const Actor* trader = Scriptable::As<Actor>(&merchant);
			std::string why = fmt::format("Player {} is trading with {} right now; try again once they are done.", busy, trader ? Utf8(trader->GetShortName()) : std::string("them"));
			Log(MESSAGE, "Net", "Store {} is busy (slot {} trades there), not opening it for slot {}.", storeRef, busy, info ? info->slot : NET_HOST_SLOT);
			if (info) {
				Notify(*info, why);
			} else {
				ShowLocal(why);
			}
			return true;
		}
		if (ours) return false;
		if (!Allowed(info->slot, NetPermission::Purchase)) {
			Notify(*info, "The host doesn't let you trade.");
			return true;
		}
		Store* store = gamedata->GetStore(storeRef);
		if (!store) return false;
		ieDword pcID = talk.speakerID == merchantID ? talk.targetID : talk.speakerID;
		info->store = storeRef;
		info->storeActor = pcID;
		info->storeOwner = merchantID;
		NetStoreState msg = StoreMessage(store);
		storeHashes[std::string(storeRef.c_str())] = NetMessageHash(msg);
		msg.flags = NetStoreState::OPEN;
		msg.ownerID = merchantID;
		msg.actorID = pcID;
		Send(info->peer, msg);
		Log(MESSAGE, "Net", "Store {} opens for slot {} (net id {} shopping, {} wares).", storeRef, info->slot, pcID, msg.items.size());
		return true;
	}

	// stores remote players have open change when anyone trades there; everyone gets the
	// new stock (shoppers see it at once, the others have it in their cache for later)
	void TrackStores()
	{
		std::set<std::string> open;
		for (const auto& info : peers) {
			if (info.live && !info.store.IsEmpty()) open.insert(std::string(info.store.c_str()));
		}
		for (const std::string& name : open) {
			const Store* store = gamedata->GetStore(ResRef(name.c_str()));
			if (!store) continue;
			NetStoreState msg = StoreMessage(store);
			uint64_t hash = NetMessageHash(msg);
			uint64_t& known = storeHashes[name];
			if (hash == known) continue;
			known = hash;
			SendEvent(msg, true);
			Log(MESSAGE, "Net", "Event: store {} changed, {} wares.", name, msg.items.size());
		}
	}

	// GUI calls (host) -----------------------------------------------------------

	Actor* GuiActor(const NetPeerInfo& info, NetGuiOp op, int32_t arg) const
	{
		const Game* game = core->GetGame();
		if (arg == 0 && op == NetGuiOp::ChangeContainerItem) return game->GetActorByGlobalID(info.containerActor);
		if (arg > 1000) return game->GetActorByGlobalID(ieDword(arg));
		return game->FindPC(arg);
	}

	// a Python string literal
	static std::string PyString(const std::string& text)
	{
		std::string literal = "\"";
		for (char ch : text) {
			if (ch == '\\' || ch == '"') literal += '\\';
			if (ch == '\n') {
				literal += "\\n";
				continue;
			}
			literal += ch;
		}
		return literal + "\"";
	}

	static std::string PythonCall(const NetGuiOpInfo& opInfo, const NetGuiCall& call)
	{
		std::vector<std::string> args;
		for (int32_t arg : call.args) {
			args.push_back(fmt::format("{}", arg));
		}
		switch (call.op) {
			case NetGuiOp::DragItem:
				// the dragged item's picture isn't used by the core
				args.insert(args.begin() + 2, "''");
				break;
			case NetGuiOp::AddPartyGold:
				args.emplace_back("1");
				break;
			case NetGuiOp::SetModalState:
				args.push_back(PyString(call.text));
				break;
			case NetGuiOp::SetJournalEntry:
				// a text entry instead of a strref; no feedback string
				if (!call.text.empty()) args[0] = PyString(call.text);
				if (call.args[3] == -1) args.pop_back();
				break;
			case NetGuiOp::SetMapnote:
				args.push_back(PyString(call.text));
				break;
			default:
				break;
		}
		std::string list;
		for (const auto& arg : args) {
			list += (list.empty() ? "" : ", ") + arg;
		}
		return fmt::format("import GemRB\nGemRB.{}({})", opInfo.name, list);
	}

	void RunGuiCalls()
	{
		auto calls = std::move(guiCalls);
		guiCalls.clear();
		for (const auto& entry : calls) {
			NetPeerInfo* info = FindSlotPeer(entry.first);
			if (info && info->live && core->GetGame()) {
				RunGuiCall(*info, entry.second);
			}
		}
	}

	// the same GemRB function the client's GUI called, run on the client's GUI state
	// (dragged item, open container and store, selection), so the rules are the GUI's own
	void RunGuiCall(NetPeerInfo& info, const NetGuiCall& call)
	{
		NetGuiOpInfo opInfo = NetGuiInfo(call.op);
		Game* game = core->GetGame();
		Actor* actor = nullptr;
		std::string why;
		if (call.args.size() < opInfo.minArgs) {
			why = "missing arguments";
		} else if (opInfo.actorArg >= 0) {
			actor = GuiActor(info, call.op, call.args[size_t(opInfo.actorArg)]);
			if (!actor || !actor->InParty) {
				why = "not a party member";
			} else if (!Allowed(info.slot, NetPermission::Control, actor)) {
				why = fmt::format("{} belongs to player {}", fmt::WideToChar { actor->GetShortName() }, OwnerOf(actor));
			}
		}
		if (why.empty() && opInfo.permission != NetPermission::Control && !Allowed(info.slot, opInfo.permission)) {
			why = fmt::format("no {} permission", NetPermissionName(opInfo.permission));
		}
		bool storeOp = call.op == NetGuiOp::ChangeStoreItem || call.op == NetGuiOp::SetPurchasedAmount;
		if (why.empty() && storeOp && info.store.IsEmpty()) why = "no store open";
		Map* area = actor ? actor->GetCurrentArea() : nullptr;
		Container* container = nullptr;
		if (info.container) {
			const Actor* opener = game->GetActorByGlobalID(info.containerActor);
			Map* map = opener ? opener->GetCurrentArea() : nullptr;
			container = map ? Scriptable::As<Container>(map->GetScriptableByGlobalID(info.container)) : nullptr;
		}
		if (why.empty() && call.op == NetGuiOp::ChangeContainerItem && call.args[0] == 0 && !container) why = "no container open";
		if (!why.empty()) {
			Log(WARNING, "Net", "Slot {} ({}): refused GUI call {}: {}.", info.slot, info.name, opInfo.name, why);
			Notify(info, fmt::format("The host refused that: {}.", why));
			// the client changed its copy already; resend the truth
			if (actor) ForgetActor(actor->GetGlobalID());
			ForgetContainer(info.container);
			partyDirty = true;
			storeHashes.erase(std::string(info.store.c_str()));
			return;
		}
		if (actor) info.guiActor = actor->GetGlobalID();

		switch (call.op) {
			case NetGuiOp::LeaveContainer:
				info.container = info.containerActor = 0;
				Log(MESSAGE, "Net", "Slot {} ({}) closed its container.", info.slot, info.name);
				return;
			case NetGuiOp::LeaveStore:
				// the store stays cached; our own GUI may have it open too
				Log(MESSAGE, "Net", "Slot {} ({}) left store {}.", info.slot, info.name, info.store);
				info.store.Reset();
				info.storeActor = info.storeOwner = 0;
				return;
			case NetGuiOp::RestParty:
				{
					// what GemRB.RestParty does, telling the player why not
					ieStrRef err = ieStrRef::INVALID;
					if (!game->CanPartyRest(RestChecks(call.args[0]), &err)) {
						std::string why = err == ieStrRef::INVALID ? "you may not rest now" : Utf8(core->GetString(err));
						Log(MESSAGE, "Net", "Slot {} ({}) wants to rest, but the party can't: {}.", info.slot, info.name, why);
						Notify(info, fmt::format("The party can't rest: {}", why));
						return;
					}
					game->RestParty(RestChecks(call.args[0] & (RestChecks::Area | RestChecks::NoRepeats)), call.args[1], call.args[2]);
					Log(MESSAGE, "Net", "Slot {} ({}) rested the party.", info.slot, info.name);
					partyDirty = true;
					return;
				}
			case NetGuiOp::UseItem:
				// only items used without a target come this way (the rest are orders)
				if (!actor->inventory.GetSlotItem(ieDword(call.args[1]))) {
					Log(WARNING, "Net", "Slot {}: refused UseItem, nothing in slot {}.", info.slot, call.args[1]);
					return;
				}
				actor->UseItem(ieDword(call.args[1]), call.args[2], nullptr, call.args.size() > 3 ? call.args[3] : 0);
				Log(MESSAGE, "Net", "Slot {} ({}): {} uses the item in slot {}.", info.slot, info.name, fmt::WideToChar { actor->GetShortName() }, call.args[1]);
				ForgetActor(actor->GetGlobalID());
				return;
			default:
				break;
		}

		// swap in the client's GUI state and selection
		GUIState state;
		state.draggedItem = std::move(info.dragged);
		state.container = container;
		state.useContainer = container != nullptr;
		state.store = storeOp ? gamedata->GetStore(info.store) : core->GetCurrentStore();
		core->SwapGUIState(state);
		Actor* selected = call.op == NetGuiOp::ChangeContainerItem && call.args[0] == 0 ? game->GetActorByGlobalID(info.containerActor) : game->FindPC(call.selected);
		std::vector<std::pair<Actor*, ieWord>> selection;
		for (int pcSlot = 1; pcSlot <= game->GetPartySize(false); ++pcSlot) {
			Actor* pc = game->FindPC(pcSlot);
			if (!pc) continue;
			selection.emplace_back(pc, pc->Selected);
			pc->Selected = pc == selected ? 1 : 0;
		}
		int single = game->SelectedSingle;
		if (selected) game->SelectedSingle = selected->InParty;

		std::string code = PythonCall(opInfo, call);
		guiCaller = &info;
		bool ok = core->GetGUIScriptEngine()->ExecString(code);
		guiCaller = nullptr;

		game->SelectedSingle = single;
		for (const auto& pc : selection) {
			pc.first->Selected = pc.second;
		}
		core->SwapGUIState(state);
		info.dragged = std::move(state.draggedItem);
		Log(MESSAGE, "Net", "Slot {} ({}): ran GUI call {}{} for {}{}.", info.slot, info.name, code.substr(code.find('\n') + 7), ok ? "" : " (it failed)", actor ? fmt::format("{}", fmt::WideToChar { actor->GetShortName() }) : "the party", info.dragged ? fmt::format(", now dragging {}", info.dragged->item->ItemResRef) : "");

		// the client went ahead on its own copy: send the result even if nothing changed
		if (actor) ForgetActor(actor->GetGlobalID());
		if (selected) ForgetActor(selected->GetGlobalID());
		ForgetContainer(info.container);
		if (area) {
			const Container* pile = area->TMap->GetContainer(actor->Pos, IE_CONTAINER_PILE);
			if (pile) ForgetContainer(pile->GetGlobalID());
		}
		partyDirty = true;
		storeHashes.erase(std::string(info.store.c_str()));
	}

	void ForgetActor(ieDword id)
	{
		inventories.erase(id);
		stats.erase(id);
		spellbooks.erase(id);
	}

	void ForgetContainer(ieDword id)
	{
		if (id) inventories.erase(id);
	}

	// inventories, stats, spellbooks, gold (host) ----------------------------------

	static NetItem ItemEntry(const CREItem* item, size_t slot)
	{
		NetItem entry;
		entry.slot = uint16_t(slot);
		entry.resref = std::string(item->ItemResRef.c_str());
		std::copy(item->Usages.begin(), item->Usages.end(), entry.usages.begin());
		entry.flags = item->Flags;
		entry.expired = item->Expired;
		return entry;
	}

	static void ListItems(const Inventory& inv, NetInventory& msg)
	{
		for (int slot = 0; slot < inv.GetSlotCount(); ++slot) {
			const CREItem* item = inv.GetSlotItem(ieDword(slot));
			if (item) msg.items.push_back(ItemEntry(item, size_t(slot)));
		}
	}

	static NetInventory InventoryMessage(const Actor* actor)
	{
		NetInventory msg;
		msg.netID = actor->GetGlobalID();
		msg.equipped = int16_t(actor->inventory.GetEquipped());
		msg.equippedHeader = uint16_t(actor->inventory.GetEquippedHeader());
		ListItems(actor->inventory, msg);
		return msg;
	}

	static NetInventory InventoryMessage(const Container* container)
	{
		NetInventory msg;
		msg.kind = NetObjectKind::Container;
		if (IsPile(container)) {
			msg.area = std::string(container->GetCurrentArea()->GetScriptRef());
			msg.x = container->Pos.x;
			msg.y = container->Pos.y;
		} else {
			msg.netID = container->GetGlobalID();
		}
		ListItems(container->inventory, msg);
		return msg;
	}

	static std::vector<NetStatEntry> ActorStats(const Actor* actor)
	{
		std::vector<NetStatEntry> list(MAX_STATS);
		for (unsigned int stat = 0; stat < MAX_STATS; ++stat) {
			list[stat] = NetStatEntry { uint16_t(stat), actor->GetBase(stat), actor->GetStat(stat) };
		}
		return list;
	}

	static NetSpellbook SpellbookMessage(const Actor* actor)
	{
		NetSpellbook msg;
		msg.netID = actor->GetGlobalID();
		Spellbook& book = const_cast<Actor*>(actor)->spellbook;
		for (int type = 0; type < book.GetTypes(); ++type) {
			for (unsigned int level = 0; level < book.GetSpellLevelCount(type); ++level) {
				const CRESpellMemorization* sm = book.GetSpellMemorization(unsigned(type), level);
				if (!sm || (sm->knownSpells.empty() && sm->memorizedSpells.empty() && !sm->SlotCount)) continue;
				NetSpellPage page;
				page.type = uint8_t(type);
				page.level = uint8_t(level);
				page.slots = sm->SlotCount;
				page.slotsWithBonus = sm->SlotCountWithBonus;
				for (const auto& known : sm->knownSpells) {
					page.known.emplace_back(known->SpellResRef.c_str());
				}
				for (const auto& memorized : sm->memorizedSpells) {
					page.memorized.push_back(NetMemorizedSpell { std::string(memorized->SpellResRef.c_str()), memorized->Flags });
				}
				msg.pages.push_back(std::move(page));
			}
		}
		return msg;
	}

	static size_t CountReady(const NetSpellbook& book)
	{
		size_t ready = 0;
		for (const auto& page : book.pages) {
			for (const auto& memorized : page.memorized) {
				ready += memorized.flags ? 1 : 0;
			}
		}
		return ready;
	}

	// send whatever changed since the clients last heard: inventories of everyone in the
	// replicated area and of the party, containers and piles there, party members' stats
	// and spellbooks and the party's gold; with send unset, only note the current state
	void TrackItems(bool send)
	{
		const Game* game = core->GetGame();
		if (!replMap) return;
		std::vector<const Actor*> actors(replMap->GetAllActors().begin(), replMap->GetAllActors().end());
		for (int pcSlot = 1; pcSlot <= game->GetPartySize(false); ++pcSlot) {
			const Actor* pc = game->FindPC(pcSlot);
			if (pc && pc->GetCurrentArea() != replMap) actors.push_back(pc);
		}
		for (const Actor* actor : actors) {
			NetInventory inv = InventoryMessage(actor);
			uint64_t hash = NetMessageHash(inv);
			auto it = inventories.find(inv.netID);
			if (it != inventories.end() && it->second == hash) continue;
			inventories[inv.netID] = hash;
			if (!send) continue;
			SendEvent(inv, true);
			Log(MESSAGE, "Net", "Event: inventory of net id {} ({}): {} items, equipped {}.", inv.netID, fmt::WideToChar { actor->GetShortName() }, inv.items.size(), inv.equipped);
		}

		std::set<ieDword> seenPiles;
		for (const auto& holder : replMap->GetTileMap()->GetContainers()) {
			const Container* container = holder.get();
			ieDword id = container->GetGlobalID();
			bool pile = IsPile(container);
			if (pile) {
				seenPiles.insert(id);
				piles[id] = container->Pos;
			}
			NetInventory inv = InventoryMessage(container);
			uint64_t hash = NetMessageHash(inv);
			auto it = inventories.find(id);
			if (it != inventories.end() && it->second == hash) continue;
			inventories[id] = hash;
			if (!send) continue;
			SendEvent(inv, true);
			Log(MESSAGE, "Net", "Event: inventory of {} '{}' (id {}{}): {} items.", pile ? "pile" : "container", container->GetScriptName(), id, pile ? fmt::format(" at {}", container->Pos) : "", inv.items.size());
		}
		// a pile that went away (emptied and cleaned up) is just empty for the clients
		for (auto it = piles.begin(); it != piles.end();) {
			if (seenPiles.count(it->first)) {
				++it;
				continue;
			}
			if (send) {
				NetInventory gone;
				gone.kind = NetObjectKind::Container;
				gone.area = replArea;
				gone.x = it->second.x;
				gone.y = it->second.y;
				SendEvent(gone, true);
				Log(MESSAGE, "Net", "Event: pile (id {}) at {} is gone.", it->first, it->second);
			}
			inventories.erase(it->first);
			it = piles.erase(it);
		}

		for (int pcSlot = 1; pcSlot <= game->GetPartySize(false); ++pcSlot) {
			const Actor* pc = game->FindPC(pcSlot);
			if (!pc) continue;
			ieDword id = pc->GetGlobalID();
			std::vector<NetStatEntry> now = ActorStats(pc);
			auto known = stats.find(id);
			NetActorStats msg;
			msg.netID = id;
			msg.stats = NetStatsDiff(known == stats.end() ? std::vector<NetStatEntry>() : known->second, now, SNAPSHOT_STATS);
			stats[id] = std::move(now);
			if (send && !msg.stats.empty()) {
				SendEvent(msg, true);
				std::string list;
				for (size_t i = 0; i < msg.stats.size() && i < 6; ++i) {
					list += fmt::format(" {}={}/{}", msg.stats[i].stat, msg.stats[i].base, msg.stats[i].modified);
				}
				Log(MESSAGE, "Net", "Event: {} stat(s) of net id {} ({}) changed:{}{}", msg.stats.size(), id, fmt::WideToChar { pc->GetShortName() }, list, msg.stats.size() > 6 ? " ..." : "");
			}

			NetSpellbook book = SpellbookMessage(pc);
			uint64_t hash = NetMessageHash(book);
			auto bookIt = spellbooks.find(id);
			if (bookIt != spellbooks.end() && bookIt->second == hash) continue;
			spellbooks[id] = hash;
			if (!send) continue;
			SendEvent(book, true);
			Log(MESSAGE, "Net", "Event: spellbook of net id {} ({}): {} pages, {} spells ready.", id, fmt::WideToChar { pc->GetShortName() }, book.pages.size(), CountReady(book));
		}

		NetPartyState now;
		now.gold = game->PartyGold;
		now.reputation = game->Reputation;
		if (now.gold != party.gold || now.reputation != party.reputation || partyDirty) {
			party = now;
			partyDirty = false;
			if (send) {
				SendEvent(now, true);
				Log(MESSAGE, "Net", "Event: party gold {}, reputation {}.", now.gold, now.reputation);
			}
		}
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
		bool anyArea = false;
		for (const auto& info : peers) {
			anyArea |= info.welcomed && info.wantsArea && !info.wantsSave && !info.transfer;
		}
		NetAreaData area;
		NetActorTable areaTable;
		if (anyArea) {
			bool ok = TakeAreaSnapshot(area, areaTable);
			if (ok) TrackGlobals(); // before the peers stop taking live events
			for (auto& info : peers) {
				if (!info.welcomed || !info.wantsArea || info.wantsSave || info.transfer) continue;
				info.wantsArea = false;
				if (!ok) {
					info.wantsSave = true;
					continue;
				}
				area.transferID = areaTable.transferID = nextTransferID++;
				info.live = false;
				info.awaitedTransfer = area.transferID;
				info.pending.clear();
				auto data = NetEncode(area);
				auto tableData = NetEncode(areaTable);
				SendRaw(info.peer, data, NetChannel::Transfer);
				SendRaw(info.peer, tableData, NetChannel::Transfer);
				Log(MESSAGE, "Net", "Area change for slot {}: sent {} as area data #{}, {} bytes (ARE {}) and a table of {} entries, {} bytes.", info.slot, area.area, area.transferID, data.size(), area.are.size(), areaTable.actors.size(), tableData.size());
			}
		}

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
				Log(MESSAGE, "Net", "Sending save #{} to slot {}: {} bytes, hash {:016x}, party in {}{}.", begin.transferID, info.slot, begin.size, begin.hash, begin.area, info.areaChangeAt ? " (an area change)" : "");
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

	// who controls each party member: its slot's owner (the lobby's assignment or the default),
	// or the host while that player is away
	void UpdateOwnership()
	{
		const Game* game = core->GetGame();
		NetOwnership now;
		for (int slot = 1; slot <= game->GetPartySize(false); ++slot) {
			const Actor* pc = game->FindPC(slot);
			if (!pc) continue;
			NetOwnershipEntry entry;
			entry.netID = pc->GetGlobalID();
			entry.partySlot = uint8_t(slot);
			entry.owner = NetSlotController(entry.partySlot, assigned, roster);
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
			uint8_t owner = NetSlotOwner(entry.partySlot, assigned, roster);
			Log(MESSAGE, "Net", "Ownership: party slot {} {} (net id {}) -> player slot {}{}.", entry.partySlot, pc ? fmt::format("{}", fmt::WideToChar { pc->GetShortName() }) : "?", entry.netID, entry.owner, owner != entry.owner ? fmt::format(" (for player {}, who is away)", owner) : "");
		}
		SendEvent(owners, false);
	}

	// lobby (host) ------------------------------------------------------------------

	NetLobbyState BuildLobby() const
	{
		NetLobbyState state;
		state.phase = phase;
		for (const NetPlayer& player : roster.All()) {
			NetLobbyPlayer entry;
			entry.slot = player.slot;
			entry.name = player.name;
			entry.flags = (player.connected ? NetLobbyPlayer::CONNECTED : 0) | (player.ready ? NetLobbyPlayer::READY : 0);
			entry.allowed = permissions.Mask(player.slot);
			for (const auto& info : peers) {
				if (info.welcomed && info.slot == player.slot) entry.ping = info.rtt;
			}
			state.players.push_back(std::move(entry));
		}
		// before the game runs there are six slots to hand out, BG style
		const Game* game = core->GetGame();
		int slots = game && game->GetPartySize(false) ? game->GetPartySize(false) : NET_PARTY_SLOTS;
		for (int slot = 1; slot <= slots && slot <= NET_PARTY_SLOTS; ++slot) {
			NetLobbySlot entry;
			entry.partySlot = uint8_t(slot);
			entry.owner = NetSlotOwner(entry.partySlot, assigned, roster);
			entry.controller = NetSlotController(entry.partySlot, assigned, roster);
			const Actor* pc = game ? game->FindPC(slot) : nullptr;
			if (pc) entry.name = Utf8(pc->GetShortName());
			state.party.push_back(std::move(entry));
		}
		return state;
	}

	// everyone gets the lobby whenever it changes (and now and then for the pings)
	void SendLobby(bool force = false)
	{
		NetLobbyState state = BuildLobby();
		NetLobbyState noPings = state;
		for (auto& player : noPings.players) {
			player.ping = 0;
		}
		auto data = NetEncode(noPings);
		uint64_t hash = NetHash(data.data(), data.size());
		tick_t now = GetMilliseconds();
		if (hash == lobbyHash && !force && now - lobbySentAt < LOBBY_REFRESH) return;
		if (hash != lobbyHash) {
			std::string list;
			for (const auto& player : state.players) {
				list += fmt::format(" {}:'{}'{}{}", player.slot, player.name, player.flags & NetLobbyPlayer::CONNECTED ? "" : " (away)", player.flags & NetLobbyPlayer::READY ? " ready" : "");
			}
			std::string party;
			for (const auto& slot : state.party) {
				party += fmt::format(" {}->{}{}", slot.partySlot, slot.owner, slot.controller != slot.owner ? fmt::format("(host meanwhile)") : "");
			}
			Log(MESSAGE, "Net", "Lobby ({}): players{}; party slots{}.", phase == NetPhase::Lobby ? "waiting" : "in game", list, party);
			lobbyRevision++;
		}
		lobbyHash = hash;
		lobbySentAt = now;
		lobby = std::move(state);
		Broadcast(lobby);
	}

	bool Assign(uint8_t partySlot, uint8_t playerSlot)
	{
		// players may join later, until then the slot goes by the default
		if (mode != Mode::Host || !partySlot || partySlot > NET_PARTY_SLOTS || playerSlot > NET_MAX_PEERS) return false;
		assigned[partySlot] = playerSlot;
		Log(MESSAGE, "Net", "Party slot {} assigned to player slot {}.", partySlot, playerSlot);
		SendLobby();
		return true;
	}

	bool SetPermission(uint8_t slot, NetPermission perm, bool allowed)
	{
		if (mode != Mode::Host || slot == NET_HOST_SLOT || perm == NetPermission::Control || perm >= NetPermission::count) return false;
		permissions.Set(slot, perm, allowed);
		Log(MESSAGE, "Net", "Slot {}: {} permission {}.", slot, NetPermissionName(perm), allowed ? "granted" : "taken away");
		SendLobby();
		return true;
	}

	bool CanStart() const
	{
		return mode == Mode::Host && phase == NetPhase::Lobby && roster.AllReady();
	}

	bool StartGame()
	{
		if (!CanStart()) {
			Log(WARNING, "Net", "Not starting: {}.", phase != NetPhase::Lobby ? "the game runs already" : "not everyone is ready");
			return false;
		}
		phase = NetPhase::Game;
		size_t count = 0;
		for (auto& info : peers) {
			if (!info.welcomed) continue;
			info.wantsSave = true;
			count++;
		}
		Log(MESSAGE, "Net", "Starting the game for {} player(s); they get it once it runs.", count);
		SendLobby(true);
		return true;
	}

	// our own permissions: the host may do everything, clients what the lobby says
	bool MayI(NetPermission perm) const
	{
		if (mode == Mode::Host) return true;
		return myPermissions.Allowed(slot, perm, slot);
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
		// the next save carries all of it
		inventories.clear();
		piles.clear();
		stats.clear();
		spellbooks.clear();
		partyDirty = false;
		TrackItems(false);
	}

	// party members changing areas need a fresh save (all loaded areas, new IDs on the client)
	void TrackParty(const Map* map)
	{
		const Game* game = core->GetGame();
		std::map<ieDword, std::string> party;
		std::vector<ieDword> members;
		bool oneArea = true;
		for (int slot = 1; slot <= game->GetPartySize(false); ++slot) {
			const Actor* pc = game->FindPC(slot);
			if (!pc) continue;
			members.push_back(pc->GetGlobalID());
			if (pc->GetCurrentArea()) {
				party[pc->GetGlobalID()] = std::string(pc->GetCurrentArea()->GetScriptRef());
			}
			oneArea &= pc->GetCurrentArea() == map;
		}
		if (!partyAreas.empty() && (party != partyAreas || members != partyMembers)) {
			// joins, leaves and a new party order need a save; only moving, the area is enough
			partyChanged |= members != partyMembers;
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
		partyMembers = std::move(members);

		if (areaSettle && --areaSettle == 0) {
			bool light = lightAreas && !partyChanged && oneArea && !(map->AreaFlags & AF_NOSAVE);
			size_t count = 0;
			for (auto& info : peers) {
				if (!info.welcomed) continue;
				// players in sync get the area, the others (joining, loading) a save anyway
				if (light && info.live && !info.transfer && !info.wantsSave) {
					info.wantsArea = true;
				} else {
					info.wantsSave = true;
				}
				info.areaChangeAt = GetMilliseconds();
				count++;
			}
			Log(MESSAGE, "Net", "Party settled in {}: {} {} player(s).", map->GetScriptRef(), light ? "sending the area to" : "resyncing", count);
			partyChanged = false;
		}
	}

	// the party's area as an ARE file, the way a save stores it, and who is who in it
	bool TakeAreaSnapshot(NetAreaData& out, NetActorTable& table)
	{
		const Actor* first = GameReady();
		if (!first) return false;
		tick_t start = GetMilliseconds();
		Map* map = first->GetCurrentArea();
		auto mm = GetImporter<MapMgr>(IE_ARE_CLASS_ID);
		int size = mm ? mm->GetStoredFileSize(map) : 0;
		if (size <= 0 || size_t(size) > NET_MAX_AREA_SIZE) {
			Log(ERROR, "Net", "Unable to size area {} for the clients ({}).", map->GetScriptRef(), size);
			return false;
		}
		void* buffer = malloc(size);
		MemoryStream stream("netarea.are", buffer, size); // owns the buffer
		if (mm->PutArea(&stream, map) != GEM_OK) {
			Log(ERROR, "Net", "Unable to write area {} for the clients.", map->GetScriptRef());
			return false;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
		out.are.assign(bytes, bytes + size);
		out.area = std::string(map->GetScriptRef());
		const Game* game = core->GetGame();
		for (int slot = 1; slot <= game->GetPartySize(false); ++slot) {
			const Actor* pc = game->FindPC(slot);
			if (!pc) continue;
			out.party.push_back(NetAreaPartyEntry { pc->GetGlobalID(), uint8_t(slot), pc->Pos.x, pc->Pos.y, uint8_t(pc->GetOrientation()) });
		}
		for (auto& entry : ListActors()) {
			if (entry.area == out.area || entry.partySlot) table.actors.push_back(std::move(entry));
		}
		Log(MESSAGE, "Net", "Area data of {} written in {} ms: ARE {} bytes, {} party members, {} actors and objects.", out.area, GetMilliseconds() - start, size, out.party.size(), table.actors.size());
		return true;
	}

	// global variables the clients don't know yet; the GUI reads some (eg. the chapter)
	void TrackGlobals()
	{
		const Game* game = core->GetGame();
		if (!game) return;
		std::map<std::string, int32_t> now;
		for (const auto& var : game->locals) {
			now[std::string(var.first.c_str())] = int32_t(var.second);
		}
		NetGlobals msg;
		msg.vars = NetVariablesDiff(knownGlobals, now);
		knownGlobals = std::move(now);
		if (msg.vars.empty()) return;
		SendEvent(msg, true);
		std::string list;
		for (size_t i = 0; i < msg.vars.size() && i < 6; ++i) {
			list += fmt::format(" {}={}", msg.vars[i].name, msg.vars[i].value);
		}
		Log(MESSAGE, "Net", "Event: {} global(s) changed:{}{}", msg.vars.size(), list, msg.vars.size() > 6 ? " ..." : "");
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
		TrackItems(true);
		TrackStores();
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
		Heartbeats();
		SendLobby();
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
			TrackGlobals();
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
		if (!Capturing() || localMessage) return;
		if (guiCaller) {
			// what a remote player's GUI call says is for that player
			NetText msg;
			msg.text = Utf8(markup);
			Send(guiCaller->peer, msg);
			Log(MESSAGE, "Net", "Feedback for slot {}: '{}'", guiCaller->slot, PlainText(msg.text));
			return;
		}
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
		// GUI sounds of remote players' GUI calls played on their side already
		if (!Capturing() || resource.empty() || config.loop || guiCaller) return;
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

	void OnPause(bool paused, bool quiet)
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
		if (quiet) {
			// a store or inventory window: it shouldn't stop everyone else
			Log(MESSAGE, "Net", "Game {} here only (a window).", paused ? "paused" : "unpaused");
			return;
		}
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
					token = welcome.token;
					bool inGame = welcome.flags & NetWelcome::IN_GAME;
					Log(MESSAGE, "Net", "Welcomed by host '{}', we are slot {}{}. {}", welcome.hostName, slot, welcome.flags & NetWelcome::REJOINED ? " again (rejoined)" : "", inGame ? "Waiting for the game..." : "Waiting in the lobby.");
					SetStatus(inGame ? "Loading the host's game" : "Waiting for the host to start the game");
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
					SetStatus(fmt::format("Rejected: {}", reject.reason));
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
			case NetMsgType::Heartbeat:
				{
					NetHeartbeat beat;
					if (!NetDecode(data, size, beat)) return false;
					hostTime = beat.time;
					hostTimeAt = GetMilliseconds();
					if (beat.echo) rtt = RoundTrip(beat);
					return true;
				}
			case NetMsgType::LobbyState:
				{
					NetLobbyState msg;
					if (state == ClientState::Connecting || !NetDecode(data, size, msg)) return false;
					ApplyLobby(msg);
					return true;
				}
			case NetMsgType::AreaData:
				{
					NetAreaData msg;
					if (state == ClientState::Connecting || !NetDecode(data, size, msg)) return false;
					ApplyAreaData(msg);
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
				return ClientEvent<NetAreaChange>(data, size, true, [this](const NetAreaChange& msg) {
					areaChangeAt = GetMilliseconds();
					Log(MESSAGE, "Net", "The host's party moved to {}; the new area follows.", msg.area);
				});
			case NetMsgType::Globals:
				return ClientEvent<NetGlobals>(data, size, live, [this](const NetGlobals& msg) { ApplyGlobals(msg); });
			case NetMsgType::Ownership:
				return ClientEvent<NetOwnership>(data, size, true, [this](const NetOwnership& msg) { ApplyOwnership(msg); });
			case NetMsgType::Dialog:
				return ClientEvent<NetDialog>(data, size, live, [this](const NetDialog& msg) { ApplyDialog(msg); });
			case NetMsgType::Inventory:
				return ClientEvent<NetInventory>(data, size, live, [this](const NetInventory& msg) { ApplyInventory(msg); });
			case NetMsgType::ActorStats:
				return ClientEvent<NetActorStats>(data, size, live, [this](const NetActorStats& msg) { ApplyStats(msg); });
			case NetMsgType::PartyState:
				return ClientEvent<NetPartyState>(data, size, live, [this](const NetPartyState& msg) { ApplyParty(msg); });
			case NetMsgType::Spellbook:
				return ClientEvent<NetSpellbook>(data, size, live, [this](const NetSpellbook& msg) { ApplySpellbook(msg); });
			case NetMsgType::StoreState:
				return ClientEvent<NetStoreState>(data, size, live, [this](const NetStoreState& msg) { ApplyStore(msg); });
			case NetMsgType::ContainerOpen:
				return ClientEvent<NetContainerOpen>(data, size, live, [this](const NetContainerOpen& msg) { ApplyContainerOpen(msg); });
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
		hostStats.clear();
		clientDialog = ClientDialog();
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
		lightLoad = false;
		if (!stopped) {
			SetStatus("Lost the host, trying to reconnect");
			Log(MESSAGE, "Net", "Will keep trying to reach the host.");
		}
	}

	void ApplyLobby(const NetLobbyState& msg)
	{
		uint32_t before = myPermissions.Mask(slot);
		phase = msg.phase;
		for (const auto& player : msg.players) {
			if (player.slot != NET_HOST_SLOT) myPermissions.SetMask(player.slot, player.allowed);
		}
		bool first = lobby.players.empty();
		lobby = msg;
		lobbyRevision++;
		uint32_t now = myPermissions.Mask(slot);
		if (first || now != before) {
			std::string list;
			for (uint8_t raw = uint8_t(NetPermission::Purchase); raw < uint8_t(NetPermission::count); ++raw) {
				list += fmt::format(" {}{}", now & (1u << raw) ? "+" : "-", NetPermissionName(NetPermission(raw)));
			}
			Log(MESSAGE, "Net", "Lobby: {} player(s), {}; our permissions:{}.", msg.players.size(), phase == NetPhase::Lobby ? "waiting for the host" : "in game", list);
		}
	}

	void ApplyGlobals(const NetGlobals& msg)
	{
		Game* game = core->GetGame();
		for (const auto& var : msg.vars) {
			SetVariable(game, StringParam(var.name), ieDword(var.value), "GLOBAL");
		}
		Log(MESSAGE, "Net", "{} global(s) from the host{}.", msg.vars.size(), msg.vars.empty() ? "" : fmt::format(", eg. {}={}", msg.vars[0].name, msg.vars[0].value));
	}

	// can't follow with the area alone: ask for the whole game instead
	void AreaDataFailed(const std::string& why)
	{
		Log(WARNING, "Net", "Can't load the area data: {}; asking for a save instead.", why);
		NetResyncRequest request;
		request.reason = NetResyncRequest::MANUAL;
		if (server) Send(server, request);
	}

	// the party moved: load the new area from the host's copy and move our party members
	// into it, the way MoveBetweenAreas does; the actor table follows and maps it
	void ApplyAreaData(const NetAreaData& msg)
	{
		Game* game = core->GetGame();
		if (state != ClientState::InSync || !game || !GameReady()) {
			AreaDataFailed("we aren't in a game");
			return;
		}
		tick_t start = GetMilliseconds();
		ResRef area(msg.area.c_str());
		// our copy is stale; it can't be the one we look at, all of the party left that
		int idx = game->FindMap(area);
		if (idx >= 0 && (game->GetMap(unsigned(idx)) == game->GetCurrentArea() || game->DelMap(unsigned(idx), true) != 0)) {
			AreaDataFailed(fmt::format("our copy of {} can't be unloaded", area));
			return;
		}
		// what a save of ours had of it is replaced too
		core->saveGameAREExtractor.extractARE(area);
		FileStream file;
		if (!file.Create(area.c_str(), IE_ARE_CLASS_ID) || file.Write(msg.are.data(), msg.are.size()) != strret_t(msg.are.size())) {
			AreaDataFailed(fmt::format("can't write {} into the cache", area));
			return;
		}
		file.Close();
		for (const auto& entry : msg.party) {
			Actor* pc = game->FindPC(entry.partySlot);
			if (!pc) continue;
			MoveBetweenAreasCore(pc, area, Point(entry.x, entry.y), entry.orientation, false);
		}
		if (!game->GetMap(area, true)) {
			AreaDataFailed(fmt::format("{} doesn't load", area));
			return;
		}
		core->GetGameControl()->ChangeMap(game->FindPC(1), true);

		loadingTransfer = msg.transferID;
		haveTable = false;
		ForgetMappings();
		lightLoad = true;
		state = ClientState::Loading;
		Log(MESSAGE, "Net", "Area data #{}: loaded {} ({} bytes) and moved {} party member(s) there in {} ms.", msg.transferID, area, msg.are.size(), msg.party.size(), GetMilliseconds() - start);
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
		SetStatus("Loading the host's game");
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
		// whatever the host doesn't have, doesn't exist (in the areas it told us about)
		std::set<std::string> areas;
		for (const auto& entry : table.actors) {
			areas.insert(entry.area);
		}
		for (const auto& entry : local) {
			if (entry.kind != NetObjectKind::Actor || entry.partySlot || ids.Net(entry.netID) || !areas.count(entry.area)) continue;
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
		Log(MESSAGE, "Net", "In sync with the host: {} #{} loaded, replica mode on.", lightLoad ? "area data" : "save", ready.transferID);
		if (areaChangeAt) {
			Log(MESSAGE, "Net", "Area change done in {} ms ({}).", GetMilliseconds() - areaChangeAt, lightLoad ? "area data" : "full save");
			areaChangeAt = 0;
		}
		lightLoad = false;
		SetStatus("In game");
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
		EnforceAllStats();
		RefreshGUI();
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

	// dialog (client) ----------------------------------------------------------

	// the conversation shows up: its owner gets the dialog GUI, everyone else a transcript
	void EnterDialog(const NetDialog& msg)
	{
		if (clientDialog.active) return;
		GameControl* gc = core->GetGameControl();
		clientDialog = ClientDialog();
		clientDialog.active = true;
		clientDialog.owner = msg.owner;
		clientDialog.freeze = msg.flags & NetDialog::FREEZE;
		clientDialog.ui = msg.owner == slot;
		if (clientDialog.ui) {
			gc->SetDialogueFlags(DF_IN_DIALOG | (clientDialog.freeze ? DF_FREEZE_SCRIPTS : 0), BitOp::OR);
			core->ToggleViewsEnabled(false, "NOT_DLG");
			core->GetGUIScriptEngine()->RunFunction("GUIWORLD", "DialogStarted");
		} else if (clientDialog.freeze) {
			gc->SetDialogueFlags(DF_FREEZE_SCRIPTS, BitOp::OR);
		}
		Log(MESSAGE, "Net", "Dialog with net id {}: {}{}.", msg.targetID, clientDialog.ui ? "we answer" : fmt::format("player {} answers, we watch", msg.owner), clientDialog.freeze ? ", the world is frozen" : "");
	}

	void ApplyDialog(const NetDialog& msg)
	{
		GameControl* gc = core->GetGameControl();
		TextArea* ta = core->GetMessageTextArea();
		if (!gc || !ta) return;
		if (msg.kind == NetDialog::END) {
			if (!clientDialog.active) return;
			if (clientDialog.ui) {
				// the GUI half of DialogHandler::EndDialog
				ta->SetSpeakerPicture(nullptr);
				ta->ClearSelectOptions();
				ta->ClearDialogStart();
				core->ToggleViewsEnabled(true, "NOT_DLG");
				core->GetGUIScriptEngine()->RunFunction("GUIWORLD", "DialogEnded");
				core->GetGame()->SetControlStatus(CS_DIALOG, BitOp::NAND);
				gc->SetDialogueFlags(0, BitOp::SET);
			} else if (clientDialog.freeze) {
				gc->SetDialogueFlags(DF_FREEZE_SCRIPTS, BitOp::NAND);
			}
			core->SetEventFlag(EF_PORTRAIT);
			Log(MESSAGE, "Net", "Dialog ended (owned by slot {}).", clientDialog.owner);
			clientDialog = ClientDialog();
			return;
		}

		EnterDialog(msg);
		const Actor* speaker = LocalActor(msg.speakerID);
		if (msg.kind == NetDialog::LINE) {
			if (!(msg.flags & NetDialog::PARTY) && clientDialog.ui) {
				ta->MarkDialogStart();
				ta->SetSpeakerPicture(speaker ? speaker->CopyPortrait(1) : nullptr);
			}
			ta->AppendText(u"\n");
			String text = StringFromUtf8(msg.text.c_str());
			if (speaker) {
				displaymsg->DisplayStringName(std::move(text), msg.flags & NetDialog::PARTY ? GUIColors::DIALOGPARTY : GUIColors::DIALOG, speaker);
			} else {
				displaymsg->DisplayMarkupString(std::move(text));
			}
			Log(MESSAGE, "Net", "Dialog line of net id {}: '{}'", msg.speakerID, msg.text);
			return;
		}

		clientDialog.seq = msg.seq;
		clientDialog.options = msg.options;
		clientDialog.continueIndex = msg.continueIndex;
		clientDialog.buttons = msg.flags & (NetDialog::CONTINUE | NetDialog::END_BUTTON);
		std::vector<SelectOption> options;
		for (const auto& option : msg.options) {
			options.emplace_back(option.index, StringFromUtf8(option.text.c_str()));
		}
		if (clientDialog.ui) {
			ta->SetSelectOptions(options, true);
			ControlEventHandler handler = [this](const Control* c) {
				DialogChoiceHandled(int(c->GetValue()));
			};
			ta->SetAction(std::move(handler), TextArea::Action::Select);
			auto gs = core->GetGUIScriptEngine();
			if (clientDialog.buttons) {
				// what GetDialogOptions leaves for the continue/end button
				core->GetDictionary().Set("DialogOption", msg.continueIndex);
				gs->RunFunction("GUIWORLD", clientDialog.buttons & NetDialog::CONTINUE ? "OpenContinueMessageWindow" : "OpenEndMessageWindow");
			} else {
				gs->RunFunction("GUIWORLD", "NextDialogState");
			}
		} else {
			// read only: the replies as plain text
			int n = 1;
			for (const auto& option : msg.options) {
				displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format(READONLY_REPLY, n++, option.text).c_str()));
			}
			displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format("[p][color=A0A0A0FF]   (player {} is choosing)[/color][/p]", msg.owner).c_str()));
		}
		Log(MESSAGE, "Net", "Dialog node #{}: {} replies{}, {}.", msg.seq, msg.options.size(), clientDialog.buttons ? fmt::format(" and a {} button", clientDialog.buttons & NetDialog::CONTINUE ? "continue" : "end") : "", clientDialog.ui ? "ours to pick" : "read only");
	}

	// both sides: a reply picked in the GUI (the text area or the continue/end button)
	bool DialogChoiceHandled(int choice)
	{
		if (mode == Mode::Host) return HostChoiceHandled(choice);
		if (!clientDialog.active || !clientDialog.ui || !server) {
			Log(MESSAGE, "Net", "Ignoring reply {}: not our conversation.", choice);
			return true;
		}
		NetDialogChoice msg;
		msg.seq = clientDialog.seq;
		msg.index = uint16_t(choice);
		Send(server, msg);
		Log(MESSAGE, "Net", "Picked reply {} of dialog node #{}, sent to the host.", choice, msg.seq);
		return true;
	}

	// inventories, stats, spellbooks, gold, stores, containers (client) -------------

	static CREItem* MakeItem(const NetItem& entry)
	{
		CREItem* item = new CREItem();
		item->ItemResRef = ResRef(entry.resref.c_str());
		std::copy(entry.usages.begin(), entry.usages.end(), item->Usages.begin());
		item->Flags = entry.flags;
		item->Expired = entry.expired;
		const Item* itm = gamedata->GetItem(item->ItemResRef, true);
		if (itm) {
			item->MaxStackAmount = itm->MaxStackAmount;
			gamedata->FreeItem(itm, item->ItemResRef, false);
		}
		return item;
	}

	static bool SameItem(const CREItem* item, const NetItem& entry)
	{
		return item && ItemEntry(item, entry.slot) == entry;
	}

	void ApplyInventory(const NetInventory& msg)
	{
		if (msg.kind == NetObjectKind::Actor) {
			Actor* actor = LocalActor(msg.netID);
			if (!actor) return;
			ApplyActorInventory(actor, msg);
			Log(MESSAGE, "Net", "Inventory of net id {} ({}): {} items, equipped {}.", msg.netID, fmt::WideToChar { actor->GetShortName() }, msg.items.size(), msg.equipped);
			return;
		}
		Container* container = nullptr;
		if (msg.netID) {
			container = Scriptable::As<Container>(FindScriptable(ids.Local(msg.netID)));
		} else if (Map* map = FindLoadedMap(msg.area)) {
			// piles are named and found by their search map square, the same on both sides
			container = map->TMap->GetContainer(Point(msg.x, msg.y), IE_CONTAINER_PILE);
			if (!container && !msg.items.empty()) container = map->GetPile(Point(msg.x, msg.y));
		}
		if (!container) {
			if (!msg.items.empty()) Log(WARNING, "Net", "Inventory of container {} at ({}, {}): not here.", msg.netID, msg.x, msg.y);
			return;
		}
		while (container->inventory.GetSlotCount()) {
			delete container->RemoveItem(0, 0);
		}
		for (const auto& entry : msg.items) {
			container->AddItem(MakeItem(entry));
		}
		refreshGUI = true;
		Log(MESSAGE, "Net", "Inventory of {} '{}': {} items.", msg.netID ? "container" : "pile", container->GetScriptName(), msg.items.size());
	}

	void ApplyActorInventory(Actor* actor, const NetInventory& msg)
	{
		Inventory& inv = actor->inventory;
		std::map<uint16_t, const NetItem*> wanted;
		for (const auto& entry : msg.items) {
			wanted[entry.slot] = &entry;
		}
		std::vector<int> added;
		for (int slot = 0; slot < inv.GetSlotCount(); ++slot) {
			auto it = wanted.find(uint16_t(slot));
			const CREItem* have = inv.GetSlotItem(ieDword(slot));
			if (it == wanted.end() ? !have : SameItem(have, *it->second)) continue;
			// the engine's own removal, so equipped items come off properly
			if (have) delete inv.RemoveItem(unsigned(slot));
			if (it == wanted.end()) continue;
			inv.SetSlotItem(MakeItem(*it->second), unsigned(slot));
			added.push_back(slot);
		}
		// worn things show on the avatar; weapons follow the equipped slot below
		for (int slot : added) {
			int effect = core->QuerySlotEffects(unsigned(slot));
			if (effect && effect != SLOT_EFFECT_MELEE && effect != SLOT_EFFECT_MISSILE && effect != SLOT_EFFECT_MAGIC) {
				inv.EquipItem(ieDword(slot));
			}
		}
		if (inv.GetEquipped() != msg.equipped || inv.GetEquippedHeader() != msg.equippedHeader || !added.empty()) {
			inv.SetEquippedSlot(msg.equipped, msg.equippedHeader, true);
		}
		actor->ReinitQuickSlots();
		if (actor->InParty) {
			core->SetEventFlag(EF_ACTION);
			core->SetEventFlag(EF_PORTRAIT);
		}
		refreshGUI = true;
	}

	void ApplyStats(const NetActorStats& msg)
	{
		Actor* actor = LocalActor(msg.netID);
		if (!actor) return;
		auto& known = hostStats[actor->GetGlobalID()];
		for (const auto& entry : msg.stats) {
			known[entry.stat] = { entry.base, entry.modified };
		}
		EnforceStats(actor, known);
		if (actor->InParty) core->SetEventFlag(EF_PORTRAIT);
		refreshGUI = true;
		std::string list;
		for (size_t i = 0; i < msg.stats.size() && i < 6; ++i) {
			list += fmt::format(" {}={}/{}", msg.stats[i].stat, msg.stats[i].base, msg.stats[i].modified);
		}
		Log(MESSAGE, "Net", "{} stat(s) of net id {} ({}):{}{}", msg.stats.size(), msg.netID, fmt::WideToChar { actor->GetShortName() }, list, msg.stats.size() > 6 ? " ..." : "");
	}

	// the host's numbers win over whatever our copy worked out (eg. after a local RefreshEffects)
	static bool EnforceStats(Actor* actor, const std::map<uint16_t, std::pair<ieDword, ieDword>>& known)
	{
		bool changed = false;
		for (const auto& stat : known) {
			if (actor->GetBase(stat.first) != stat.second.first) {
				actor->SetBaseNoPCF(stat.first, stat.second.first);
				changed = true;
			}
			if (actor->GetStat(stat.first) != stat.second.second) {
				actor->SetStat(stat.first, stat.second.second, 0);
				changed = true;
			}
		}
		return changed;
	}

	void EnforceAllStats()
	{
		const Game* game = core->GetGame();
		if (!game) return;
		for (auto it = hostStats.begin(); it != hostStats.end();) {
			Actor* actor = game->GetActorByGlobalID(it->first);
			if (!actor) {
				it = hostStats.erase(it);
				continue;
			}
			EnforceStats(actor, it->second);
			++it;
		}
	}

	void ApplyParty(const NetPartyState& msg)
	{
		Game* game = core->GetGame();
		Log(MESSAGE, "Net", "Party gold {} -> {}, reputation {}.", game->PartyGold, msg.gold, msg.reputation);
		game->PartyGold = msg.gold;
		game->Reputation = msg.reputation;
		core->SetEventFlag(EF_PORTRAIT);
		refreshGUI = true;
	}

	void ApplySpellbook(const NetSpellbook& msg)
	{
		Actor* actor = LocalActor(msg.netID);
		if (!actor) return;
		Spellbook& book = actor->spellbook;
		for (const auto& page : msg.pages) {
			CRESpellMemorization* sm = book.GetSpellMemorization(page.type, page.level);
			if (!sm) continue;
			sm->SlotCount = page.slots;
			sm->SlotCountWithBonus = page.slotsWithBonus;
			sm->knownSpells.clear();
			for (const auto& known : page.known) {
				auto spell = MakeHolder<CREKnownSpell>();
				spell->SpellResRef = ResRef(known.c_str());
				spell->Level = page.level;
				spell->Type = page.type;
				sm->knownSpells.push_back(std::move(spell));
			}
			sm->memorizedSpells.clear();
			for (const auto& memorized : page.memorized) {
				auto spell = MakeHolder<CREMemorizedSpell>();
				spell->SpellResRef = ResRef(memorized.resref.c_str());
				spell->Flags = memorized.flags;
				sm->memorizedSpells.push_back(std::move(spell));
			}
		}
		book.ClearSpellInfo();
		if (actor->InParty) core->SetEventFlag(EF_ACTION);
		refreshGUI = true;
		Log(MESSAGE, "Net", "Spellbook of net id {} ({}): {} pages, {} spells ready.", msg.netID, fmt::WideToChar { actor->GetShortName() }, msg.pages.size(), CountReady(msg));
	}

	static void ApplyStoreItems(Store* store, const std::vector<NetStoreItem>& items)
	{
		std::vector<STOItem*> stock;
		for (const auto& entry : items) {
			STOItem* item = new STOItem();
			item->ItemResRef = ResRef(entry.resref.c_str());
			std::copy(entry.usages.begin(), entry.usages.end(), item->Usages.begin());
			item->Flags = entry.flags;
			item->AmountInStock = entry.amount;
			item->InfiniteSupply = entry.infinite;
			item->PurchasedAmount = entry.purchased;
			const Item* itm = gamedata->GetItem(item->ItemResRef, true);
			if (itm) {
				item->Weight = itm->Weight;
				item->MaxStackAmount = itm->MaxStackAmount;
				gamedata->FreeItem(itm, item->ItemResRef, false);
			}
			stock.push_back(item);
		}
		for (const STOItem* item : store->items) {
			delete item;
		}
		store->items = std::move(stock);
	}

	void ApplyStore(const NetStoreState& msg)
	{
		Store* store = core->GetCurrentStore();
		ResRef name(msg.store.c_str());
		if (msg.flags & NetStoreState::OPEN) {
			Actor* pc = LocalActor(msg.actorID);
			if (pc && pc->InParty) core->GetGame()->SelectedSingle = pc->InParty;
			store = core->SetCurrentStore(name, ids.Local(msg.ownerID));
		}
		if (!store || store->Name != name) {
			// not open here: update our cached copy, for later
			store = gamedata->GetStore(name);
			if (!store) return;
		}
		ApplyStoreItems(store, msg.items);
		if (msg.flags & NetStoreState::OPEN) {
			core->SetEventFlag(EF_OPENSTORE);
		} else {
			refreshGUI = true;
		}
		Log(MESSAGE, "Net", "Store {}{}: {} wares.", msg.store, msg.flags & NetStoreState::OPEN ? " opens for us" : " changed", msg.items.size());
	}

	void ApplyContainerOpen(const NetContainerOpen& msg)
	{
		Actor* actor = LocalActor(msg.actorID);
		Map* map = actor ? actor->GetCurrentArea() : nullptr;
		Container* container = nullptr;
		if (msg.containerID) {
			container = Scriptable::As<Container>(FindScriptable(ids.Local(msg.containerID)));
		} else if (map) {
			container = map->GetPile(Point(msg.x, msg.y));
		}
		if (!actor || !container) {
			Log(WARNING, "Net", "The host opened container {} for net id {}, which isn't here.", msg.containerID, msg.actorID);
			return;
		}
		// the container window works on the first selected PC
		if (core->GetFirstSelectedPC(false) != actor) SelectOnly(actor);
		core->SetCurrentContainer(actor, container, true);
		Log(MESSAGE, "Net", "The host opened container '{}' ({} items) for net id {} ({}).", container->GetScriptName(), container->inventory.GetSlotCount(), msg.actorID, fmt::WideToChar { actor->GetShortName() });
	}

	// open windows show what the host sent
	void RefreshGUI()
	{
		if (!refreshGUI || !core->GetGame()) return;
		refreshGUI = false;
		static const std::string code = R"(import sys
import GemRB
_m = sys.modules
try:
	if "Container" in _m and _m["Container"].ContainerWindow:
		_m["Container"].UpdateContainerWindow ()
	if "GUIINV" in _m and getattr (_m["GUIINV"], "InventoryWindow", None):
		_m["GUIINV"].RefreshInventoryWindow ()
	if "GUISTORE" in _m and GemRB.GetView ("WINSHOP"):
		_m["GUISTORE"].UpdateStoreShoppingWindow (GemRB.GetView ("WINSHOP"))
except Exception as e:
	print ("[Net] GUI refresh failed:", e)
)";
		core->GetGUIScriptEngine()->ExecString(code);
	}

	bool ForwardUseContainer(const Actor& actor, const Container& container)
	{
		if (state != ClientState::InSync || !server) return false;
		NetCommand cmd;
		cmd.actorID = ids.Net(actor.GetGlobalID());
		cmd.action = "usecontainer";
		if (IsPile(&container)) {
			cmd.x = container.Pos.x;
			cmd.y = container.Pos.y;
		} else {
			cmd.int0 = int32_t(ids.Net(container.GetGlobalID()));
			if (!cmd.int0) return false;
		}
		if (!cmd.actorID) return false;
		Send(server, cmd);
		Log(MESSAGE, "Net", "Forwarded UseContainer('{}') for net id {}.", container.GetScriptName(), cmd.actorID);
		return true;
	}

	bool ForwardRegionClick(const Actor& actor, const InfoPoint& region)
	{
		if (state != ClientState::InSync || !server) return false;
		NetCommand cmd;
		cmd.actorID = ids.Net(actor.GetGlobalID());
		cmd.action = "clickregion";
		cmd.int0 = int32_t(ids.Net(region.GetGlobalID()));
		if (!cmd.actorID || !cmd.int0) return false;
		Send(server, cmd);
		Log(MESSAGE, "Net", "Forwarded a click on info point '{}' for net id {}.", region.GetScriptName(), cmd.actorID);
		return true;
	}

	// the party member a GUI call is about, here
	static const Actor* GuiCallActor(const NetGuiOpInfo& opInfo, const std::vector<int32_t>& args)
	{
		const Game* game = core->GetGame();
		if (!game || opInfo.actorArg < 0 || size_t(opInfo.actorArg) >= args.size()) return nullptr;
		int32_t arg = args[size_t(opInfo.actorArg)];
		if (arg > 1000) return game->GetActorByGlobalID(ieDword(arg));
		return arg > 0 ? game->FindPC(arg) : nullptr;
	}

	// tell our player, at most every couple of seconds per party member
	void RefuseActor(const Actor& actor, uint8_t owner)
	{
		tick_t now = GetMilliseconds();
		tick_t& last = refusals[actor.GetGlobalID()];
		if (last && now - last < REFUSAL_THROTTLE) return;
		last = now;
		Log(MESSAGE, "Net", "Refused locally: {} (id {}) is controlled by player {}.", fmt::WideToChar { actor.GetShortName() }, actor.GetGlobalID(), owner);
		ShowLocal(fmt::format("{} is controlled by player {}.", fmt::WideToChar { actor.GetShortName() }, owner));
	}

	// for our own player only
	void ShowLocal(const std::string& text)
	{
		localMessage = true;
		displaymsg->DisplayMarkupString(StringFromUtf8(fmt::format("[p][color=FF8080FF]{}[/color][/p]", text).c_str()));
		localMessage = false;
	}

	void RefusePermission(NetPermission perm)
	{
		Log(MESSAGE, "Net", "Refused locally: no {} permission.", NetPermissionName(perm));
		ShowLocal(fmt::format("The host doesn't let you do that (no {} permission).", NetPermissionName(perm)));
	}

	// who controls a party member, as far as we know; -1 when we can't tell
	int Controller(const Actor& actor) const
	{
		if (!actor.InParty) return -1;
		if (mode == Mode::Host) return OwnerOf(&actor);
		auto it = myOwners.find(ids.Net(actor.GetGlobalID()));
		return it == myOwners.end() ? -1 : it->second;
	}

	bool MayOrder(const Actor& actor)
	{
		if (mode == Mode::Client && state != ClientState::InSync) return true;
		int owner = Controller(actor);
		uint8_t me = mode == Mode::Host ? NET_HOST_SLOT : slot;
		if (owner < 0 || owner == me) return true;
		RefuseActor(actor, uint8_t(owner));
		return false;
	}

	// a party swap keeps players with their characters, not with the slots
	void SwapAssignments(int a, int b)
	{
		if (a < 1 || b < 1 || a > NET_PARTY_SLOTS || b > NET_PARTY_SLOTS || a == b) return;
		uint8_t ownerA = NetSlotOwner(uint8_t(a), assigned, roster);
		uint8_t ownerB = NetSlotOwner(uint8_t(b), assigned, roster);
		assigned[uint8_t(a)] = ownerB;
		assigned[uint8_t(b)] = ownerA;
		Log(MESSAGE, "Net", "Party slots {} and {} swap places; their players go with them.", a, b);
	}

	bool GuiCall(NetGuiOp op, std::vector<int32_t> args, const std::string& text)
	{
		NetGuiOpInfo opInfo = NetGuiInfo(op);
		if (mode == Mode::Host) {
			if (op == NetGuiOp::SwapPCs && args.size() >= 2) SwapAssignments(args[0], args[1]);
			// a remote player's call, checked already
			if (guiCaller) return true;
			const Actor* actor = GuiCallActor(opInfo, args);
			return !actor || MayOrder(*actor);
		}
		if (state != ClientState::InSync || !server) return true;
		const Actor* actor = GuiCallActor(opInfo, args);
		if (actor && !MayOrder(*actor)) return false;
		if (opInfo.permission != NetPermission::Control && !MayI(opInfo.permission)) {
			RefusePermission(opInfo.permission);
			return false;
		}
		ForwardGuiCall(op, std::move(args), text);
		return !opInfo.hostOnly;
	}

	void ForwardGuiCall(NetGuiOp op, std::vector<int32_t> args, const std::string& text)
	{
		if (mode != Mode::Client || state != ClientState::InSync || !server) return;
		NetGuiOpInfo opInfo = NetGuiInfo(op);
		NetGuiCall call;
		call.op = op;
		call.selected = uint8_t(core->GetGame()->GetSelectedPCSingle());
		call.args = std::move(args);
		call.text = text;
		if (opInfo.actorArg >= 0 && size_t(opInfo.actorArg) < call.args.size() && call.args[size_t(opInfo.actorArg)] > 1000) {
			int32_t& actor = call.args[size_t(opInfo.actorArg)];
			uint32_t netID = ids.Net(ieDword(actor));
			if (!netID) {
				Log(WARNING, "Net", "Not forwarding GUI call {}: actor {} has no network ID.", opInfo.name, actor);
				return;
			}
			actor = int32_t(netID);
		}
		Send(server, call);
		std::string list;
		for (int32_t arg : call.args) {
			list += fmt::format("{}{}", list.empty() ? "" : ", ", arg);
		}
		Log(MESSAGE, "Net", "Forwarded GUI call {}({}){}, selected PC {}.", opInfo.name, list, call.text.empty() ? "" : fmt::format(" '{}'", call.text), call.selected);
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
		if (ActionName(action.actionID) == "nidspecial1" && !MayI(NetPermission::Dialog)) {
			RefusePermission(NetPermission::Dialog);
			return false;
		}
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
		if (!MayI(NetPermission::AreaTransition)) {
			RefusePermission(NetPermission::AreaTransition);
			return false;
		}
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
			Assign(uint8_t(atoi(arg1.c_str())), uint8_t(atoi(arg2.c_str())));
		} else if (verb == "perm") {
			// perm SLOT PERMISSION 0|1, PERMISSION as NetPermissionName with _ for spaces
			std::string permName = arg2.substr(0, arg2.find(' '));
			std::replace(permName.begin(), permName.end(), '_', ' ');
			for (uint8_t raw = 0; raw < uint8_t(NetPermission::count); ++raw) {
				if (permName != NetPermissionName(NetPermission(raw))) continue;
				permissions.Set(uint8_t(atoi(arg1.c_str())), NetPermission(raw), arg2.back() == '1');
				Log(MESSAGE, "Net", "Test hook: slot {} {} {}.", arg1, arg2.back() == '1' ? "may now" : "may no longer", permName);
			}
		} else if (verb == "talk") {
			// the GUI doesn't let anyone click while a conversation is up
			const Actor* target = FindActorNamed(arg);
			if (gc->InDialog()) {
				Log(MESSAGE, "Net", "Test hook: not talking, a conversation is up.");
			} else if (selected && target) {
				gc->TryToTalk(selected, target);
			}
		} else if (verb == "npctalk") {
			// npctalk NAME N: what a creature script's Dialogue(PlayerN) does
			Actor* actor = FindActorNamed(arg1);
			if (actor) actor->AddAction(GenerateAction(fmt::format("Dialogue(Player{})", arg2)));
		} else if (verb == "reply") {
			// the Nth reply in the message window, as a click (or number key) picks it
			TextArea* ta = core->GetMessageTextArea();
			if (ta) ta->SelectAvailableOption(size_t(atoi(arg.c_str()) - 1));
		} else if (verb == "forcereply") {
			// transition N, bypassing the GUI: what a player without the dialog GUI could still send
			DialogChoiceHandled(atoi(arg.c_str()));
		} else if (verb == "continue") {
			// the continue / end button
			core->GetGUIScriptEngine()->ExecString("import GUIWORLD\nGUIWORLD.CloseContinueWindow ()");
		} else if (verb == "container") {
			Container* container = game->GetCurrentArea()->GetTileMap()->GetContainer(ieVariable(arg.c_str()));
			if (selected && container) gc->HandleContainer(container, selected);
		} else if (verb == "region") {
			InfoPoint* region = game->GetCurrentArea()->GetTileMap()->GetInfoPoint(ieVariable(arg.c_str()));
			if (selected && region) gc->HandleActiveRegion(region, selected, region->Pos);
		} else if (verb == "gui") {
			// GUI script code, | for line breaks: the GemRB.* calls the windows make
			std::string code = "try:\n\timport GemRB\n\t" + arg + "\nexcept Exception as e:\n\tprint ('[Net] test hook code failed:', repr (e))\n";
			size_t bar;
			while ((bar = code.find('|')) != std::string::npos) {
				code.replace(bar, 1, "\n\t");
			}
			core->GetGUIScriptEngine()->ExecString(code);
		} else if (verb == "report") {
			Report();
		} else if (verb == "drop" && mode == Mode::Client && server) {
			// the connection goes away without a word, as with a crash or a cable pulled
			enet_peer_disconnect_now(server, 0);
			server = nullptr;
			LeaveGame("Test hook: dropped our connection to the host.");
			nextConnectAttempt = GetMilliseconds() + RECONNECT_DELAY;
		} else if (verb == "rawgui" && mode == Mode::Client && server) {
			// rawgui OP ARGS...: a GUI call straight to the host, past our own checks (actors as party slots)
			NetGuiCall call;
			call.op = NetGuiOp(atoi(arg1.c_str()));
			call.selected = uint8_t(game->GetSelectedPCSingle());
			for (size_t pos = arg.find(' '); pos != std::string::npos; pos = arg.find(' ', pos + 1)) {
				call.args.push_back(atoi(arg.c_str() + pos + 1));
			}
			Send(server, call);
		} else if (verb == "rawexit" && mode == Mode::Client && server && selected) {
			// rawexit REGION: UseExit straight to the host, past our own permission check
			const InfoPoint* exit = game->GetCurrentArea()->GetTileMap()->GetInfoPoint(ieVariable(arg.c_str()));
			NetCommand cmd;
			cmd.actorID = ids.Net(selected->GetGlobalID());
			cmd.action = "useexit";
			cmd.int0 = exit ? int32_t(ids.Net(exit->GetGlobalID())) : 0;
			Send(server, cmd);
		} else if (verb == "hang") {
			// no polling and no heartbeats for that many seconds, like a hung process
			hangUntil = GetMilliseconds() + tick_t(atoi(arg.c_str())) * 1000;
			Log(MESSAGE, "Net", "Test hook: not listening to the network for {} s.", atoi(arg.c_str()));
		} else if (verb == "lobby") {
			Log(MESSAGE, "Net", "Test hook: lobby is {}.", Utf8(StringFromUtf8(LobbyLine().c_str())));
		} else {
			Log(WARNING, "Net", "Unknown test order '{}'.", order);
		}
	}

	std::string LobbyLine() const
	{
		std::string line = fmt::format("{}, we are slot {}", phase == NetPhase::Lobby ? "waiting" : "in game", mode == Mode::Host ? NET_HOST_SLOT : slot);
		for (const auto& slotEntry : lobby.party) {
			line += fmt::format("; PC {} '{}' owner {} controller {}", slotEntry.partySlot, slotEntry.name, slotEntry.owner, slotEntry.controller);
		}
		return line;
	}

	// NetTestOrders "report": what both sides should agree on, for comparing logs
	void Report() const
	{
		const Game* game = core->GetGame();
		const char* side = mode == Mode::Host ? "host" : "client";
		Log(MESSAGE, "Net", "Report ({}): party gold {}, reputation {}.", side, game->PartyGold, game->Reputation);
		for (int pcSlot = 1; pcSlot <= game->GetPartySize(false); ++pcSlot) {
			const Actor* pc = game->FindPC(pcSlot);
			if (!pc) continue;
			NetInventory inv = InventoryMessage(pc);
			std::string items;
			for (const auto& item : inv.items) {
				items += fmt::format(" {}:{}x{}", item.slot, item.resref, item.usages[0]);
			}
			NetSpellbook book = SpellbookMessage(pc);
			Log(MESSAGE, "Net", "Report ({}): PC {} {} XP {} hp {} equipped {}, {} spells ready, items:{}", side, pcSlot, fmt::WideToChar { pc->GetShortName() }, pc->GetStat(IE_XP), pc->GetStat(IE_HITPOINTS), inv.equipped, CountReady(book), items);
		}
		for (const auto& holder : game->GetCurrentArea()->GetTileMap()->GetContainers()) {
			const Container* container = holder.get();
			std::string items;
			for (int i = 0; i < container->inventory.GetSlotCount(); ++i) {
				const CREItem* item = container->inventory.GetSlotItem(ieDword(i));
				items += fmt::format(" {}x{}", item->ItemResRef, item->Usages[0]);
			}
			Log(MESSAGE, "Net", "Report ({}): {} '{}' at {}:{}", side, IsPile(container) ? "pile" : "container", container->GetScriptName(), container->Pos, items.empty() ? " empty" : items);
		}
		const Store* store = gamedata->GetStore(ResRef("MPTSTORE"));
		if (store) {
			std::string wares;
			for (const STOItem* item : store->items) {
				wares += fmt::format(" {}x{}", item->ItemResRef, item->AmountInStock);
			}
			Log(MESSAGE, "Net", "Report ({}): store MPTSTORE:{}", side, wares);
		}
		ReportTestVar(side);
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

std::unique_ptr<NetSession> NetSession::Create(const CoreSettings& config, bool lobby)
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

	auto impl = std::make_unique<Impl>(mode, config, lobby);
	if (!impl->Start()) return nullptr;
	return std::unique_ptr<NetSession>(new NetSession(mode, std::move(impl)));
}

bool NetSession::Available()
{
	return true;
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
			impl->RunGuiCalls();
			impl->RunTestOrders();
		}
	}
}

void NetSession::PostTick()
{
	if (mode == Mode::Host) {
		impl->capturing = false;
		impl->HostPostTick();
	} else {
		impl->Heartbeats();
	}
	if (!impl->Hanging()) enet_host_flush(impl->host);
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

void NetSession::OnPause(bool paused, bool quiet)
{
	impl->OnPause(paused, quiet);
}

void NetSession::OnDialogLine(const Scriptable* speaker, const String& text, bool party)
{
	impl->OnDialogLine(speaker, text, party);
}

void NetSession::OnDialogState(const std::vector<std::pair<int, String>>& options)
{
	impl->OnDialogState(options);
}

void NetSession::OnDialogEnd()
{
	impl->OnDialogEnd();
}

bool NetSession::DialogChoiceHandled(int choice)
{
	return impl->DialogChoiceHandled(choice);
}

bool NetSession::OnStartStore(const ResRef& store, const Scriptable& merchant)
{
	return impl->OnStartStore(store, merchant);
}

void NetSession::OnContainerUsed(const Actor& actor, Container& container)
{
	impl->OnContainerUsed(actor, container);
}

bool NetSession::ForwardUseContainer(const Actor& actor, const Container& container)
{
	return impl->ForwardUseContainer(actor, container);
}

bool NetSession::ForwardRegionClick(const Actor& actor, const InfoPoint& region)
{
	return impl->ForwardRegionClick(actor, region);
}

bool NetSession::GuiCall(NetGuiOp op, std::vector<int32_t> args, const std::string& text)
{
	return impl->GuiCall(op, std::move(args), text);
}

bool NetSession::MayOrder(const Actor& actor)
{
	return impl->MayOrder(actor);
}

NetPhase NetSession::GetPhase() const
{
	return impl->phase;
}

const NetLobbyState& NetSession::GetLobby() const
{
	return impl->lobby;
}

uint32_t NetSession::GetLobbyRevision() const
{
	return impl->lobbyRevision;
}

uint8_t NetSession::GetSlot() const
{
	return mode == Mode::Host ? NET_HOST_SLOT : impl->slot;
}

const std::string& NetSession::GetStatus() const
{
	return impl->status;
}

const std::vector<std::string>& NetSession::GetChatLog() const
{
	return impl->chatLog;
}

bool NetSession::Allowed(NetPermission perm) const
{
	return impl->MayI(perm);
}

void NetSession::SetReady(bool ready)
{
	if (mode == Mode::Host) {
		impl->roster.SetReady(NET_HOST_SLOT, ready);
		Log(MESSAGE, "Net", "We (the host) are {}.", ready ? "ready" : "not ready");
		impl->SendLobby();
	} else if (impl->server && impl->state != Impl::ClientState::Connecting) {
		NetLobbyReady msg;
		msg.ready = ready;
		impl->Send(impl->server, msg);
		Log(MESSAGE, "Net", "Told the host we are {}.", ready ? "ready" : "not ready");
	}
}

bool NetSession::Assign(uint8_t partySlot, uint8_t playerSlot)
{
	return impl->Assign(partySlot, playerSlot);
}

bool NetSession::SetPermission(uint8_t slot, NetPermission perm, bool allowed)
{
	return impl->SetPermission(slot, perm, allowed);
}

bool NetSession::StartGame()
{
	return impl->StartGame();
}

bool NetSession::CanStart() const
{
	return impl->CanStart();
}

#else // HAVE_ENET

struct NetSession::Impl {};

std::unique_ptr<NetSession> NetSession::Create(const CoreSettings& config, bool)
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
void NetSession::OnPause(bool, bool) {}
void NetSession::OnDialogLine(const Scriptable*, const String&, bool) {}
void NetSession::OnDialogState(const std::vector<std::pair<int, String>>&) {}
void NetSession::OnDialogEnd() {}

bool NetSession::DialogChoiceHandled(int)
{
	return false;
}

bool NetSession::OnStartStore(const ResRef&, const Scriptable&)
{
	return false;
}

void NetSession::OnContainerUsed(const Actor&, Container&) {}

bool NetSession::ForwardUseContainer(const Actor&, const Container&)
{
	return false;
}

bool NetSession::ForwardRegionClick(const Actor&, const InfoPoint&)
{
	return false;
}

bool NetSession::GuiCall(NetGuiOp, std::vector<int32_t>, const std::string&)
{
	return true;
}

bool NetSession::MayOrder(const Actor&)
{
	return true;
}

bool NetSession::Available()
{
	return false;
}

NetPhase NetSession::GetPhase() const
{
	return NetPhase::Lobby;
}

const NetLobbyState& NetSession::GetLobby() const
{
	static const NetLobbyState none;
	return none;
}

uint32_t NetSession::GetLobbyRevision() const
{
	return 0;
}

uint8_t NetSession::GetSlot() const
{
	return NET_HOST_SLOT;
}

const std::string& NetSession::GetStatus() const
{
	static const std::string none;
	return none;
}

const std::vector<std::string>& NetSession::GetChatLog() const
{
	static const std::vector<std::string> none;
	return none;
}

bool NetSession::Allowed(NetPermission) const
{
	return true;
}

void NetSession::SetReady(bool) {}

bool NetSession::Assign(uint8_t, uint8_t)
{
	return false;
}

bool NetSession::SetPermission(uint8_t, NetPermission, bool)
{
	return false;
}

bool NetSession::StartGame()
{
	return false;
}

bool NetSession::CanStart() const
{
	return false;
}

#endif // HAVE_ENET

NetSession::NetSession(Mode mode, std::unique_ptr<Impl> impl)
	: mode(mode), impl(std::move(impl))
{
}

NetSession::~NetSession() = default;

}
