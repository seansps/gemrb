// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/Net/Protocol.h"

#include <gtest/gtest.h>

namespace GemRB {

TEST(NetProtocolTest, HeaderLayout)
{
	NetHello hello;
	hello.name = "Jora";
	hello.gameType = "demo";
	auto data = NetEncode(hello);
	ASSERT_GE(data.size(), NET_HEADER_SIZE);
	EXPECT_EQ(data[0], NET_MAGIC);
	EXPECT_EQ(data[1], NET_PROTOCOL_VERSION);
	EXPECT_EQ(data[2], uint8_t(NetMsgType::Hello));
	// u16 length prefixed strings, little-endian
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 2 + 4 + 2 + 4);
	EXPECT_EQ(data[3], 4);
	EXPECT_EQ(data[4], 0);
	EXPECT_EQ(data[5], 'J');

	NetMsgType type;
	ASSERT_TRUE(NetPeekType(data.data(), data.size(), type));
	EXPECT_EQ(type, NetMsgType::Hello);
}

TEST(NetProtocolTest, HelloWelcomeChatRoundTrip)
{
	NetHello hello;
	hello.name = "Jora the Seeker";
	hello.gameType = "ethren";
	NetHello hello2;
	auto data = NetEncode(hello);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), hello2));
	EXPECT_EQ(hello2.name, hello.name);
	EXPECT_EQ(hello2.gameType, hello.gameType);

	NetWelcome welcome;
	welcome.slot = 2;
	welcome.hostName = "host";
	NetWelcome welcome2;
	data = NetEncode(welcome);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), welcome2));
	EXPECT_EQ(welcome2.slot, 2);
	EXPECT_EQ(welcome2.hostName, "host");

	NetChat chat;
	chat.slot = 1;
	chat.name = "client";
	chat.text = u8"héllo wörld";
	NetChat chat2;
	data = NetEncode(chat);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), chat2));
	EXPECT_EQ(chat2.slot, 1);
	EXPECT_EQ(chat2.name, "client");
	EXPECT_EQ(chat2.text, chat.text);
}

TEST(NetProtocolTest, CommandRoundTrip)
{
	NetCommand cmd;
	cmd.actorID = 0xDEADBEEF;
	cmd.flags = NetCommand::STOP | NetCommand::QUEUE;
	cmd.action = "NIDSpecial6";
	cmd.int0 = -7;
	cmd.int1 = 3;
	cmd.int2 = 0x40000000;
	cmd.x = 533;
	cmd.y = 359;
	cmd.string0 = "MPTBOLT";
	cmd.actionFlags = 0x11;
	// only the target object is present
	cmd.objectMask = 2;
	cmd.objects[1].fields[0] = -1;
	cmd.objects[1].fields[1] = 10042;
	cmd.objects[1].filters[4] = 9;
	cmd.objects[1].rect = { 1, 2, 3, 4 };
	cmd.objects[1].name = "mptrab1";
	NetCommand cmd2;
	auto data = NetEncode(cmd);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), cmd2));
	EXPECT_EQ(cmd2.actorID, 0xDEADBEEF);
	EXPECT_EQ(cmd2.flags, NetCommand::STOP | NetCommand::QUEUE);
	EXPECT_EQ(cmd2.action, "NIDSpecial6");
	EXPECT_EQ(cmd2.int0, -7);
	EXPECT_EQ(cmd2.int1, 3);
	EXPECT_EQ(cmd2.int2, 0x40000000);
	EXPECT_EQ(cmd2.x, 533);
	EXPECT_EQ(cmd2.y, 359);
	EXPECT_EQ(cmd2.string0, "MPTBOLT");
	EXPECT_EQ(cmd2.string1, "");
	EXPECT_EQ(cmd2.actionFlags, 0x11U);
	EXPECT_EQ(cmd2.objectMask, 2);
	EXPECT_EQ(cmd2.objects[1].fields, cmd.objects[1].fields);
	EXPECT_EQ(cmd2.objects[1].filters, cmd.objects[1].filters);
	EXPECT_EQ(cmd2.objects[1].rect, cmd.objects[1].rect);
	EXPECT_EQ(cmd2.objects[1].name, "mptrab1");
	EXPECT_EQ(cmd2.objects[0].name, "");

	// absent objects take no space, unknown object bits are refused
	NetCommand bare;
	bare.action = "MoveToPoint";
	auto small = NetEncode(bare);
	EXPECT_EQ(data.size() - small.size(), 4 * (NET_OBJECT_FIELDS + NET_OBJECT_FILTERS + 4) + 2 + 7 /* the object */ + 7 /* string0 */);
	bare.objectMask = 8;
	small = NetEncode(bare);
	EXPECT_FALSE(NetDecode(small.data(), small.size(), cmd2));
}

