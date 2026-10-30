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
	// u16 length prefixed strings, little-endian, then the u64 token
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 2 + 4 + 2 + 4 + 8);
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

// every truncation and any trailing byte must fail
template<typename MSG>
static void ExpectStrict(const MSG& msg)
{
	auto data = NetEncode(msg);
	MSG out;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	for (size_t len = 0; len < data.size(); ++len) {
		EXPECT_FALSE(NetDecode(data.data(), len, out)) << "len " << len;
	}
	data.push_back(0);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));
}

TEST(NetProtocolTest, DialogRoundTrip)
{
	NetDialog state;
	state.kind = NetDialog::STATE;
	state.seq = 7;
	state.owner = 1;
	state.flags = NetDialog::FREEZE;
	state.speakerID = 10002;
	state.targetID = 10011;
	state.options = { { 0, "What do you have for sale?" }, { 3, u8"Farewell, Tamsín." } };
	NetDialog out;
	auto data = NetEncode(state);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.kind, NetDialog::STATE);
	EXPECT_EQ(out.seq, 7U);
	EXPECT_EQ(out.owner, 1);
	EXPECT_EQ(out.flags, NetDialog::FREEZE);
	EXPECT_EQ(out.speakerID, 10002U);
	EXPECT_EQ(out.targetID, 10011U);
	ASSERT_EQ(out.options.size(), 2U);
	EXPECT_EQ(out.options[1].index, 3);
	EXPECT_EQ(out.options[1].text, u8"Farewell, Tamsín.");
	ExpectStrict(state);

	NetDialog line;
	line.kind = NetDialog::LINE;
	line.flags = NetDialog::PARTY;
	line.speakerID = 10002;
	line.text = "Where is the key?";
	data = NetEncode(line);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.kind, NetDialog::LINE);
	EXPECT_EQ(out.text, line.text);
	EXPECT_TRUE(out.options.empty());

	// unknown kinds and flags are rejected
	NetDialog bad = line;
	bad.kind = NetDialog::count;
	data = NetEncode(bad);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));
	bad = line;
	bad.flags = 0x80;
	data = NetEncode(bad);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));

	NetDialogChoice choice;
	choice.seq = 7;
	choice.index = 3;
	NetDialogChoice choice2;
	data = NetEncode(choice);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), choice2));
	EXPECT_EQ(choice2.seq, 7U);
	EXPECT_EQ(choice2.index, 3);
	ExpectStrict(choice);
}

TEST(NetProtocolTest, InventoryRoundTrip)
{
	NetInventory inv;
	inv.netID = 10009;
	inv.equipped = -1000; // IW_NO_EQUIPPED
	inv.equippedHeader = 2;
	NetItem potion;
	potion.slot = 21;
	potion.resref = "MPTPOT";
	potion.usages = { 2, 0, 0 };
	potion.flags = 1;
	NetItem sword;
	sword.slot = 9;
	sword.resref = "MPTSWRD";
	sword.usages = { 1, 0, 0 };
	sword.expired = 12;
	inv.items = { potion, sword };
	NetInventory out;
	auto data = NetEncode(inv);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.netID, 10009U);
	EXPECT_EQ(out.kind, NetObjectKind::Actor);
	EXPECT_EQ(out.equipped, -1000);
	EXPECT_EQ(out.equippedHeader, 2);
	ASSERT_EQ(out.items.size(), 2U);
	EXPECT_TRUE(out.items[0] == potion);
	EXPECT_TRUE(out.items[1] == sword);
	ExpectStrict(inv);

	// a ground pile: no network ID, found by area and position
	NetInventory pile;
	pile.kind = NetObjectKind::Container;
	pile.area = "MPT01";
	pile.x = 1208;
	pile.y = 1060;
	data = NetEncode(pile);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.netID, 0U);
	EXPECT_EQ(out.kind, NetObjectKind::Container);
	EXPECT_EQ(out.area, "MPT01");
	EXPECT_EQ(out.x, 1208);
	EXPECT_TRUE(out.items.empty());

	// only actors and containers have inventories
	NetInventory door = pile;
	door.kind = NetObjectKind::Door;
	data = NetEncode(door);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));
}

