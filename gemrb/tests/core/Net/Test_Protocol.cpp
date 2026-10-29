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
	cmd.flags = NetCommand::APPEND;
	cmd.action = "MoveToPoint([533.359])";
	NetCommand cmd2;
	auto data = NetEncode(cmd);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), cmd2));
	EXPECT_EQ(cmd2.actorID, 0xDEADBEEF);
	EXPECT_EQ(cmd2.flags, NetCommand::APPEND);
	EXPECT_EQ(cmd2.action, cmd.action);
}

TEST(NetProtocolTest, StateAndActorTableRoundTrip)
{
	NetActorState state;
	state.tick = 123456;
	state.area = "AR0100";
	for (int i = 0; i < 6; ++i) {
		NetActorStateEntry entry;
		entry.netID = 10001 + i;
		entry.x = i * 100 - 50; // negative coordinates survive too
		entry.y = 2000000 + i;
		entry.orientation = uint8_t(i);
		entry.stance = uint8_t(10 + i);
		state.actors.push_back(entry);
	}
	NetActorState state2;
	auto data = NetEncode(state);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), state2));
	EXPECT_EQ(state2.tick, state.tick);
	EXPECT_EQ(state2.area, state.area);
	ASSERT_EQ(state2.actors.size(), state.actors.size());
	for (size_t i = 0; i < state.actors.size(); ++i) {
		EXPECT_EQ(state2.actors[i].netID, state.actors[i].netID);
		EXPECT_EQ(state2.actors[i].x, state.actors[i].x);
		EXPECT_EQ(state2.actors[i].y, state.actors[i].y);
		EXPECT_EQ(state2.actors[i].orientation, state.actors[i].orientation);
		EXPECT_EQ(state2.actors[i].stance, state.actors[i].stance);
	}

	NetActorTable table;
	table.transferID = 7;
	table.actors.push_back({ 10005, 1, "AR0100", "protagon", 700, 500 });
	table.actors.push_back({ 10042, 0, "AR0100", "rabbit", -3, 1234567 });
	NetActorTable table2;
	data = NetEncode(table);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), table2));
	EXPECT_EQ(table2.transferID, 7U);
	ASSERT_EQ(table2.actors.size(), 2U);
	EXPECT_EQ(table2.actors[0].netID, 10005U);
	EXPECT_EQ(table2.actors[0].partySlot, 1);
	EXPECT_EQ(table2.actors[0].area, "AR0100");
	EXPECT_EQ(table2.actors[0].scriptName, "protagon");
	EXPECT_EQ(table2.actors[0].x, 700);
	EXPECT_EQ(table2.actors[1].partySlot, 0);
	EXPECT_EQ(table2.actors[1].scriptName, "rabbit");
	EXPECT_EQ(table2.actors[1].x, -3);
	EXPECT_EQ(table2.actors[1].y, 1234567);
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
	NetReady ready2;
	data = NetEncode(ready);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), ready2));
	EXPECT_EQ(ready2.transferID, 3U);
	EXPECT_EQ(ready2.mapped, 12);

	NetResyncRequest resync;
	data = NetEncode(resync);
	EXPECT_EQ(data.size(), NET_HEADER_SIZE);
	EXPECT_TRUE(NetDecode(data.data(), data.size(), resync));
}

TEST(NetProtocolTest, RejectsBadPackets)
{
	NetCommand cmd;
	cmd.actorID = 42;
	cmd.action = "MoveToPoint([1.2])";
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
	w2.Str("AR0100");
	w2.U16(uint16_t(NET_MAX_ENTRIES + 1));
	NetActorState state;
	EXPECT_FALSE(NetDecode(w2.Data().data(), w2.Data().size(), state));
}

}
