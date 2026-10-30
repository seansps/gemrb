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
			return { 0, 3, NetPermission::Control, "UseItem" };
		case NetGuiOp::MemorizeSpell:
			return { 0, 4, NetPermission::Control, "MemorizeSpell" };
		case NetGuiOp::UnmemorizeSpell:
			return { 0, 4, NetPermission::Control, "UnmemorizeSpell" };
		case NetGuiOp::ChangeItemFlag:
			return { 0, 4, NetPermission::Control, "ChangeItemFlag" };
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

uint64_t NetMessageHash(const NetSpellbook& msg)
{
	return EncodedHash(msg);
}

uint64_t NetMessageHash(const NetStoreState& msg)
{
	return EncodedHash(msg);
}

}