TEST(NetProtocolTest, StatsPartyAndSpellbookRoundTrip)
{
	NetActorStats stats;
	stats.netID = 10002;
	stats.stats = { { 44, 150, 150 }, { 36, 12, 14 } }; // IE_XP, IE_STR
	NetActorStats stats2;
	auto data = NetEncode(stats);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), stats2));
	ASSERT_EQ(stats2.stats.size(), 2U);
	EXPECT_EQ(stats2.stats[0].stat, 44);
	EXPECT_EQ(stats2.stats[0].base, 150U);
	EXPECT_EQ(stats2.stats[1].modified, 14U);
	ExpectStrict(stats);
	// the DCC stats past the IE ones travel too
	stats.stats[0].stat = uint16_t(NET_MAX_ENTRIES + 28); // IE_DCC_STATE
	data = NetEncode(stats);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), stats2));
	EXPECT_EQ(stats2.stats[0].stat, NET_MAX_ENTRIES + 28);
	// stat indices past the stat array are rejected
	stats.stats[0].stat = uint16_t(NET_MAX_STATS);
	data = NetEncode(stats);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), stats2));

	NetPartyState party;
	party.gold = 120;
	party.reputation = 100;
	NetPartyState party2;
	data = NetEncode(party);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), party2));
	EXPECT_EQ(party2.gold, 120U);
	EXPECT_EQ(party2.reputation, 100U);
	ExpectStrict(party);

	NetSpellbook book;
	book.netID = 10009;
	NetSpellPage page;
	page.type = 1;
	page.level = 0;
	page.slots = 3;
	page.slotsWithBonus = 3;
	page.known = { "MPTBOLT" };
	page.memorized = { { "MPTBOLT", 1 }, { "MPTBOLT", 0 }, { "MPTBOLT", 1 } };
	book.pages = { page };
	NetSpellbook book2;
	data = NetEncode(book);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), book2));
	ASSERT_EQ(book2.pages.size(), 1U);
	EXPECT_EQ(book2.pages[0].type, 1);
	EXPECT_EQ(book2.pages[0].slots, 3);
	ASSERT_EQ(book2.pages[0].known.size(), 1U);
	EXPECT_EQ(book2.pages[0].known[0], "MPTBOLT");
	ASSERT_EQ(book2.pages[0].memorized.size(), 3U);
	EXPECT_EQ(book2.pages[0].memorized[1].flags, 0U);
	ExpectStrict(book);
}