TEST(NetProtocolTest, StateAndActorTableRoundTrip)
{
	NetActorState state;
	state.tick = 123456;
	state.flags = NetActorState::KEYFRAME | NetActorState::CHECKSUM;
	state.checksum = 0xfeedfacecafebeefULL;
	state.area = "AR0100";
	for (int i = 0; i < 6; ++i) {
		NetActorStateEntry entry;
		entry.netID = 10001 + i;
		entry.x = i * 100 - 50; // negative coordinates survive too
		entry.y = 2000000 + i;
		entry.orientation = uint8_t(i);
		entry.stance = uint8_t(10 + i);
		entry.hp = 5 - i;
		entry.maxHP = 12;
		entry.state = 0x800u << i;
		state.actors.push_back(entry);
	}
	NetActorState state2;
	auto data = NetEncode(state);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), state2));
	EXPECT_EQ(state2.tick, state.tick);
	EXPECT_EQ(state2.flags, state.flags);
	EXPECT_EQ(state2.checksum, state.checksum);
	EXPECT_EQ(state2.area, state.area);
	ASSERT_EQ(state2.actors.size(), state.actors.size());
	for (size_t i = 0; i < state.actors.size(); ++i) {
		EXPECT_EQ(state2.actors[i].netID, state.actors[i].netID);
		EXPECT_EQ(state2.actors[i].fields, NAF_ALL);
		EXPECT_EQ(state2.actors[i].x, state.actors[i].x);
		EXPECT_EQ(state2.actors[i].y, state.actors[i].y);
		EXPECT_EQ(state2.actors[i].orientation, state.actors[i].orientation);
		EXPECT_EQ(state2.actors[i].stance, state.actors[i].stance);
		EXPECT_EQ(state2.actors[i].hp, state.actors[i].hp);
		EXPECT_EQ(state2.actors[i].maxHP, 12);
		EXPECT_EQ(state2.actors[i].state, state.actors[i].state);
	}

	// a delta only carries the flagged fields
	NetActorState delta;
	delta.tick = 7;
	delta.area = "AR0100";
	NetActorStateEntry hpOnly;
	hpOnly.netID = 10001;
	hpOnly.fields = NAF_HP;
	hpOnly.hp = -3;
	hpOnly.maxHP = 9;
	hpOnly.x = 999; // not sent
	delta.actors.push_back(hpOnly);
	data = NetEncode(delta);
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 4 + 1 + 2 + 6 + 2 + 4 + 1 + 8);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), state2));
	ASSERT_EQ(state2.actors.size(), 1U);
	EXPECT_EQ(state2.checksum, 0U);
	EXPECT_EQ(state2.actors[0].fields, NAF_HP);
	EXPECT_EQ(state2.actors[0].hp, -3);
	EXPECT_EQ(state2.actors[0].maxHP, 9);
	EXPECT_EQ(state2.actors[0].x, 0);
	// unknown field bits are refused
	delta.actors[0].fields = 0x80;
	data = NetEncode(delta);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), state2));

	NetActorTable table;
	table.transferID = 7;
	table.actors.push_back({ 10005, 1, "AR0100", "protagon", 700, 500 });
	table.actors.push_back({ 10042, 0, "AR0100", "rabbit", -3, 1234567 });
	table.actors.push_back({ 10077, 0, "AR0100", "MptGate", 520, 300, NetObjectKind::Door });
	NetActorTable table2;
	data = NetEncode(table);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), table2));
	EXPECT_EQ(table2.transferID, 7U);
	ASSERT_EQ(table2.actors.size(), 3U);
	EXPECT_EQ(table2.actors[0].netID, 10005U);
	EXPECT_EQ(table2.actors[0].partySlot, 1);
	EXPECT_EQ(table2.actors[0].area, "AR0100");
	EXPECT_EQ(table2.actors[0].scriptName, "protagon");
	EXPECT_EQ(table2.actors[0].x, 700);
	EXPECT_EQ(table2.actors[0].kind, NetObjectKind::Actor);
	EXPECT_EQ(table2.actors[1].partySlot, 0);
	EXPECT_EQ(table2.actors[1].scriptName, "rabbit");
	EXPECT_EQ(table2.actors[1].x, -3);
	EXPECT_EQ(table2.actors[1].y, 1234567);
	EXPECT_EQ(table2.actors[2].kind, NetObjectKind::Door);
	EXPECT_EQ(table2.actors[2].scriptName, "MptGate");
}

