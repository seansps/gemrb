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

#include "GUI/GameControl.h"
#include "GameScript/GameScript.h"
#include "Logging/Logging.h"
#include "Net/Protocol.h"
#include "Scriptable/Actor.h"
#include "Strings/StringConversion.h"

#ifdef HAVE_ENET
	#include <enet/enet.h>
#endif

#include <map>
#include <vector>

namespace GemRB {

#ifdef HAVE_ENET

// how often (in game loop ticks) the host sends actor snapshots; ticks run at ~15 Hz
static constexpr unsigned int SNAPSHOT_INTERVAL = 1;
// client: how long to wait between connection attempts
static constexpr tick_t RECONNECT_DELAY = 2000;
// test hooks wait this many ticks after syncing, so both sides are settled
static constexpr unsigned int TEST_HOOK_DELAY = 30;

struct NetPeerInfo {
	ENetPeer* peer = nullptr;
	uint8_t slot = 0;
	std::string name;
	bool welcomed = false;
	bool manifestSent = false;
};

struct NetSession::Impl {
	Mode mode;
	std::string name;
	std::string remoteHost;
	uint16_t port;
	ENetHost* host = nullptr;

	// host side
	std::vector<NetPeerInfo> peers;
	uint32_t snapshotSeq = 0;

	// client side
	ENetPeer* server = nullptr;
	tick_t nextConnectAttempt = 0;
	bool welcomed = false;
	bool haveManifest = false;
	bool replica = false;
	uint8_t slot = NET_HOST_SLOT;
	uint32_t lastSnapshot = 0;
	NetPartyManifest manifest;
	// host global ID <-> local global ID, matched by party slot
	std::map<uint32_t, ieDword> hostToLocal;
	std::map<ieDword, uint32_t> localToHost;

	// spike test hooks
	std::string testChat;
	std::string testMove;
	unsigned int syncedTicks = 0;

	unsigned int ticks = 0;
	std::map<uint32_t, Point> lastLoggedPos;