TEST(NetProtocolTest, GuiCallStoreAndContainerRoundTrip)
{
	NetGuiCall call;
	call.op = NetGuiOp::ChangeStoreItem;
	call.selected = 2;
	call.args = { 10010, 0, 0x41 };
	NetGuiCall call2;
	auto data = NetEncode(call);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), call2));
	EXPECT_EQ(call2.op, NetGuiOp::ChangeStoreItem);
	EXPECT_EQ(call2.selected, 2);
	EXPECT_EQ(call2.args, call.args);
	ExpectStrict(call);
	// unknown operations and too many arguments are rejected
	NetWriter w(NetMsgType::GuiCall);
	w.U8(uint8_t(NetGuiOp::count));
	w.U8(0);
	w.U16(0);
	EXPECT_FALSE(NetDecode(w.Data().data(), w.Data().size(), call2));
	NetWriter w2(NetMsgType::GuiCall);
	w2.U8(uint8_t(NetGuiOp::DragItem));
	w2.U8(0);
	w2.U16(uint16_t(NET_GUI_ARGS + 1));
	for (size_t i = 0; i <= NET_GUI_ARGS; ++i) {
		w2.I32(0);
	}
	EXPECT_FALSE(NetDecode(w2.Data().data(), w2.Data().size(), call2));

	NetStoreState store;
	store.flags = NetStoreState::OPEN;
	store.store = "MPTSTORE";
	store.ownerID = 10011;
	store.actorID = 10010;
	NetStoreItem potions;
	potions.resref = "MPTPOT";
	potions.usages = { 1, 0, 0 };
	potions.amount = 9;
	potions.purchased = 1;
	potions.flags = 0x40;
	NetStoreItem sword;
	sword.resref = "MPTSWRD";
	sword.infinite = -1;
	store.items = { potions, sword };
	NetStoreState store2;
	data = NetEncode(store);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), store2));
	EXPECT_EQ(store2.flags, NetStoreState::OPEN);
	EXPECT_EQ(store2.store, "MPTSTORE");
	EXPECT_EQ(store2.ownerID, 10011U);
	EXPECT_EQ(store2.actorID, 10010U);
	ASSERT_EQ(store2.items.size(), 2U);
	EXPECT_EQ(store2.items[0].amount, 9U);
	EXPECT_EQ(store2.items[0].purchased, 1);
	EXPECT_EQ(store2.items[0].flags, 0x40U);
	EXPECT_EQ(store2.items[1].infinite, -1);
	ExpectStrict(store);

	NetContainerOpen open;
	open.actorID = 10010;
	open.containerID = 10004;
	NetContainerOpen open2;
	data = NetEncode(open);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), open2));
	EXPECT_EQ(open2.actorID, 10010U);
	EXPECT_EQ(open2.containerID, 10004U);
	ExpectStrict(open);
}

TEST(NetProtocolTest, LobbyAndIdentityRoundTrip)
{
	NetHello hello;
	hello.name = "client";
	hello.gameType = "demo";
	hello.token = 0x0123456789ABCDEFULL;
	NetHello hello2;
	auto data = NetEncode(hello);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), hello2));
	EXPECT_EQ(hello2.token, hello.token);

	NetWelcome welcome;
	welcome.slot = 3;
	welcome.token = 42;
	welcome.flags = NetWelcome::REJOINED | NetWelcome::IN_GAME;
	NetWelcome welcome2;
	data = NetEncode(welcome);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), welcome2));
	EXPECT_EQ(welcome2.token, 42U);
	EXPECT_EQ(welcome2.flags, NetWelcome::REJOINED | NetWelcome::IN_GAME);
	data.back() = 0x80; // unknown flag
	EXPECT_FALSE(NetDecode(data.data(), data.size(), welcome2));

	NetLobbyState lobby;
	lobby.phase = NetPhase::Game;
	lobby.players.push_back(NetLobbyPlayer { 0, NetLobbyPlayer::CONNECTED | NetLobbyPlayer::READY, 0, "host", 0x7e });
	lobby.players.push_back(NetLobbyPlayer { 2, 0, 35, "gone", 0x02 });
	lobby.party.push_back(NetLobbySlot { 1, 0, 0, "Jora" });
	lobby.party.push_back(NetLobbySlot { 2, 2, 0, "" });
	NetLobbyState lobby2;
	data = NetEncode(lobby);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), lobby2));
	EXPECT_EQ(lobby2.phase, NetPhase::Game);
	ASSERT_EQ(lobby2.players.size(), 2U);
	EXPECT_EQ(lobby2.players[1].slot, 2);
	EXPECT_EQ(lobby2.players[1].flags, 0);
	EXPECT_EQ(lobby2.players[1].ping, 35);
	EXPECT_EQ(lobby2.players[1].name, "gone");
	EXPECT_EQ(lobby2.players[0].allowed, 0x7eU);
	ASSERT_EQ(lobby2.party.size(), 2U);
	EXPECT_EQ(lobby2.party[1].owner, 2);
	EXPECT_EQ(lobby2.party[1].controller, 0);
	EXPECT_EQ(lobby2.party[0].name, "Jora");
	// party slots are 1-based and at most six
	lobby.party[0].partySlot = 7;
	data = NetEncode(lobby);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), lobby2));
	lobby.party[0].partySlot = 1;
	lobby.players[1].slot = NET_MAX_PEERS + 1;
	data = NetEncode(lobby);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), lobby2));

	NetLobbyReady ready;
	ready.ready = 1;
	NetLobbyReady ready2;
	data = NetEncode(ready);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), ready2));
	EXPECT_EQ(ready2.ready, 1);
	data.back() = 2;
	EXPECT_FALSE(NetDecode(data.data(), data.size(), ready2));

	NetHeartbeat beat;
	beat.seq = 7;
	beat.time = 123456;
	beat.echo = 123400;
	beat.rtt = 12;
	NetHeartbeat beat2;
	data = NetEncode(beat);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), beat2));
	EXPECT_EQ(beat2.seq, 7U);
	EXPECT_EQ(beat2.echo, 123400U);
	EXPECT_EQ(beat2.rtt, 12);
}