TEST(NetProtocolTest, EventMessagesRoundTrip)
{
	NetSpawn spawn;
	spawn.netID = 10123;
	spawn.area = "MPT01";
	spawn.scriptName = "netrab";
	spawn.x = 400;
	spawn.y = 600;
	spawn.orientation = 9;
	spawn.creature = { 'C', 'R', 'E', ' ', 'V', '1', '.', '0', 0, 0xff };
	NetSpawn spawn2;
	auto data = NetEncode(spawn);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), spawn2));
	EXPECT_EQ(spawn2.netID, 10123U);
	EXPECT_EQ(spawn2.area, "MPT01");
	EXPECT_EQ(spawn2.scriptName, "netrab");
	EXPECT_EQ(spawn2.x, 400);
	EXPECT_EQ(spawn2.y, 600);
	EXPECT_EQ(spawn2.orientation, 9);
	EXPECT_EQ(spawn2.creature, spawn.creature);

	NetDespawn despawn;
	despawn.netID = 10123;
	NetDespawn despawn2;
	data = NetEncode(despawn);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), despawn2));
	EXPECT_EQ(despawn2.netID, 10123U);

	NetProjectile pro;
	pro.name = "MPTBOLT";
	pro.index = 1;
	pro.flags = NetProjectile::AT_POINT;
	pro.casterID = 10003;
	pro.targetID = 0;
	pro.x = 10;
	pro.y = -20;
	pro.tx = 300;
	pro.ty = 400;
	NetProjectile pro2;
	data = NetEncode(pro);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), pro2));
	EXPECT_EQ(pro2.name, "MPTBOLT");
	EXPECT_EQ(pro2.index, 1);
	EXPECT_EQ(pro2.flags, NetProjectile::AT_POINT);
	EXPECT_EQ(pro2.casterID, 10003U);
	EXPECT_EQ(pro2.y, -20);
	EXPECT_EQ(pro2.ty, 400);

	NetVfx vfx;
	vfx.key = 5;
	vfx.flags = NetVfx::TWIN | NetVfx::LOOPING;
	vfx.actorID = 10003;
	vfx.resource = "FIRESCIS";
	vfx.zOffset = 30;
	vfx.tint = 0x11223344;
	vfx.duration = 45;
	vfx.orientation = 3;
	NetVfx vfx2;
	data = NetEncode(vfx);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), vfx2));
	EXPECT_EQ(vfx2.key, 5U);
	EXPECT_EQ(vfx2.flags, vfx.flags);
	EXPECT_EQ(vfx2.actorID, 10003U);
	EXPECT_EQ(vfx2.resource, "FIRESCIS");
	EXPECT_EQ(vfx2.zOffset, 30);
	EXPECT_EQ(vfx2.tint, 0x11223344U);
	EXPECT_EQ(vfx2.duration, 45U);
	EXPECT_EQ(vfx2.orientation, 3);
	// an END is just the key
	NetVfx end;
	end.key = 5;
	end.flags = NetVfx::END;
	end.resource = "ignored";
	data = NetEncode(end);
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 5);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), vfx2));
	EXPECT_EQ(vfx2.flags, NetVfx::END);

	NetSound sound;
	sound.resource = "AMB_D01";
	sound.flags = NetSound::SPATIAL | NetSound::SPEECH;
	sound.x = 120;
	sound.y = 240;
	sound.z = -5;
	sound.channelVolume = 70;
	sound.muteDistance = 600;
	NetSound sound2;
	data = NetEncode(sound);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), sound2));
	EXPECT_EQ(sound2.resource, "AMB_D01");
	EXPECT_EQ(sound2.flags, sound.flags);
	EXPECT_EQ(sound2.x, 120);
	EXPECT_EQ(sound2.y, 240);
	EXPECT_EQ(sound2.z, -5);
	EXPECT_EQ(sound2.masterVolume, 100);
	EXPECT_EQ(sound2.channelVolume, 70);
	EXPECT_EQ(sound2.muteDistance, 600);
	// a plain sound has no position on the wire
	NetSound plain;
	plain.resource = "GAM_09";
	data = NetEncode(plain);
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 2 + 6 + 1 + 4);

	NetText text;
	text.kind = NetText::OVERHEAD;
	text.ownerID = 10002;
	text.color = 0xff0000ff;
	text.text = u8"Ouch! 5 dégâts";
	NetText text2;
	data = NetEncode(text);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), text2));
	EXPECT_EQ(text2.kind, NetText::OVERHEAD);
	EXPECT_EQ(text2.ownerID, 10002U);
	EXPECT_EQ(text2.color, 0xff0000ffU);
	EXPECT_EQ(text2.text, text.text);
	text.kind = 7;
	data = NetEncode(text);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), text2));

	NetObjectState door;
	door.netID = 10077;
	door.kind = NetObjectKind::Door;
	door.flags = 1;
	door.trapDetected = 1;
	door.items = 4;
	NetObjectState door2;
	data = NetEncode(door);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), door2));
	EXPECT_EQ(door2.netID, 10077U);
	EXPECT_EQ(door2.kind, NetObjectKind::Door);
	EXPECT_EQ(door2.flags, 1U);
	EXPECT_EQ(door2.trapDetected, 1);
	EXPECT_EQ(door2.trapped, 0);
	EXPECT_EQ(door2.items, 4);
	// actors aren't objects
	door.kind = NetObjectKind::Actor;
	data = NetEncode(door);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), door2));

	NetPause pause;
	pause.on = 1;
	pause.slot = 2;
	NetPause pause2;
	data = NetEncode(pause);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), pause2));
	EXPECT_EQ(pause2.on, 1);
	EXPECT_EQ(pause2.slot, 2);

	NetAreaChange change;
	change.area = "MPT02";
	NetAreaChange change2;
	data = NetEncode(change);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), change2));
	EXPECT_EQ(change2.area, "MPT02");

	NetOwnership owners;
	owners.entries.push_back({ 10002, 1, 0 });
	owners.entries.push_back({ 10010, 2, 1 });
	NetOwnership owners2;
	data = NetEncode(owners);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), owners2));
	ASSERT_EQ(owners2.entries.size(), 2U);
	EXPECT_EQ(owners2.entries[1].netID, 10010U);
	EXPECT_EQ(owners2.entries[1].partySlot, 2);
	EXPECT_EQ(owners2.entries[1].owner, 1);
}

