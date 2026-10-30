// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/Net/Replication.h"
#include "../../../core/Net/Resync.h"

#include <gtest/gtest.h>

#include <map>

namespace GemRB {

TEST(NetReplicationTest, IDMap)
{
	NetIDMap ids;
	EXPECT_EQ(ids.Local(10002), 0U);
	EXPECT_EQ(ids.Net(10017), 0U);

	ids.Add(10002, 10017);
	ids.Add(10015, 10030);
	EXPECT_EQ(ids.Size(), 2U);
	EXPECT_EQ(ids.Local(10002), 10017U);
	EXPECT_EQ(ids.Net(10017), 10002U);
	EXPECT_EQ(ids.Net(10030), 10015U);

	// a spawn that reuses a local ID (the old actor was deleted) replaces both directions
	ids.Add(10099, 10030);
	EXPECT_EQ(ids.Local(10015), 0U);
	EXPECT_EQ(ids.Net(10030), 10099U);
	EXPECT_EQ(ids.Size(), 2U);
	// and so does a network ID that gets a new local actor
	ids.Add(10002, 10050);
	EXPECT_EQ(ids.Net(10017), 0U);
	EXPECT_EQ(ids.Local(10002), 10050U);

	ids.RemoveNet(10099);
	EXPECT_EQ(ids.Net(10030), 0U);
	ids.RemoveLocal(10050);
	EXPECT_EQ(ids.Local(10002), 0U);
	EXPECT_EQ(ids.Size(), 0U);
	// removing unknown IDs is harmless
	ids.RemoveNet(1);
	ids.RemoveLocal(1);

	ids.Add(1, 2);
	ids.Clear();
	EXPECT_EQ(ids.Size(), 0U);
	EXPECT_EQ(ids.Net(2), 0U);
}

static NetActorStateEntry Actor(uint32_t id, int x, int hp)
{
	NetActorStateEntry entry;
	entry.netID = id;
	entry.x = x;
	entry.y = 100;
	entry.orientation = 2;
	entry.stance = 7;
	entry.hp = hp;
	entry.maxHP = 10;
	entry.state = 0;
	return entry;
}

TEST(NetReplicationTest, DeltaSnapshots)
{
	NetDeltaTracker tracker(2);
	std::vector<NetActorStateEntry> area = { Actor(1, 10, 10), Actor(2, 20, 10) };

	// new actors go out whole
	auto out = tracker.Build(1, false, area);
	ASSERT_EQ(out.size(), 2U);
	EXPECT_EQ(out[0].fields, NAF_ALL);
	// they keep going out for the redundancy window, then nothing is sent
	EXPECT_EQ(tracker.Build(2, false, area).size(), 2U);
	EXPECT_TRUE(tracker.Build(3, false, area).empty());

	// a hit: only the hit points of that actor, for two snapshots
	area[1].hp = 4;
	out = tracker.Build(4, false, area);
	ASSERT_EQ(out.size(), 1U);
	EXPECT_EQ(out[0].netID, 2U);
	EXPECT_EQ(out[0].fields, NAF_HP);
	EXPECT_EQ(out[0].hp, 4);
	out = tracker.Build(5, false, area);
	ASSERT_EQ(out.size(), 1U);
	EXPECT_EQ(out[0].fields, NAF_HP);
	EXPECT_TRUE(tracker.Build(6, false, area).empty());

	// walking changes position and stance together
	area[0].x = 14;
	area[0].stance = 10;
	out = tracker.Build(7, false, area);
	ASSERT_EQ(out.size(), 1U);
	EXPECT_EQ(out[0].fields, NAF_POS | NAF_STANCE);
	// a keyframe has everyone and every field
	out = tracker.Build(8, true, area);
	ASSERT_EQ(out.size(), 2U);
	EXPECT_EQ(out[0].fields, NAF_ALL);
	EXPECT_EQ(out[1].fields, NAF_ALL);

	// leaving the area forgets the actor; coming back sends it whole again
	area.pop_back();
	tracker.Build(9, false, area);
	EXPECT_EQ(tracker.Size(), 1U);
	area.push_back(Actor(2, 20, 4));
	out = tracker.Build(10, false, area);
	ASSERT_EQ(out.size(), 1U);
	EXPECT_EQ(out[0].netID, 2U);
	EXPECT_EQ(out[0].fields, NAF_ALL);

	tracker.Reset();
	EXPECT_EQ(tracker.Build(11, false, area).size(), 2U);
}

TEST(NetReplicationTest, Checksum)
{
	std::vector<NetCheckEntry> host = {
		{ 10002, NetObjectKind::Actor, 12, 12, 0 },
		{ 10015, NetObjectKind::Actor, 0, 6, 0x800 }, // a dead rabbit
		{ 10077, NetObjectKind::Door, 2, 0, 0 },
	};
	uint64_t sum = NetStateChecksum("MPT01", host);

	// order and the case of the area don't matter
	std::vector<NetCheckEntry> client = { host[2], host[0], host[1] };
	EXPECT_EQ(NetStateChecksum("mpt01", client), sum);

	// any state difference does
	client[1].a = 11;
	EXPECT_NE(NetStateChecksum("MPT01", client), sum);
	client[1].a = 12;
	client[0].a = 3; // the door opened
	EXPECT_NE(NetStateChecksum("MPT01", client), sum);
	client[0].a = 2;
	client[2].c = 0;
	EXPECT_NE(NetStateChecksum("MPT01", client), sum);
	client[2].c = 0x800;
	EXPECT_EQ(NetStateChecksum("MPT01", client), sum);
	// so does a missing, extra or unmapped actor, or another area
	client.pop_back();
	EXPECT_NE(NetStateChecksum("MPT01", client), sum);
	client.push_back({ 0, NetObjectKind::Actor, 0, 6, 0x800 });
	EXPECT_NE(NetStateChecksum("MPT01", client), sum);
	EXPECT_NE(NetStateChecksum("MPT02", host), sum);
	// the same ID as another kind is something else
	std::vector<NetCheckEntry> kinds = host;
	kinds[2].kind = NetObjectKind::Container;
	EXPECT_NE(NetStateChecksum("MPT01", kinds), sum);
}

TEST(NetReplicationTest, Interpolation)
{
	NetLerp lerp;
	lerp.fromX = 100;
	lerp.fromY = 200;
	lerp.toX = 160;
	lerp.toY = 170;
	lerp.start = 1000;
	lerp.duration = 60;

	int32_t x = 0;
	int32_t y = 0;
	EXPECT_FALSE(NetInterpolate(lerp, 1000, x, y));
	EXPECT_EQ(x, 100);
	EXPECT_EQ(y, 200);
	EXPECT_FALSE(NetInterpolate(lerp, 1030, x, y));
	EXPECT_EQ(x, 130);
	EXPECT_EQ(y, 185);
	EXPECT_TRUE(NetInterpolate(lerp, 1060, x, y));
	EXPECT_EQ(x, 160);
	EXPECT_EQ(y, 170);
	EXPECT_TRUE(NetInterpolate(lerp, 5000, x, y));
	EXPECT_EQ(x, 160);
	// a frame from before the snapshot stays at the start
	EXPECT_FALSE(NetInterpolate(lerp, 900, x, y));
	EXPECT_EQ(x, 100);
	// no duration snaps
	lerp.duration = 0;
	EXPECT_TRUE(NetInterpolate(lerp, 1000, x, y));
	EXPECT_EQ(y, 170);
}

TEST(NetReplicationTest, DefaultOwner)
{
	std::vector<uint8_t> alone = { NET_HOST_SLOT };
	for (uint8_t slot = 1; slot <= 6; ++slot) {
		EXPECT_EQ(NetDefaultOwner(slot, alone), NET_HOST_SLOT);
	}
	std::vector<uint8_t> two = { 0, 1 };
	EXPECT_EQ(NetDefaultOwner(1, two), 0);
	EXPECT_EQ(NetDefaultOwner(2, two), 1);
	EXPECT_EQ(NetDefaultOwner(3, two), 0);
	EXPECT_EQ(NetDefaultOwner(4, two), 1);
	std::vector<uint8_t> gap = { 0, 2, 3 }; // slot 1 left
	EXPECT_EQ(NetDefaultOwner(2, gap), 2);
	EXPECT_EQ(NetDefaultOwner(3, gap), 3);
	EXPECT_EQ(NetDefaultOwner(4, gap), 0);
	// outside the party (summons) and no players: the host
	EXPECT_EQ(NetDefaultOwner(0, two), NET_HOST_SLOT);
	EXPECT_EQ(NetDefaultOwner(2, {}), NET_HOST_SLOT);
}

TEST(NetReplicationTest, MatchObjectsByKind)
{
	// a door and a container may share a name; they never pair across kinds
	std::vector<NetActorTableEntry> host = {
		{ 20001, 1, "MPT01", "protagon", 400, 300 },
		{ 20100, 0, "MPT01", "MptGate", 520, 300, NetObjectKind::Door },
		{ 20101, 0, "MPT01", "MptGate", 520, 300, NetObjectKind::Region },
		{ 20102, 0, "MPT01", "MptChest", 125, 1066, NetObjectKind::Container },
	};
	std::vector<NetActorTableEntry> local = {
		{ 7, 0, "MPT01", "MptChest", 125, 1066, NetObjectKind::Container },
		{ 8, 0, "MPT01", "MptGate", 520, 300, NetObjectKind::Region },
		{ 9, 0, "MPT01", "MptGate", 520, 300, NetObjectKind::Door },
		{ 1, 1, "MPT01", "protagon", 400, 300 },
		{ 2, 1, "MPT01", "protagon", 400, 300, NetObjectKind::Region }, // never a party member
	};
	auto matches = NetMatchActors(host, local, 64);
	std::map<uint32_t, uint32_t> map(matches.begin(), matches.end());
	EXPECT_EQ(map.size(), 4U);
	EXPECT_EQ(map[20001], 1U);
	EXPECT_EQ(map[20100], 9U);
	EXPECT_EQ(map[20101], 8U);
	EXPECT_EQ(map[20102], 7U);
}

TEST(NetReplicationTest, Permissions)
{
	NetPermissions perms;
	// everything is allowed by default, except other players' party members
	EXPECT_TRUE(perms.Allowed(1, NetPermission::Purchase));
	EXPECT_TRUE(perms.Allowed(1, NetPermission::Dialog));
	EXPECT_TRUE(perms.Allowed(1, NetPermission::Control, 1));
	EXPECT_FALSE(perms.Allowed(1, NetPermission::Control, 0));
	EXPECT_FALSE(perms.Allowed(2, NetPermission::Control, 1));

	perms.Set(1, NetPermission::Purchase, false);
	EXPECT_FALSE(perms.Allowed(1, NetPermission::Purchase));
	EXPECT_TRUE(perms.Allowed(2, NetPermission::Purchase));
	EXPECT_TRUE(perms.Allowed(1, NetPermission::Dialog));
	perms.Set(1, NetPermission::Purchase, true);
	EXPECT_TRUE(perms.Allowed(1, NetPermission::Purchase));

	// the host can't be locked out, not even of other players' characters
	perms.Set(NET_HOST_SLOT, NetPermission::Pause, false);
	EXPECT_TRUE(perms.Allowed(NET_HOST_SLOT, NetPermission::Pause));
	EXPECT_TRUE(perms.Allowed(NET_HOST_SLOT, NetPermission::Control, 2));
	EXPECT_STREQ(NetPermissionName(NetPermission::CharacterRecords), "character records");

	// the lobby sends them as bits; Control is no per player permission
	NetPermissions other;
	EXPECT_EQ(other.Mask(1), 0x7eU);
	perms.Set(1, NetPermission::Leader, false);
	perms.Set(1, NetPermission::Dialog, false);
	uint32_t mask = perms.Mask(1);
	EXPECT_FALSE(mask & (1u << uint8_t(NetPermission::Leader)));
	EXPECT_FALSE(mask & (1u << uint8_t(NetPermission::Control)));
	other.SetMask(1, mask);
	EXPECT_FALSE(other.Allowed(1, NetPermission::Leader));
	EXPECT_FALSE(other.Allowed(1, NetPermission::Dialog));
	EXPECT_TRUE(other.Allowed(1, NetPermission::Pause));
	EXPECT_EQ(other.Mask(NET_HOST_SLOT), 0x7eU);

	EXPECT_EQ(NetPermissionByName("area_transition"), NetPermission::AreaTransition);
	EXPECT_EQ(NetPermissionByName("character records"), NetPermission::CharacterRecords);
	EXPECT_EQ(NetPermissionByName("leader"), NetPermission::Leader);
	EXPECT_EQ(NetPermissionByName("nonsense"), NetPermission::count);
}

TEST(NetReplicationTest, RosterRejoin)
{
	NetRoster roster;
	roster.SetHost("host");
	bool rejoined = true;
	EXPECT_EQ(roster.Join("one", 0, 101, rejoined), 1);
	EXPECT_FALSE(rejoined);
	EXPECT_EQ(roster.Join("two", 0, 102, rejoined), 2);
	EXPECT_EQ(roster.Find(2)->token, 102U);
	EXPECT_EQ(roster.Slots(), (std::vector<uint8_t> { 0, 1, 2 }));

	// one drops: it keeps its slot, but isn't connected or ready
	roster.SetReady(1, true);
	roster.Leave(1);
	EXPECT_FALSE(roster.Connected(1));
	EXPECT_FALSE(roster.Find(1)->ready);
	EXPECT_EQ(roster.Slots(), (std::vector<uint8_t> { 0, 1, 2 }));
	// someone new gets a free slot, not the one held for the player who left
	EXPECT_EQ(roster.Join("three", 0, 103, rejoined), 3);
	// the token brings one back to its old slot, under its new name
	EXPECT_EQ(roster.Join("one again", 101, 104, rejoined), 1);
	EXPECT_TRUE(rejoined);
	EXPECT_EQ(roster.Find(1)->name, "one again");
	EXPECT_EQ(roster.Find(1)->token, 101U);
	// a token is no master key: unknown ones, and ones of connected players, are just new players
	EXPECT_EQ(roster.Join("thief", 102, 105, rejoined), 0);
	EXPECT_FALSE(rejoined);

	// full: a newcomer takes the slot of whoever left, which forgets that player
	roster.Leave(2);
	EXPECT_EQ(roster.Join("four", 0, 106, rejoined), 2);
	EXPECT_FALSE(rejoined);
	EXPECT_EQ(roster.Find(2)->token, 106U);
	EXPECT_EQ(roster.Join("two", 102, 107, rejoined), 0);

	// ready checks count connected players only; the host is one of them
	EXPECT_FALSE(roster.AllReady());
	for (uint8_t slot = 0; slot <= 3; ++slot) {
		roster.SetReady(slot, true);
	}
	EXPECT_TRUE(roster.AllReady());
	roster.Leave(3);
	roster.SetReady(3, true); // gone, can't be ready
	EXPECT_FALSE(roster.Find(3)->ready);
	EXPECT_TRUE(roster.AllReady());
	roster.ClearReady();
	EXPECT_FALSE(roster.AllReady());
	// the host never leaves
	roster.Leave(NET_HOST_SLOT);
	EXPECT_TRUE(roster.Connected(NET_HOST_SLOT));
}

TEST(NetReplicationTest, SlotOwnerAndController)
{
	NetRoster roster;
	bool rejoined;
	roster.Join("one", 0, 1, rejoined);
	std::map<uint8_t, uint8_t> assigned;
	// round robin over the remembered players: PCs 1 and 3 the host's, 2 and 4 slot 1's
	EXPECT_EQ(NetSlotOwner(1, assigned, roster), 0);
	EXPECT_EQ(NetSlotOwner(2, assigned, roster), 1);
	EXPECT_EQ(NetSlotOwner(4, assigned, roster), 1);
	EXPECT_EQ(NetSlotController(2, assigned, roster), 1);
	// the host assigns PC 3 to slot 1
	assigned[3] = 1;
	EXPECT_EQ(NetSlotOwner(3, assigned, roster), 1);
	// slot 1 drops: it still owns them, but the host controls them meanwhile
	roster.Leave(1);
	EXPECT_EQ(NetSlotOwner(2, assigned, roster), 1);
	EXPECT_EQ(NetSlotController(2, assigned, roster), 0);
	EXPECT_EQ(NetSlotController(3, assigned, roster), 0);
	// and gets them back when it returns
	roster.Join("one", 1, 2, rejoined);
	EXPECT_TRUE(rejoined);
	EXPECT_EQ(NetSlotController(3, assigned, roster), 1);
	// an assignment to a player nobody remembers falls back to the default
	assigned[5] = 3;
	EXPECT_EQ(NetSlotOwner(5, assigned, roster), 0);
}

TEST(NetReplicationTest, VariablesDiff)
{
	std::map<std::string, int32_t> before = { { "A", 1 }, { "B", 2 } };
	std::map<std::string, int32_t> now = before;
	EXPECT_TRUE(NetVariablesDiff(before, now).empty());
	now["B"] = 3;
	now["C"] = 0;
	auto diff = NetVariablesDiff(before, now);
	ASSERT_EQ(diff.size(), 2U);
	EXPECT_EQ(diff[0], (NetVariable { "B", 3 }));
	EXPECT_EQ(diff[1], (NetVariable { "C", 0 }));
	EXPECT_EQ(NetVariablesDiff({}, now).size(), 3U);
}

TEST(NetReplicationTest, DialogOwner)
{
	// a player's PC talks to an NPC: that PC's owner answers
	EXPECT_EQ(NetDialogOwner(1, -1, NET_HOST_SLOT), 1);
	// an NPC talks to a PC: the PC's owner answers
	EXPECT_EQ(NetDialogOwner(-1, 2, NET_HOST_SLOT), 2);
	// two PCs: the speaker's owner
	EXPECT_EQ(NetDialogOwner(2, 1, NET_HOST_SLOT), 2);
	// no PC involved: the host
	EXPECT_EQ(NetDialogOwner(-1, -1, NET_HOST_SLOT), NET_HOST_SLOT);
}

TEST(NetReplicationTest, StatsDiff)
{
	std::vector<NetStatEntry> before = { { 0, 10, 10 }, { 1, 20, 20 }, { 2, 5, 7 }, { 3, 0, 0 } };
	std::vector<NetStatEntry> now = before;
	EXPECT_TRUE(NetStatsDiff(before, now, {}).empty());
	now[1].base = 25; // base changed
	now[2].modified = 6; // only the modified value changed
	now[3].base = now[3].modified = 9; // skipped
	auto diff = NetStatsDiff(before, now, { 3 });
	ASSERT_EQ(diff.size(), 2U);
	EXPECT_EQ(diff[0].stat, 1);
	EXPECT_EQ(diff[0].base, 25U);
	EXPECT_EQ(diff[1].stat, 2);
	EXPECT_EQ(diff[1].modified, 6U);
	// nothing known yet: everything is new
	EXPECT_EQ(NetStatsDiff({}, now, {}).size(), now.size());
}

TEST(NetReplicationTest, GuiOps)
{
	for (uint8_t raw = 1; raw < uint8_t(NetGuiOp::count); ++raw) {
		NetGuiOpInfo info = NetGuiInfo(NetGuiOp(raw));
		EXPECT_STRNE(info.name, "?") << int(raw);
		// the actor, if any, is one of the required arguments
		EXPECT_LT(info.actorArg, int(info.minArgs)) << info.name;
	}
	EXPECT_EQ(NetGuiInfo(NetGuiOp::ChangeStoreItem).permission, NetPermission::Purchase);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::AddPartyGold).permission, NetPermission::Purchase);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::DropDraggedItem).actorArg, 0);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::LeaveContainer).actorArg, -1);
	// M1.6: party-wide calls need Leader and run on the host only, record changes need CharacterRecords
	EXPECT_EQ(NetGuiInfo(NetGuiOp::RestParty).permission, NetPermission::Leader);
	EXPECT_TRUE(NetGuiInfo(NetGuiOp::RestParty).hostOnly);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::SwapPCs).permission, NetPermission::Leader);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::SetFormation).permission, NetPermission::Leader);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::MemorizeSpell).permission, NetPermission::CharacterRecords);
	EXPECT_EQ(NetGuiInfo(NetGuiOp::SetupQuickSpell).permission, NetPermission::CharacterRecords);
	EXPECT_TRUE(NetGuiInfo(NetGuiOp::SetMapnote).text);
	EXPECT_FALSE(NetGuiInfo(NetGuiOp::SetMapnote).hostOnly);
	EXPECT_STREQ(NetGuiInfo(NetGuiOp::count).name, "?");
}