TEST(NetProtocolTest, AreaDataAndGlobalsRoundTrip)
{
	NetAreaData area;
	area.transferID = 9;
	area.area = "MPT02";
	area.party.push_back(NetAreaPartyEntry { 10002, 1, 1500, 1600, 4 });
	area.party.push_back(NetAreaPartyEntry { 10008, 2, 1520, 1610, 3 });
	area.are = { 'A', 'R', 'E', 'A', 'V', '1', '.', '0', 0, 1, 2 };
	NetAreaData area2;
	auto data = NetEncode(area);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), area2));
	EXPECT_EQ(area2.transferID, 9U);
	EXPECT_EQ(area2.area, "MPT02");
	ASSERT_EQ(area2.party.size(), 2U);
	EXPECT_EQ(area2.party[1].netID, 10008U);
	EXPECT_EQ(area2.party[1].y, 1610);
	EXPECT_EQ(area2.party[1].orientation, 3);
	EXPECT_EQ(area2.are, area.are);
	data.pop_back();
	EXPECT_FALSE(NetDecode(data.data(), data.size(), area2));

	NetGlobals globals;
	globals.vars.push_back(NetVariable { "MPT_GIFT", 1 });
	globals.vars.push_back(NetVariable { "CHAPTER", -2 });
	NetGlobals globals2;
	data = NetEncode(globals);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), globals2));
	EXPECT_EQ(globals2.vars, globals.vars);

	NetGuiCall call;
	call.op = NetGuiOp::SetMapnote;
	call.args = { 100, 200, 1 };
	call.text = u8"Tamsin's hut";
	NetGuiCall call2;
	data = NetEncode(call);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), call2));
	EXPECT_EQ(call2.op, NetGuiOp::SetMapnote);
	EXPECT_EQ(call2.args, call.args);
	EXPECT_EQ(call2.text, call.text);
}

TEST(NetProtocolTest, ChatWhisperAndHistoryRoundTrip)
{
	NetChat whisper;
	whisper.slot = 2;
	whisper.flags = NetChat::WHISPER;
	whisper.target = 1;
	whisper.name = "client-two";
	whisper.targetName = "client-one";
	whisper.text = "psst";
	auto data = NetEncode(whisper);
	// header, slot, flags, target, then three strings
	EXPECT_EQ(data.size(), NET_HEADER_SIZE + 3 + 2 + 10 + 2 + 10 + 2 + 4);
	EXPECT_EQ(data[1], NET_PROTOCOL_VERSION);
	NetChat out;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.slot, 2);
	EXPECT_EQ(out.flags, NetChat::WHISPER);
	EXPECT_EQ(out.target, 1);
	EXPECT_EQ(out.targetName, "client-one");
	EXPECT_EQ(out.text, "psst");

	// unknown flags and slots past the last player are refused
	auto bad = data;
	bad[NET_HEADER_SIZE + 1] = 4;
	EXPECT_FALSE(NetDecode(bad.data(), bad.size(), out));
	bad = data;
	bad[NET_HEADER_SIZE + 2] = NET_MAX_PEERS + 1;
	EXPECT_FALSE(NetDecode(bad.data(), bad.size(), out));
	// a v5 peer's chat (no flags, target or target name) doesn't decode, and its version is refused
	bad = data;
	bad[1] = 5;
	NetMsgType type;
	EXPECT_FALSE(NetPeekType(bad.data(), bad.size(), type));

	NetChatHistory history;
	for (int i = 0; i < 50; ++i) {
		NetChat line;
		line.slot = uint8_t(i % 4);
		line.flags = i % 10 == 0 ? NetChat::SYSTEM : 0;
		line.name = "p" + std::to_string(i % 4);
		line.text = "line " + std::to_string(i);
		history.lines.push_back(line);
	}
	data = NetEncode(history);
	NetChatHistory history2;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), history2));
	// only the newest NET_CHAT_HISTORY go out, oldest first
	ASSERT_EQ(history2.lines.size(), NET_CHAT_HISTORY);
	EXPECT_EQ(history2.lines.front().text, "line 10");
	EXPECT_EQ(history2.lines.front().flags, NetChat::SYSTEM);
	EXPECT_EQ(history2.lines.back().text, "line 49");
	EXPECT_EQ(history2.lines.back().slot, 1);
	for (size_t len = 0; len < data.size(); len += 7) {
		EXPECT_FALSE(NetDecode(data.data(), len, history2)) << "len " << len;
	}
}