	Impl(Mode mode, const CoreSettings& config)
		: mode(mode), name(config.NetName), remoteHost(config.NetHost), port(uint16_t(config.NetPort)),
		  testChat(config.NetTestChat), testMove(config.NetTestMove)
	{}

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
		enet_uint32 flags = channel == NetChannel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
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
		} else if (welcomed) {
			Send(server, chat);
		} else {
			Log(WARNING, "Net", "Not connected, chat dropped: {}", text);
		}
	}

	// the event loop -------------------------------------------------------

	void Poll()
	{
		if (mode == Mode::Client && !server && GetMilliseconds() >= nextConnectAttempt) {
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
		peers.push_back(info);
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
			if (welcomed) {
				Log(MESSAGE, "Net", "Disconnected from the host, continuing locally.");
			} else {
				Log(MESSAGE, "Net", "Could not reach the host, retrying in {}s.", RECONNECT_DELAY / 1000);
			}
			server = nullptr;
			welcomed = false;
			haveManifest = false;
			if (replica) {
				Log(MESSAGE, "Net", "Leaving replica mode, local scripts resume.");
			}
			replica = false;
			nextConnectAttempt = GetMilliseconds() + RECONNECT_DELAY;
			return;
		}

		for (auto it = peers.begin(); it != peers.end(); ++it) {
			if (it->peer == peer) {
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
					if (!NetDecode(data, size, hello)) return false;
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
			default:
				return false;
		}
	}

	static bool IsAllowedAction(const std::string& action)
	{
		// M2.3 forwards movement only
		static const char* const allowed[] = { "MoveToPoint(", "RunToPoint(", "AddWayPoint(" };
		for (const char* prefix : allowed) {
			if (action.compare(0, strlen(prefix), prefix) == 0) return true;
		}
		return false;
	}

	void HandleCommand(const NetPeerInfo& info, const NetCommand& cmd)
	{
		Game* game = core->GetGame();
		Actor* actor = game ? game->GetActorByGlobalID(cmd.actorID) : nullptr;
		// TODO: M2.4+ check the per-character owner slot instead of plain party membership
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

	static bool BuildManifest(NetPartyManifest& out)
	{
		const Game* game = core->GetGame();
		if (!game || game->GetPartySize(false) == 0) return false;
		const Actor* first = game->FindPC(1);
		if (!first || !first->GetCurrentArea()) return false;

		out.area = std::string(first->GetCurrentArea()->GetScriptRef());
		out.actors.clear();
		int count = game->GetPartySize(false);
		for (int idx = 1; idx <= count; idx++) {
			const Actor* pc = game->FindPC(idx);
			if (!pc) continue;
			NetManifestEntry entry;
			entry.globalID = pc->GetGlobalID();
			entry.partySlot = uint8_t(idx);
			entry.scriptName = std::string(pc->GetScriptName());
			out.actors.push_back(entry);
		}
		return true;
	}

	void HostPostTick()
	{
		bool anyWelcomed = false;
		for (auto& info : peers) {
			if (!info.welcomed) continue;
			anyWelcomed = true;
			if (info.manifestSent) continue;
			NetPartyManifest partyManifest;
			if (BuildManifest(partyManifest)) {
				Send(info.peer, partyManifest);
				info.manifestSent = true;
				Log(MESSAGE, "Net", "Sent party manifest ({} actors in {}) to slot {}.", partyManifest.actors.size(), partyManifest.area, info.slot);
			}
		}
		if (!anyWelcomed) return;

		syncedTicks++;
		RunTestMove();

		if (ticks % SNAPSHOT_INTERVAL) return;
		const Game* game = core->GetGame();
		if (!game) return;
		const Actor* first = game->FindPC(1);
		if (!first || !first->GetCurrentArea()) return;
		const Map* area = first->GetCurrentArea();

		NetActorState state;
		state.tick = ++snapshotSeq;
		state.area = std::string(area->GetScriptRef());
		int count = game->GetPartySize(false);
		for (int idx = 1; idx <= count; idx++) {
			const Actor* pc = game->FindPC(idx);
			if (!pc || pc->GetCurrentArea() != area) continue;
			NetActorStateEntry entry;
			entry.globalID = pc->GetGlobalID();
			entry.x = pc->Pos.x;
			entry.y = pc->Pos.y;
			entry.orientation = uint8_t(pc->GetOrientation());
			entry.stance = pc->GetStance();
			state.actors.push_back(entry);
			LogMovement("host", entry);
		}
		Broadcast(state, NetChannel::State);
	}

	// log a party actor's position about once a second while it moves
	void LogMovement(const char* side, const NetActorStateEntry& entry)
	{
		Point pos(entry.x, entry.y);
		auto it = lastLoggedPos.find(entry.globalID);
		bool moved = it == lastLoggedPos.end() || it->second != pos;
		if (!moved || (it != lastLoggedPos.end() && ticks % 15)) return;
		lastLoggedPos[entry.globalID] = pos;
		Log(MESSAGE, "Net", "{} state: actor {} at {} orient {} stance {}", side, entry.globalID, pos, entry.orientation, entry.stance);
	}

	// client ---------------------------------------------------------------

	bool ClientReceive(NetMsgType type, const uint8_t* data, size_t size)
	{
		switch (type) {
			case NetMsgType::Welcome:
				{
					NetWelcome welcome;
					if (!NetDecode(data, size, welcome)) return false;
					welcomed = true;
					slot = welcome.slot;
					Log(MESSAGE, "Net", "Welcomed by host '{}', we are slot {}.", welcome.hostName, slot);
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
					Log(ERROR, "Net", "The host rejected us: {}", reject.reason);
					return true;
				}
			case NetMsgType::Chat:
				{
					NetChat chat;
					if (!NetDecode(data, size, chat)) return false;
					ShowChat(chat);
					return true;
				}
			case NetMsgType::PartyManifest:
				if (!NetDecode(data, size, manifest)) return false;
				haveManifest = true;
				Log(MESSAGE, "Net", "Got party manifest: {} actors in {}.", manifest.actors.size(), manifest.area);
				return true;
			case NetMsgType::ActorState:
				{
					NetActorState state;
					if (!NetDecode(data, size, state)) return false;
					ApplyState(state);
					return true;
				}
			default:
				return false;
		}
	}

	// match the host's party to ours by party slot, as the global IDs only agree by luck
	bool MapParty()
	{
		const Game* game = core->GetGame();
		if (!game || game->GetPartySize(false) == 0) return false;
		hostToLocal.clear();
		localToHost.clear();
		for (const auto& entry : manifest.actors) {
			const Actor* pc = game->FindPC(entry.partySlot);
			if (!pc) {
				Log(WARNING, "Net", "Host party slot {} (id {}) has no local actor.", entry.partySlot, entry.globalID);
				continue;
			}
			ieDword localID = pc->GetGlobalID();
			hostToLocal[entry.globalID] = localID;
			localToHost[localID] = entry.globalID;
			Log(MESSAGE, "Net", "Party slot {}: host id {} '{}' -> local id {} '{}'{}", entry.partySlot, entry.globalID, entry.scriptName,
			    localID, pc->GetScriptName(), localID == entry.globalID ? " (match)" : " (REMAPPED)");
		}
		return !hostToLocal.empty();
	}

	void ClientPreTick()
	{
		if (!replica && welcomed && haveManifest && MapParty()) {
			replica = true;
			syncedTicks = 0;
			Log(MESSAGE, "Net", "In sync with the host: replica mode on, local scripts and AI stop.");
		}
		if (!replica) return;
		syncedTicks++;
		RunTestMove();
	}

	void ApplyState(const NetActorState& state)
	{
		// the channel is sequenced already, but resets (reconnects) can replay old numbers
		if (state.tick <= lastSnapshot && lastSnapshot - state.tick < 1000) return;
		lastSnapshot = state.tick;
		if (!replica) return;

		Game* game = core->GetGame();
		if (!game) return;
		for (const auto& entry : state.actors) {
			auto it = hostToLocal.find(entry.globalID);
			if (it == hostToLocal.end()) continue;
			Actor* actor = game->GetActorByGlobalID(it->second);
			if (!actor || !actor->GetCurrentArea()) continue;
			if (std::string(actor->GetCurrentArea()->GetScriptRef()) != state.area) {
				// TODO: M2.4 area changes are a reliable event
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
		}
	}

	bool ForwardCommand(const Actor& actor, const std::string& action, bool append)
	{
		if (!replica || !server) return false;
		auto it = localToHost.find(actor.GetGlobalID());
		if (it == localToHost.end()) {
			Log(WARNING, "Net", "Not forwarding '{}': actor {} is not in the host's party.", action, actor.GetGlobalID());
			return false;
		}
		NetCommand cmd;
		cmd.actorID = it->second;
		cmd.flags = append ? NetCommand::APPEND : NetCommand::NONE;
		cmd.action = action;
		Send(server, cmd);
		Log(MESSAGE, "Net", "Forwarded '{}' for host id {}.", action, cmd.actorID);
		return true;
	}

	// NetTestMove=x.y: issue one party move through GameControl, like a click would
	void RunTestMove()
	{
		if (testMove.empty() || syncedTicks < TEST_HOOK_DELAY) return;
		std::string spec = testMove;
		testMove.clear();

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
	return impl->replica;
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

void NetSession::PreTick() {}
void NetSession::PostTick() {}
void NetSession::SendChat(const std::string&) {}

bool NetSession::ForwardCommand(const Actor&, const std::string&, bool)
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