TEST(NetReplicationTest, MessageHashes)
{
	NetInventory inv;
	inv.netID = 10002;
	NetItem item;
	item.slot = 15;
	item.resref = "MPTPOT";
	item.usages = { 2, 0, 0 };
	inv.items.push_back(item);
	uint64_t first = NetMessageHash(inv);
	EXPECT_EQ(first, NetMessageHash(inv));
	inv.items[0].usages[0] = 1; // a potion drunk
	EXPECT_NE(first, NetMessageHash(inv));
	inv.items[0].usages[0] = 2;
	inv.equipped = 1; // a different weapon readied
	EXPECT_NE(first, NetMessageHash(inv));

	NetSpellbook book;
	book.pages.resize(1);
	book.pages[0].memorized = { { "MPTBOLT", 1 } };
	uint64_t ready = NetMessageHash(book);
	book.pages[0].memorized[0].flags = 0; // cast
	EXPECT_NE(ready, NetMessageHash(book));
}

TEST(NetReplicationTest, AreaChangePayload)
{
	NetAreaChangeInfo info;
	// only moving, everyone in sync: the area is enough
	EXPECT_EQ(NetAreaChangeKind(info), NetAreaFollow::Area);
	NetAreaChangeInfo other = info;
	other.lightEnabled = false;
	EXPECT_EQ(NetAreaChangeKind(other), NetAreaFollow::Save);
	other = info;
	other.partyChanged = true;
	EXPECT_EQ(NetAreaChangeKind(other), NetAreaFollow::Save);
	other = info;
	other.oneArea = false;
	EXPECT_EQ(NetAreaChangeKind(other), NetAreaFollow::Save);
	other = info;
	other.noSave = true;
	EXPECT_EQ(NetAreaChangeKind(other), NetAreaFollow::Save);
	other = info;
	other.peerLive = false;
	EXPECT_EQ(NetAreaChangeKind(other), NetAreaFollow::Save);

	// the table: everything in the new area and the party, nothing from elsewhere
	std::vector<NetActorTableEntry> all(5);
	all[0].netID = 1;
	all[0].area = "MPT02";
	all[0].partySlot = 1;
	all[1].netID = 2;
	all[1].area = "MPT01";
	all[2].netID = 3;
	all[2].area = "MPT02";
	all[2].kind = NetObjectKind::Door;
	all[3].netID = 4;
	all[3].area = "MPT01";
	all[3].partySlot = 2; // left behind, still the party's
	all[4].netID = 5;
	all[4].area = "MPT02";
	auto table = NetAreaTable(all, "MPT02");
	ASSERT_EQ(table.size(), 4U);
	EXPECT_EQ(table[0].netID, 1U);
	EXPECT_EQ(table[1].netID, 3U);
	EXPECT_EQ(table[2].netID, 4U);
	EXPECT_EQ(table[3].netID, 5U);
}

}
