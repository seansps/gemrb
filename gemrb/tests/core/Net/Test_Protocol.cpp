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

TEST(NetProtocolTest, StateAndManifestRoundTrip)
{
	NetActorState state;
	state.tick = 123456;
	state.area = "AR0100";
	for (int i = 0; i < 6; ++i) {
		NetActorStateEntry entry;
		entry.globalID = 10001 + i;
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
		EXPECT_EQ(state2.actors[i].globalID, state.actors[i].globalID);
		EXPECT_EQ(state2.actors[i].x, state.actors[i].x);
		EXPECT_EQ(state2.actors[i].y, state.actors[i].y);
		EXPECT_EQ(state2.actors[i].orientation, state.actors[i].orientation);
		EXPECT_EQ(state2.actors[i].stance, state.actors[i].stance);
	}

	NetPartyManifest manifest;
	manifest.area = "AR0100";
	manifest.actors.push_back({ 10005, 1, "protagon" });
	NetPartyManifest manifest2;
	data = NetEncode(manifest);
	ASSERT_TRUE(NetDecode(data.data(), data.size(), manifest2));
	ASSERT_EQ(manifest2.actors.size(), 1U);
	EXPECT_EQ(manifest2.actors[0].globalID, 10005U);
	EXPECT_EQ(manifest2.actors[0].partySlot, 1);
	EXPECT_EQ(manifest2.actors[0].scriptName, "protagon");
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
