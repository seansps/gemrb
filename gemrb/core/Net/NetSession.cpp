// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/NetSession.h"

#include "config.h"

#include "DisplayMessage.h"
#include "Game.h"
#include "Interface.h"
#include "InterfaceConfig.h"
#include "Map.h"
#include "SaveGame.h"
#include "SaveGameIterator.h"

#include "GUI/GameControl.h"
#include "GameScript/GSUtils.h"
#include "GameScript/GameScript.h"
#include "Logging/Logging.h"
#include "Net/Protocol.h"
#include "Net/Resync.h"
#include "Scriptable/Actor.h"
#include "Streams/FileStream.h"
#include "Strings/StringConversion.h"
#include "System/VFS.h"

#ifdef HAVE_ENET
	#include <enet/enet.h>
#endif

#include <algorithm>
#include <map>
#include <vector>

namespace GemRB {

#ifdef HAVE_ENET

// how often (in game loop ticks) the host sends actor snapshots; ticks run at ~15 Hz
static constexpr unsigned int SNAPSHOT_INTERVAL = 1;
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
};

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
	// network ID (the host's global ID) <-> local global ID
	std::map<uint32_t, ieDword> hostToLocal;
	std::map<ieDword, uint32_t> localToHost;

	// test hooks
	std::string testChat;
	std::string testMove;
	std::string testVar;
	int testResync = 0;
	bool slowTransfer = false;
	unsigned int syncedTicks = 0;

	unsigned int ticks = 0;
	std::map<uint32_t, Point> lastLoggedPos;

	Impl(Mode mode, const CoreSettings& config)
		: mode(mode), name(config.NetName), remoteHost(config.NetHost), port(uint16_t(config.NetPort)),
		  testChat(config.NetTestChat), testMove(config.NetTestMove), testVar(config.NetTestVar),
		  testResync(config.NetTestResync), slowTransfer(config.NetTestSlowTransfer != 0)
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

	template<typename MSG>
	static void Send(ENetPeer* peer, const MSG& msg, NetChannel channel = NetChannel::Reliable)
	{
		const auto data = NetEncode(msg);
		enet_uint32 flags = channel == NetChannel::State ? 0 : ENET_PACKET_FLAG_RELIABLE;
		ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
		enet_peer_send(peer, enet_uint8(channel), packet);
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

	// every actor in every loaded area; netID is the local global ID
	static std::vector<NetActorTableEntry> ListActors()
	{
		std::vector<NetActorTableEntry> list;
		const Game* game = core->GetGame();
		for (size_t idx = 0; idx < game->GetLoadedMapCount(); ++idx) {
			const Map* map = game->GetMap(unsigned(idx));
			std::string area(map->GetScriptRef());
			for (const Actor* actor : map->GetAllActors()) {
				NetActorTableEntry entry;
				entry.netID = actor->GetGlobalID();
				entry.partySlot = actor->InParty;
				entry.area = area;
				entry.scriptName = std::string(actor->GetScriptName());
				entry.x = actor->Pos.x;
				entry.y = actor->Pos.y;
				list.push_back(std::move(entry));
			}
		}
		return list;
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
					Log(MESSAGE, "Net", "Slot {} ({}) loaded save #{} and mapped {} actors.", info->slot, info->name, ready.transferID, ready.mapped);
					return true;
				}
			case NetMsgType::ResyncRequest:
				{
					NetResyncRequest request;
					if (!info->welcomed || !NetDecode(data, size, request)) return false;
					if (!info->transfer) {
						info->wantsSave = true;
						Log(MESSAGE, "Net", "Slot {} ({}) asked for a resync.", info->slot, info->name);
					}
					return true;
				}
			default:
				return false;
		}
	}

	static bool IsAllowedAction(const std::string& action)
	{
		// M1.3 forwards movement only
		static const char* const allowed[] = { "MoveToPoint(", "RunToPoint(", "AddWayPoint(" };
		for (const char* prefix : allowed) {
			if (action.compare(0, strlen(prefix), prefix) == 0) return true;
		}
		return false;
	}

	void HandleCommand(const NetPeerInfo& info, const NetCommand& cmd)
	{
		Game* game = core->GetGame();
		// network IDs are our own global IDs
		Actor* actor = game ? game->GetActorByGlobalID(cmd.actorID) : nullptr;
		// TODO: M1.5 check the per-character owner slot instead of plain party membership
		if (!actor || !actor->InParty) {
			Log(WARNING, "Net", "Slot {}: rejected command for unknown or non-party actor {}.", info.slot, cmd.actorID);
			return;
		}
		if (!IsAllowedAction(cmd.action)) {
			Log(WARNING, "Net", "Slot {}: rejected disallowed action '{}'.", info.slot, cmd.action);
			return;
		}
		Holder<Action> action = GenerateAction(cmd.action);
		if (!action) {
			Log(WARNING, "Net", "Slot {}: rejected unparsable action '{}'.", info.slot, cmd.action);
			return;
		}

		bool append = cmd.flags & NetCommand::APPEND;
		// mirror GameControl::CommandSelectedMovement
		if (!append) {
			actor->Stop();
		}
		actor->CommandActor(std::move(action), !append);
		Log(MESSAGE, "Net", "Slot {} ({}): injected '{}' for {} (id {}) at {}.", info.slot, info.name, cmd.action, fmt::WideToChar { actor->GetShortName() }, cmd.actorID, actor->Pos);
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
		Log(MESSAGE, "Net", "Snapshot of the game written in {} ms: {} files,{}; {} actors.", GetMilliseconds() - start, files.size(), fileList, out.table.actors.size());
		size_t logged = 0;
		for (const auto& entry : out.table.actors) {
			if (logged++ == MAX_LOGGED_ACTORS) break;
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
			Log(MESSAGE, "Net", "Sent save #{} and its table of {} actors to slot {}.", info.table.transferID, info.table.actors.size(), info.slot);
			info.transfer.reset();
			info.table = NetActorTable();
		}
	}

	void HostPostTick()
	{
		if (GameReady()) {
			hostGameTicks++;
			if (hostGameTicks == TEST_HOOK_DELAY) {
				RunTestVar();
				RunTestMove();
			}
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
		for (const Actor* actor : area->GetAllActors()) {
			if (state.actors.size() == NET_MAX_ENTRIES) break;
			NetActorStateEntry entry;
			entry.netID = actor->GetGlobalID();
			entry.x = actor->Pos.x;
			entry.y = actor->Pos.y;
			entry.orientation = uint8_t(actor->GetOrientation());
			entry.stance = actor->GetStance();
			state.actors.push_back(entry);
			LogMovement("host", entry);
			if (state.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Sync check #{}: net id {} at ({}, {}) stance {}", state.tick, entry.netID, entry.x, entry.y, entry.stance);
			}
		}
		Broadcast(state, NetChannel::State);
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
				return false;
		}
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
		hostToLocal.clear();
		localToHost.clear();
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
		hostToLocal.clear();
		localToHost.clear();
		state = ClientState::Loading;
		receiver.Reset();
	}

	// once the engine has loaded and entered the save, pair our actors with the host's
	void FinishLoad()
	{
		if (!haveTable || !GameReady()) return;

		std::vector<NetActorTableEntry> local = ListActors();
		auto matches = NetMatchActors(table.actors, local, MATCH_DISTANCE);
		for (const auto& match : matches) {
			hostToLocal[match.first] = match.second;
			localToHost[match.second] = match.first;
		}

		std::map<ieDword, const NetActorTableEntry*> localByID;
		for (const auto& entry : local) {
			localByID[entry.netID] = &entry;
		}
		size_t logged = 0;
		for (const auto& entry : table.actors) {
			if (logged++ == MAX_LOGGED_ACTORS) break;
			auto it = hostToLocal.find(entry.netID);
			if (it == hostToLocal.end()) {
				Log(WARNING, "Net", "Host actor {} '{}' in {} at ({}, {}) has no local match.", entry.netID, entry.scriptName, entry.area, entry.x, entry.y);
				continue;
			}
			const NetActorTableEntry* mine = localByID[it->second];
			Log(MESSAGE, "Net", "Mapped net id {} '{}' party slot {} -> local id {} at ({}, {}), host had ({}, {})", entry.netID, entry.scriptName, entry.partySlot, it->second, mine->x, mine->y, entry.x, entry.y);
		}
		Log(MESSAGE, "Net", "Mapped {} of {} host actors ({} local).", matches.size(), table.actors.size(), local.size());

		NetReady ready;
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
		if (syncedTicks == TEST_HOOK_DELAY) {
			RunTestMove();
		}
		if (testResync && syncedTicks == unsigned(testResync)) {
			testResync = 0;
			Log(MESSAGE, "Net", "Test hook: asking the host for a resync.");
			RequestResync();
		}
	}

	bool RequestResync()
	{
		if (mode != Mode::Client || !server || state != ClientState::InSync) return false;
		Send(server, NetResyncRequest());
		// stay in sync until the new save arrives, so the game keeps mirroring the host meanwhile
		return true;
	}

	void ApplyState(const NetActorState& snapshot)
	{
		// the channel is sequenced already, but resets (reconnects) can replay old numbers
		if (snapshot.tick <= lastSnapshot && lastSnapshot - snapshot.tick < 1000) return;
		lastSnapshot = snapshot.tick;
		if (state != ClientState::InSync) return;

		Game* game = core->GetGame();
		if (!game) return;
		for (const auto& entry : snapshot.actors) {
			auto it = hostToLocal.find(entry.netID);
			if (it == hostToLocal.end()) continue;
			Actor* actor = game->GetActorByGlobalID(it->second);
			if (!actor || !actor->GetCurrentArea()) continue;
			if (std::string(actor->GetCurrentArea()->GetScriptRef()) != snapshot.area) {
				// TODO: M1.4 area changes are a reliable event
				continue;
			}

			Point pos(entry.x, entry.y);
			if (actor->Pos != pos) {
				actor->MoveTo(pos);
			}
			actor->SetOrientation(orient_t(entry.orientation & (MAX_ORIENT - 1)), false);
			if (actor->GetStance() != entry.stance) {
				actor->SetStance(entry.stance);
			}
			LogMovement("client", entry);
			if (snapshot.tick % SYNC_CHECK_INTERVAL == 0) {
				Log(MESSAGE, "Net", "Sync check #{}: net id {} (local {}) at {} stance {}", snapshot.tick, entry.netID, it->second, actor->Pos, actor->GetStance());
			}
		}
	}

	bool ForwardCommand(const Actor& actor, const std::string& action, bool append)
	{
		if (state != ClientState::InSync || !server) return false;
		auto it = localToHost.find(actor.GetGlobalID());
		if (it == localToHost.end()) {
			Log(WARNING, "Net", "Not forwarding '{}': actor {} has no network ID.", action, actor.GetGlobalID());
			return false;
		}
		NetCommand cmd;
		cmd.actorID = it->second;
		cmd.flags = append ? NetCommand::APPEND : NetCommand::NONE;
		cmd.action = action;
		Send(server, cmd);
		Log(MESSAGE, "Net", "Forwarded '{}' for net id {}.", action, cmd.actorID);
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
		if (game->selected.empty()) {
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
	}
}

void NetSession::PostTick()
{
	if (mode == Mode::Host) {
		impl->HostPostTick();
	}
	enet_host_flush(impl->host);
}

void NetSession::SendChat(const std::string& text)
{
	impl->SendChat(text);
}

bool NetSession::ForwardCommand(const Actor& actor, const std::string& action, bool append)
{
	return impl->ForwardCommand(actor, action, append);
}

bool NetSession::RequestResync()
{
	return impl->RequestResync();
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
void NetSession::SendChat(const std::string&) {}

bool NetSession::ForwardCommand(const Actor&, const std::string&, bool)
{
	return false;
}

bool NetSession::RequestResync()
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