TEST(NetProtocolTest, PromptRoundTrip)
{
	NetPrompt prompt;
	prompt.id = 7;
	prompt.remaining = 12345;
	prompt.question.kind = PromptKind::Integer;
	prompt.question.tag = "spellburn";
	prompt.question.title = "Spellburn";
	prompt.question.text = "Burn ability points?";
	prompt.question.fields = { { "Str", 0, 3, 0 }, { "Agi", 0, 3, 1 }, { "Sta", 0, 2, 0 } };
	prompt.question.timeout = 30000;
	prompt.question.actorID = 10009;
	NetPromptVoter voter;
	voter.slot = 1;
	voter.by = 0;
	voter.away = 1;
	voter.name = "client-one";
	voter.byName = "host-player";
	prompt.voters.push_back(voter);
	auto data = NetEncode(prompt);
	NetPrompt out;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.id, 7U);
	EXPECT_EQ(out.state, NetPrompt::OPEN);
	EXPECT_EQ(out.remaining, 12345U);
	EXPECT_EQ(out.question.kind, PromptKind::Integer);
	EXPECT_EQ(out.question.tag, "spellburn");
	ASSERT_EQ(out.question.fields.size(), 3U);
	EXPECT_EQ(out.question.fields[1].label, "Agi");
	EXPECT_EQ(out.question.fields[1].value, 1);
	EXPECT_EQ(out.question.fields[2].max, 2);
	EXPECT_EQ(out.question.actorID, 10009U);
	ASSERT_EQ(out.voters.size(), 1U);
	EXPECT_EQ(out.voters[0].away, 1);
	EXPECT_EQ(out.voters[0].byName, "host-player");
	for (size_t len = 0; len < data.size(); ++len) {
		EXPECT_FALSE(NetDecode(data.data(), len, out)) << "len " << len;
	}

	// the outcome
	NetPrompt closed;
	closed.id = 8;
	closed.state = NetPrompt::CLOSED;
	closed.question.kind = PromptKind::Choice;
	closed.question.choices = { "Counter", "Ignore" };
	closed.outcome = uint8_t(PromptOutcome::TimedOut);
	closed.values = { 1 };
	closed.outcomeText = "Counterspell: no answer from client-one in time, Ignore (the default).";
	data = NetEncode(closed);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), out));
	EXPECT_EQ(out.state, NetPrompt::CLOSED);
	EXPECT_EQ(out.question.choices[1], "Ignore");
	EXPECT_EQ(out.outcome, uint8_t(PromptOutcome::TimedOut));
	EXPECT_EQ(out.values, std::vector<int32_t> { 1 });
	EXPECT_EQ(out.outcomeText, closed.outcomeText);

	// a question nobody could answer doesn't decode: a choice prompt with one choice, unknown kinds
	NetPrompt broken = closed;
	broken.question.choices = { "alone" };
	data = NetEncode(broken);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));
	data = NetEncode(closed);
	data[NET_HEADER_SIZE + 4 + 1 + 4] = uint8_t(PromptKind::count);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));
	data = NetEncode(closed);
	data[NET_HEADER_SIZE + 4] = 2; // state
	EXPECT_FALSE(NetDecode(data.data(), data.size(), out));

	NetPromptAnswer answer;
	answer.id = 7;
	answer.values = { 1, 0, 2 };
	data = NetEncode(answer);
	NetPromptAnswer answer2;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), answer2));
	EXPECT_EQ(answer2.id, 7U);
	EXPECT_EQ(answer2.values, answer.values);
	answer.values.assign(PROMPT_MAX_FIELDS + 1, 0);
	data = NetEncode(answer);
	// the writer caps the list, so it still decodes, at the cap
	ASSERT_TRUE(NetDecode(data.data(), data.size(), answer2));
	EXPECT_EQ(answer2.values.size(), PROMPT_MAX_FIELDS);
	data[NET_HEADER_SIZE + 4] = uint8_t(PROMPT_MAX_FIELDS + 1);
	EXPECT_FALSE(NetDecode(data.data(), data.size(), answer2));
}

