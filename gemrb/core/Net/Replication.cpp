// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/Replication.h"

#include "Net/Resync.h"

#include <algorithm>
#include <tuple>

namespace GemRB {

void NetIDMap::Add(uint32_t netID, uint32_t localID)
{
	RemoveNet(netID);
	RemoveLocal(localID);
	toLocal[netID] = localID;
	toNet[localID] = netID;
}

void NetIDMap::RemoveNet(uint32_t netID)
{
	auto it = toLocal.find(netID);
	if (it == toLocal.end()) return;
	toNet.erase(it->second);
	toLocal.erase(it);
}

void NetIDMap::RemoveLocal(uint32_t localID)
{
	auto it = toNet.find(localID);
	if (it == toNet.end()) return;
	toLocal.erase(it->second);
	toNet.erase(it);
}

uint32_t NetIDMap::Local(uint32_t netID) const
{
	auto it = toLocal.find(netID);
	return it == toLocal.end() ? 0 : it->second;
}

uint32_t NetIDMap::Net(uint32_t localID) const
{
	auto it = toNet.find(localID);
	return it == toNet.end() ? 0 : it->second;
}

void NetIDMap::Clear()
{
	toLocal.clear();
	toNet.clear();
}

// which fields differ between two full entries
static uint8_t ChangedFields(const NetActorStateEntry& a, const NetActorStateEntry& b)
{
	uint8_t fields = 0;
	if (a.x != b.x || a.y != b.y) fields |= NAF_POS;
	if (a.orientation != b.orientation) fields |= NAF_ORIENT;
	if (a.stance != b.stance) fields |= NAF_STANCE;
	if (a.hp != b.hp || a.maxHP != b.maxHP) fields |= NAF_HP;
	if (a.state != b.state) fields |= NAF_STATE;
	return fields;
}

std::vector<NetActorStateEntry> NetDeltaTracker::Build(uint32_t seq, bool keyframe, const std::vector<NetActorStateEntry>& current)
{
	std::vector<NetActorStateEntry> out;
	std::map<uint32_t, Track> next;
	for (const auto& entry : current) {
		Track track;
		auto it = tracks.find(entry.netID);
		uint8_t changed = NAF_ALL;
		if (it != tracks.end()) {
			track = it->second;
			changed = ChangedFields(track.last, entry);
		}
		track.last = entry;
		track.last.fields = NAF_ALL;

		uint8_t send = keyframe ? NAF_ALL : 0;
		for (size_t bit = 0; bit < track.changedAt.size(); ++bit) {
			if (changed & (1 << bit)) {
				track.changedAt[bit] = seq;
			}
			// unsigned, so a stale entry from before a counter reset simply counts as old
			if (it == tracks.end() || seq - track.changedAt[bit] < redundancy) {
				send |= 1 << bit;
			}
		}
		next[entry.netID] = track;
		if (!send) continue;

		NetActorStateEntry delta = entry;
		delta.fields = send;
		out.push_back(delta);
	}
	tracks = std::move(next);
	return out;
}

uint64_t NetStateChecksum(const std::string& area, std::vector<NetCheckEntry> entries)
{
	std::sort(entries.begin(), entries.end(), [](const NetCheckEntry& x, const NetCheckEntry& y) {
		return std::make_tuple(x.kind, x.netID, x.a, x.b, x.c) < std::make_tuple(y.kind, y.netID, y.a, y.b, y.c);
	});

	std::vector<uint8_t> bytes;
	bytes.reserve(area.size() + entries.size() * 17 + 1);
	// area names are case insensitive resource names
	for (char ch : area) {
		bytes.push_back(uint8_t(tolower(static_cast<unsigned char>(ch))));
	}
	bytes.push_back(0);
	auto put32 = [&bytes](uint32_t value) {
		for (int i = 0; i < 4; ++i) {
			bytes.push_back(uint8_t(value >> (8 * i)));
		}
	};
	for (const auto& entry : entries) {
		bytes.push_back(uint8_t(entry.kind));
		put32(entry.netID);
		put32(static_cast<uint32_t>(entry.a));
		put32(entry.b);
		put32(entry.c);
	}
	return NetHash(bytes.data(), bytes.size());
}

bool NetInterpolate(const NetLerp& lerp, uint64_t now, int32_t& x, int32_t& y)
{
	if (lerp.duration == 0 || now >= lerp.start + lerp.duration) {
		x = lerp.toX;
		y = lerp.toY;
		return true;
	}
	int64_t elapsed = now > lerp.start ? int64_t(now - lerp.start) : 0;
	x = lerp.fromX + int32_t((int64_t(lerp.toX) - lerp.fromX) * elapsed / lerp.duration);
	y = lerp.fromY + int32_t((int64_t(lerp.toY) - lerp.fromY) * elapsed / lerp.duration);
	return false;
}

uint8_t NetDefaultOwner(uint8_t partySlot, const std::vector<uint8_t>& players)
{
	if (partySlot == 0 || players.empty()) return NET_HOST_SLOT;
	return players[(partySlot - 1) % players.size()];
}

const char* NetPermissionName(NetPermission perm)
{
	switch (perm) {
		case NetPermission::Control:
			return "control";
		case NetPermission::Purchase:
			return "purchase";
		case NetPermission::AreaTransition:
			return "area transition";
		case NetPermission::Dialog:
			return "dialog";
		case NetPermission::CharacterRecords:
			return "character records";
		case NetPermission::Pause:
			return "pause";
		case NetPermission::Leader:
			return "leader";
		default:
			return "?";
	}
}

void NetPermissions::Set(uint8_t slot, NetPermission perm, bool allowed)
{
	uint32_t bit = 1u << uint8_t(perm);
	if (allowed) {
		denied[slot] &= ~bit;
	} else {
		denied[slot] |= bit;
	}
}

bool NetPermissions::Allowed(uint8_t slot, NetPermission perm, uint8_t actorOwner) const
{
	if (slot == NET_HOST_SLOT) return true;
	if (perm == NetPermission::Control) return actorOwner == slot;
	auto it = denied.find(slot);
	return it == denied.end() || !(it->second & (1u << uint8_t(perm)));
}

uint32_t NetPermissions::Mask(uint8_t slot) const
{
	uint32_t mask = 0;
	for (uint8_t raw = uint8_t(NetPermission::Purchase); raw < uint8_t(NetPermission::count); ++raw) {
		if (Allowed(slot, NetPermission(raw))) mask |= 1u << raw;
	}
	return mask;
}

void NetPermissions::SetMask(uint8_t slot, uint32_t allowed)
{
	for (uint8_t raw = uint8_t(NetPermission::Purchase); raw < uint8_t(NetPermission::count); ++raw) {
		Set(slot, NetPermission(raw), allowed & (1u << raw));
	}
}

NetPermission NetPermissionByName(const std::string& name)
{
	std::string spaced = name;
	std::replace(spaced.begin(), spaced.end(), '_', ' ');
	for (uint8_t raw = 0; raw < uint8_t(NetPermission::count); ++raw) {
		if (spaced == NetPermissionName(NetPermission(raw))) return NetPermission(raw);
	}
	return NetPermission::count;
}

NetRoster::NetRoster(uint8_t maxSlot)
	: maxSlot(maxSlot)
{
	NetPlayer host;
	host.slot = NET_HOST_SLOT;
	host.connected = true;
	players.push_back(host);
}

void NetRoster::SetHost(const std::string& name)
{
	players[0].name = name;
}

uint8_t NetRoster::Join(const std::string& name, uint64_t token, uint64_t newToken, bool& rejoined)
{
	rejoined = false;
	for (auto& player : players) {
		if (!token || player.slot == NET_HOST_SLOT || player.token != token || player.connected) continue;
		player.connected = true;
		player.ready = false;
		player.name = name;
		rejoined = true;
		return player.slot;
	}
	// a free slot, else the one of whoever left first (the lowest)
	uint8_t slot = 0;
	for (uint8_t candidate = 1; candidate <= maxSlot && !slot; ++candidate) {
		if (!Find(candidate)) slot = candidate;
	}
	for (auto it = players.begin(); !slot && it != players.end(); ++it) {
		if (it->slot == NET_HOST_SLOT || it->connected) continue;
		slot = it->slot;
		players.erase(it);
		break;
	}
	if (!slot) return 0;
	NetPlayer player;
	player.slot = slot;
	player.name = name;
	player.token = newToken;
	player.connected = true;
	auto pos = std::find_if(players.begin(), players.end(), [slot](const NetPlayer& other) { return other.slot > slot; });
	players.insert(pos, player);
	return slot;
}

void NetRoster::Leave(uint8_t slot)
{
	for (auto& player : players) {
		if (player.slot != slot || slot == NET_HOST_SLOT) continue;
		player.connected = false;
		player.ready = false;
	}
}

void NetRoster::SetReady(uint8_t slot, bool ready)
{
	for (auto& player : players) {
		if (player.slot == slot && player.connected) player.ready = ready;
	}
}

void NetRoster::ClearReady()
{
	for (auto& player : players) {
		player.ready = false;
	}
}

const NetPlayer* NetRoster::Find(uint8_t slot) const
{
	for (const auto& player : players) {
		if (player.slot == slot) return &player;
	}
	return nullptr;
}

bool NetRoster::Connected(uint8_t slot) const
{
	const NetPlayer* player = Find(slot);
	return player && player->connected;
}

std::vector<uint8_t> NetRoster::Slots() const
{
	std::vector<uint8_t> slots;
	for (const auto& player : players) {
		slots.push_back(player.slot);
	}
	return slots;
}

bool NetRoster::AllReady() const
{
	for (const auto& player : players) {
		if (player.connected && !player.ready) return false;
	}
	return true;
}

uint8_t NetSlotOwner(uint8_t partySlot, const std::map<uint8_t, uint8_t>& assigned, const NetRoster& roster)
{
	auto it = assigned.find(partySlot);
	if (it != assigned.end() && roster.Find(it->second)) return it->second;
	return NetDefaultOwner(partySlot, roster.Slots());
}

uint8_t NetSlotController(uint8_t partySlot, const std::map<uint8_t, uint8_t>& assigned, const NetRoster& roster)
{
	uint8_t owner = NetSlotOwner(partySlot, assigned, roster);
	return roster.Connected(owner) ? owner : NET_HOST_SLOT;
}

NetAreaFollow NetAreaChangeKind(const NetAreaChangeInfo& info)
{
	if (!info.lightEnabled || info.partyChanged || !info.oneArea || info.noSave || !info.peerLive) return NetAreaFollow::Save;
	return NetAreaFollow::Area;
}

std::vector<NetActorTableEntry> NetAreaTable(const std::vector<NetActorTableEntry>& all, const std::string& area)
{
	std::vector<NetActorTableEntry> table;
	for (const auto& entry : all) {
		if (entry.area == area || entry.partySlot) table.push_back(entry);
	}
	return table;
}

std::vector<NetVariable> NetVariablesDiff(const std::map<std::string, int32_t>& before, const std::map<std::string, int32_t>& now)
{
	std::vector<NetVariable> changed;
	for (const auto& var : now) {
		auto it = before.find(var.first);
		if (it != before.end() && it->second == var.second) continue;
		changed.push_back(NetVariable { var.first, var.second });
	}
	return changed;
}

uint8_t NetDialogOwner(int speakerOwner, int targetOwner, uint8_t fallback)
{
	if (speakerOwner >= 0) return uint8_t(speakerOwner);
	if (targetOwner >= 0) return uint8_t(targetOwner);
	return fallback;
}

std::vector<NetStatEntry> NetStatsDiff(const std::vector<NetStatEntry>& before, const std::vector<NetStatEntry>& now, const std::vector<uint16_t>& skip)
{
	std::vector<NetStatEntry> changed;
	for (size_t i = 0; i < now.size(); ++i) {
		const NetStatEntry& entry = now[i];
		if (std::find(skip.begin(), skip.end(), entry.stat) != skip.end()) continue;
		if (i < before.size() && before[i].base == entry.base && before[i].modified == entry.modified) continue;
		changed.push_back(entry);
	}
	return changed;
}

std::vector<NetStatEntry> NetStatsFull(const std::vector<NetStatEntry>& now, const std::vector<uint16_t>& skip)
{
	std::vector<NetStatEntry> listed;
	for (const auto& entry : now) {
		if (!entry.base && !entry.modified) continue;
		if (std::find(skip.begin(), skip.end(), entry.stat) != skip.end()) continue;
		listed.push_back(entry);
	}
	return listed;
}

std::vector<NetStatEntry> NetStatsExpand(const std::vector<NetStatEntry>& listed, size_t count, const std::vector<uint16_t>& skip)
{
	std::vector<NetStatEntry> all;
	std::vector<bool> have(count, false);
	for (const auto& entry : listed) {
		if (entry.stat < count) have[entry.stat] = true;
	}
	all.reserve(count);
	for (size_t stat = 0; stat < count; ++stat) {
		if (have[stat] || std::find(skip.begin(), skip.end(), uint16_t(stat)) != skip.end()) continue;
		all.push_back(NetStatEntry { uint16_t(stat), 0, 0 });
	}
	all.insert(all.end(), listed.begin(), listed.end());
	return all;
}

NetGuiOpInfo NetGuiInfo(NetGuiOp op)
{
	switch (op) {
		case NetGuiOp::DragItem:
			return { 0, 2, NetPermission::Control, "DragItem" };
		case NetGuiOp::DropDraggedItem:
			return { 0, 2, NetPermission::Control, "DropDraggedItem" };
		case NetGuiOp::ChangeContainerItem:
			return { 0, 3, NetPermission::Control, "ChangeContainerItem" };
		case NetGuiOp::LeaveContainer:
			return { -1, 0, NetPermission::Control, "LeaveContainer" };
		case NetGuiOp::ChangeStoreItem:
			return { 0, 3, NetPermission::Purchase, "ChangeStoreItem" };
		case NetGuiOp::SetPurchasedAmount:
			return { -1, 2, NetPermission::Purchase, "SetPurchasedAmount" };
		case NetGuiOp::LeaveStore:
			return { -1, 0, NetPermission::Control, "LeaveStore" };
		case NetGuiOp::AddPartyGold:
			return { -1, 1, NetPermission::Purchase, "GameSetPartyGold" };
		case NetGuiOp::SetEquippedQuickSlot:
			return { 0, 2, NetPermission::Control, "SetEquippedQuickSlot" };
		case NetGuiOp::UseItem:
			return { 0, 3, NetPermission::Control, "UseItem", true };
		case NetGuiOp::MemorizeSpell:
			return { 0, 4, NetPermission::CharacterRecords, "MemorizeSpell" };
		case NetGuiOp::UnmemorizeSpell:
			return { 0, 4, NetPermission::CharacterRecords, "UnmemorizeSpell" };
		case NetGuiOp::ChangeItemFlag:
			return { 0, 4, NetPermission::Control, "ChangeItemFlag" };
		// M1.6: party-wide decisions are the leader's, record changes need CharacterRecords
		case NetGuiOp::RestParty:
			return { -1, 3, NetPermission::Leader, "RestParty", true };
		case NetGuiOp::LeaveParty:
			return { 0, 1, NetPermission::Leader, "LeaveParty", true };
		case NetGuiOp::SwapPCs:
			return { -1, 2, NetPermission::Leader, "GameSwapPCs", true };
		case NetGuiOp::SetFormation:
			return { -1, 2, NetPermission::Leader, "GameSetFormation" };
		case NetGuiOp::SetModalState:
			return { 0, 2, NetPermission::Control, "SetModalState", false, true };
		case NetGuiOp::SetupQuickSlot:
			return { 0, 3, NetPermission::CharacterRecords, "SetupQuickSlot" };
		case NetGuiOp::SetupQuickSpell:
			return { 0, 4, NetPermission::CharacterRecords, "SetupQuickSpell" };
		case NetGuiOp::SetupQuickItemSlot:
			return { 0, 5, NetPermission::CharacterRecords, "SetupQuickItemSlot" };
		case NetGuiOp::SetJournalEntry:
			return { -1, 4, NetPermission::Control, "SetJournalEntry", false, true };
		case NetGuiOp::SetMapnote:
			return { -1, 3, NetPermission::Control, "SetMapnote", false, true };
		// M2.4: a character's Luck burn and deed choices, its owner's only
		case NetGuiOp::DCCSetLuckPolicy:
			return { 0, 4, NetPermission::Control, "DCCSetLuckPolicy" };
		case NetGuiOp::DCCSetDeeds:
			return { 0, 3, NetPermission::Control, "DCCSetDeeds" };
		// M2.3b: the combat options; rolling a body over is the host's roll, for the ally's owner
		case NetGuiOp::DCCSetCombat:
			return { 0, 3, NetPermission::Control, "DCCSetCombat" };
		case NetGuiOp::DCCRecoverBody:
			return { 1, 2, NetPermission::Control, "DCCRecoverBody", true };
		// M2.5: spellburn settings for the owner; atonement, lay on hands and turning are host rolls
		case NetGuiOp::DCCSetSpellburn:
			return { 0, 4, NetPermission::Control, "DCCSetSpellburn" };
		case NetGuiOp::DCCAtone:
			return { 0, 2, NetPermission::Control, "DCCAtone", true };
		case NetGuiOp::DCCLayOnHands:
			return { 0, 2, NetPermission::Control, "DCCLayOnHands", true };
		case NetGuiOp::DCCTurnUnholy:
			return { 0, 1, NetPermission::Control, "DCCTurnUnholy", true };
		// Phase 3 (v12): the level-up and its spells (the hit die and the picks are host rolls)
		case NetGuiOp::DCCLevelUp:
			return { 0, 2, NetPermission::Control, "DCCLevelUp", true };
		case NetGuiOp::DCCReplaceSpell:
			return { 0, 1, NetPermission::Control, "DCCReplaceSpell", true, true };
		case NetGuiOp::DCCConfirmSpells:
			return { 0, 1, NetPermission::Control, "DCCConfirmSpells", true, true };
		// M2.10 (v13): what a character does on its own (the DCC party AI), its owner's choice
		case NetGuiOp::DCCSetPartyAI:
			return { 0, 2, NetPermission::Control, "DCCSetPartyAI" };
		// M3.3 (v15): a level-0 character's alignment and god, its owner's choice
		case NetGuiOp::DCCSetAlignment:
			return { 0, 3, NetPermission::Control, "DCCSetAlignment" };
		default:
			return {};
	}
}

template<typename MSG>
static uint64_t EncodedHash(const MSG& msg)
{
	auto data = NetEncode(msg);
	return NetHash(data.data(), data.size());
}

uint64_t NetMessageHash(const NetInventory& msg)
{
	return EncodedHash(msg);
}

uint64_t NetMessageHash(const NetDCCExtension& msg)
{
	return EncodedHash(msg);
}

uint64_t NetMessageHash(const NetSpellbook& msg)
{
	return EncodedHash(msg);
}

uint64_t NetMessageHash(const NetStoreState& msg)
{
	return EncodedHash(msg);
}

// chat --------------------------------------------------------------------

// names: a palette that stays apart on the dark message window; the host gets gold
static constexpr const char* CHAT_COLORS[] = { "F0C850FF", "70B8FFFF", "7CE07CFF", "F090E0FF", "50E0D0FF", "FFA050FF" };
static constexpr const char* CHAT_SYSTEM_COLOR = "B4B4AAFF";
static constexpr const char* CHAT_WHISPER_COLOR = "C8A8FFFF";

bool NetChatVisibleTo(const NetChat& chat, uint8_t slot)
{
	if (!(chat.flags & NetChat::WHISPER)) return true;
	return chat.slot == slot || chat.target == slot;
}

const char* NetChatColor(uint8_t slot)
{
	return CHAT_COLORS[slot % (sizeof(CHAT_COLORS) / sizeof(CHAT_COLORS[0]))];
}

// player text may not open or close tags: the markup parser has no escapes
static std::string Defuse(const std::string& text)
{
	std::string out = text;
	for (char& c : out) {
		if (c == '[') {
			c = '(';
		} else if (c == ']') {
			c = ')';
		} else if (c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
	}
	return out;
}

static std::string Colored(const char* color, const std::string& text)
{
	return "[color=" + std::string(color) + "]" + text + "[/color]";
}

std::string NetSystemMarkup(const std::string& text)
{
	return "[p]" + Colored(CHAT_SYSTEM_COLOR, "* " + Defuse(text)) + "[/p]";
}

std::string NetChatMarkup(const NetChat& chat, uint8_t viewer)
{
	std::string name = Defuse(chat.name);
	std::string text = Defuse(chat.text);
	if (chat.flags & NetChat::SYSTEM) {
		return NetSystemMarkup(chat.text);
	}
	if (!(chat.flags & NetChat::WHISPER)) {
		return "[p]" + Colored(NetChatColor(chat.slot), name + ":") + " " + text + "[/p]";
	}
	if (chat.slot == viewer) {
		// our own whisper: who it went to
		return "[p]" + Colored(CHAT_WHISPER_COLOR, "(to ") + Colored(NetChatColor(chat.target), Defuse(chat.targetName)) + Colored(CHAT_WHISPER_COLOR, ") " + text) + "[/p]";
	}
	if (chat.target == viewer) {
		return "[p]" + Colored(NetChatColor(chat.slot), name) + Colored(CHAT_WHISPER_COLOR, " whispers: " + text) + "[/p]";
	}
	return "[p]" + Colored(NetChatColor(chat.slot), name) + Colored(CHAT_WHISPER_COLOR, " to " + Defuse(chat.targetName) + ": " + text) + "[/p]";
}

std::string NetChatPlain(const NetChat& chat, uint8_t viewer)
{
	if (chat.flags & NetChat::SYSTEM) {
		return "* " + chat.text;
	}
	if (!(chat.flags & NetChat::WHISPER)) {
		return chat.name + ": " + chat.text;
	}
	if (chat.slot == viewer) {
		return "(to " + chat.targetName + ") " + chat.text;
	}
	if (chat.target == viewer) {
		return chat.name + " whispers: " + chat.text;
	}
	return chat.name + " to " + chat.targetName + ": " + chat.text;
}

static std::string Lower(std::string text)
{
	for (char& c : text) {
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	}
	return text;
}

static std::string TrimLeft(const std::string& text)
{
	size_t start = text.find_first_not_of(' ');
	return start == std::string::npos ? std::string() : text.substr(start);
}

static std::string Trim(const std::string& text)
{
	std::string out = TrimLeft(text);
	size_t end = out.find_last_not_of(' ');
	return end == std::string::npos ? std::string() : out.substr(0, end + 1);
}

static NetChatCommand Notice(const std::string& text)
{
	NetChatCommand cmd;
	cmd.kind = NetChatCommand::Kind::Notice;
	cmd.text = text;
	return cmd;
}

static const NetLobbyPlayer* PlayerBySlot(const std::vector<NetLobbyPlayer>& players, int slot)
{
	for (const auto& player : players) {
		if (player.slot == slot) return &player;
	}
	return nullptr;
}

static NetChatCommand Whisper(const NetLobbyPlayer* player, const std::string& text, uint8_t self)
{
	if (player->slot == self) return Notice("You can't whisper to yourself.");
	if (!(player->flags & NetLobbyPlayer::CONNECTED)) return Notice(player->name + " is away.");
	if (text.empty()) return Notice("Whisper what? /w " + player->name + " text");
	NetChatCommand cmd;
	cmd.kind = NetChatCommand::Kind::Whisper;
	cmd.target = player->slot;
	cmd.targetName = player->name;
	cmd.text = text;
	return cmd;
}

static constexpr const char* CHAT_HELP = "Chat: /w NAME text whispers to one player, /r text answers the last whisper.";

NetChatCommand NetParseChat(const std::string& line, const std::vector<NetLobbyPlayer>& players, uint8_t self, int lastWhisperer)
{
	std::string text = Trim(line);
	NetChatCommand cmd;
	if (text.empty()) return cmd;
	if (text[0] != '/' || text.compare(0, 2, "//") == 0) {
		cmd.kind = NetChatCommand::Kind::Say;
		cmd.text = text[0] == '/' ? text.substr(1) : text;
		return cmd;
	}

	size_t space = text.find(' ');
	std::string verb = Lower(text.substr(1, space == std::string::npos ? std::string::npos : space - 1));
	std::string rest = space == std::string::npos ? std::string() : TrimLeft(text.substr(space + 1));
	if (verb == "w" || verb == "whisper" || verb == "tell" || verb == "t") {
		if (rest.empty()) return Notice(CHAT_HELP);
		// the longest name the rest starts with, followed by a space or nothing
		const NetLobbyPlayer* best = nullptr;
		std::string lowRest = Lower(rest);
		for (const auto& player : players) {
			std::string name = Lower(player.name);
			if (name.empty() || lowRest.compare(0, name.size(), name) != 0) continue;
			if (lowRest.size() > name.size() && lowRest[name.size()] != ' ') continue;
			if (!best || name.size() > best->name.size()) best = &player;
		}
		if (best) return Whisper(best, TrimLeft(rest.substr(best->name.size())), self);
		// or a player number
		size_t end = rest.find(' ');
		std::string word = rest.substr(0, end);
		if (!word.empty() && word.size() <= 2 && word.find_first_not_of("0123456789") == std::string::npos) {
			const NetLobbyPlayer* player = PlayerBySlot(players, std::stoi(word));
			if (player) return Whisper(player, end == std::string::npos ? std::string() : TrimLeft(rest.substr(end)), self);
		}
		return Notice("Nobody here is called '" + word + "'.");
	}
	if (verb == "r" || verb == "reply") {
		const NetLobbyPlayer* player = lastWhisperer < 0 ? nullptr : PlayerBySlot(players, lastWhisperer);
		if (!player) return Notice("Nobody has whispered to you yet.");
		return Whisper(player, rest, self);
	}
	if (verb == "help" || verb == "?") return Notice(CHAT_HELP);
	return Notice("Unknown command '/" + verb + "'. " + CHAT_HELP);
}

}