TEST(NetProtocolTest, SaveTransferMessagesRoundTrip)
{
	NetSaveBegin begin;
	begin.transferID = 3;
	begin.size = 123456;
	begin.hash = 0x0123456789abcdefULL;
	begin.area = "AR0100";
	NetSaveBegin begin2;
	auto data = NetEncode(begin);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), begin2));
	EXPECT_EQ(begin2.transferID, 3U);
	EXPECT_EQ(begin2.size, 123456U);
	EXPECT_EQ(begin2.hash, 0x0123456789abcdefULL);
	EXPECT_EQ(begin2.area, "AR0100");

	// sizes above the limit are refused up front
	begin.size = uint32_t(NET_MAX_SAVE_SIZE + 1);
	data = NetEncode(begin);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), begin2));

	NetSaveChunk chunk;
	chunk.transferID = 3;
	chunk.offset = 65536;
	chunk.data = { 0, 1, 2, 0xff };
	NetSaveChunk chunk2;
	data = NetEncode(chunk);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), chunk2));
	EXPECT_EQ(chunk2.offset, 65536U);
	EXPECT_EQ(chunk2.data, chunk.data);

	// a hand-made chunk above the limit is rejected
	NetWriter w(NetMsgType::SaveChunk);
	w.U32(3);
	w.U32(0);
	std::vector<uint8_t> big(NET_SAVE_CHUNK + 1, 'z');
	w.Bytes(big.data(), big.size());
	EXPECT_FALSE(NetDecode(w.Data().data(), w.Data().size(), chunk2));

	NetReady ready;
	ready.transferID = 3;
	ready.mapped = 12;
	ready.unmatched = { 10090, 10091 };
	NetReady ready2;
	data = NetEncode(ready);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), ready2));
	EXPECT_EQ(ready2.transferID, 3U);
	EXPECT_EQ(ready2.mapped, 12);
	EXPECT_EQ(ready2.unmatched, ready.unmatched);

	NetResyncRequest resync;
	resync.reason = NetResyncRequest::DESYNC;
	NetResyncRequest resync2;
	data = NetEncode(resync);
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 1);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), resync2));
	EXPECT_EQ(resync2.reason, NetResyncRequest::DESYNC);
}