TEST(NetProtocolTest, VersionProofReject)
{
	// the layout every version keeps: magic, version, type 3, the reason as a string
	NetReject reject;
	reject.reason = "Host runs protocol v7, you run v6.";
	auto v6 = NetEncodeReject(reject, 6);
	ASSERT_EQ(v6.size(), NET_HEADER_SIZE + 2 + reject.reason.size());
	EXPECT_EQ(v6[0], NET_MAGIC);
	EXPECT_EQ(v6[1], 6);
	EXPECT_EQ(v6[2], 3);
	EXPECT_EQ(v6[3], reject.reason.size());
	EXPECT_EQ(v6[4], 0);
	// what a v6 build reads (its own version in the header): the same bytes as ours, bar the version
	auto ours = NetEncode(reject);
	ours[1] = 6;
	EXPECT_EQ(v6, ours);

	// we don't take it as one of ours, but as a foreign Reject
	NetMsgType type;
	EXPECT_FALSE(NetPeekType(v6.data(), v6.size(), type));
	uint8_t version = 0;
	uint8_t foreignType = 0;
	ASSERT_TRUE(NetPeekForeign(v6.data(), v6.size(), version, foreignType));
	EXPECT_EQ(version, 6);
	EXPECT_EQ(foreignType, uint8_t(NetMsgType::Reject));
	NetReject out;
	ASSERT_TRUE(NetDecodeAnyReject(v6.data(), v6.size(), out, version));
	EXPECT_EQ(out.reason, reject.reason);
	EXPECT_EQ(version, 6);
	// from a newer host too
	auto v9 = NetEncodeReject(reject, 9);
	ASSERT_TRUE(NetDecodeAnyReject(v9.data(), v9.size(), out, version));
	EXPECT_EQ(version, 9);
	// and ours
	ASSERT_TRUE(NetDecodeAnyReject(ours.data(), ours.size(), out, version));
	// but nothing else
	NetHello hello;
	auto other = NetEncode(hello);
	other[1] = 6;
	EXPECT_FALSE(NetDecodeAnyReject(other.data(), other.size(), out, version));
	ASSERT_TRUE(NetPeekForeign(other.data(), other.size(), version, foreignType));
	EXPECT_EQ(foreignType, uint8_t(NetMsgType::Hello));
	EXPECT_FALSE(NetPeekForeign(NetEncode(hello).data(), NetEncode(hello).size(), version, foreignType));
	v6.push_back(0);
	EXPECT_FALSE(NetDecodeAnyReject(v6.data(), v6.size(), out, version));
	EXPECT_FALSE(NetDecodeAnyReject(nullptr, 0, out, version));
}

}
