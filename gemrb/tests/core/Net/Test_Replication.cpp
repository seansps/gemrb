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

}