TEST(NetProtocolTest, RejectsBadPackets)
{
	NetCommand cmd;
	cmd.actorID = 42;
	cmd.action = "MoveToPoint";
	cmd.objectMask = 1;
	cmd.objects[0].name = "x";
	auto data = NetEncode(cmd);
	NetCommand out;

	// every truncation fails
	for (size_t len = 0; len < data.size(); ++len) {
		EXPECT_FALSE(NetDecode(data.data(), len, out)) << "len " << len;
	}
	// trailing garbage fails
	auto longer = data;
	longer.push_back(0);
	EXPECT_FALSE(NetDecode(longer.data(), longer.size(), out));
	// wrong message type fails
	NetChat chat;
	EXPECT_FALSE(NetDecode(data.data(), data.size(), chat));
	// foreign magic and other versions fail
	auto bad = data;
	bad[0] ^= 0xff;
	NetMsgType type;
	EXPECT_FALSE(NetPeekType(bad.data(), bad.size(), type));
	bad = data;
	bad[1] = NET_PROTOCOL_VERSION + 1;
	EXPECT_FALSE(NetPeekType(bad.data(), bad.size(), type));
	bad = data;
	bad[2] = uint8_t(NetMsgType::count);
	EXPECT_FALSE(NetPeekType(bad.data(), bad.size(), type));
	EXPECT_FALSE(NetPeekType(nullptr, 0, type));
}

TEST(NetProtocolTest, StringLimits)
{
	NetChat chat;
	chat.text = std::string(NET_MAX_STRING + 100, 'x');
	auto data = NetEncode(chat);
	NetChat out;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.text.size(), NET_MAX_STRING); // truncated by the writer

	// a hand-made length above the limit is rejected by the reader
	NetWriter w(NetMsgType::Reject);
	w.U16(uint16_t(NET_MAX_STRING + 1));
	auto raw = w.Data();
	raw.insert(raw.end(), NET_MAX_STRING + 1, 'y');
	NetReject reject;
	EXPECT_FALSE(NetDecode(raw.data(), raw.size(), reject));

	// entry counts are bounded too
	NetWriter w2(NetMsgType::ActorState);
	w2.U32(1);
	w2.U8(0);
	w2.Str("AR0100");
	w2.U16(uint16_t(NET_MAX_ENTRIES + 1));
	NetActorState state;
	EXPECT_FALSE(NetDecode(w2.Data().data(), w2.Data().size(), state));
}

}
