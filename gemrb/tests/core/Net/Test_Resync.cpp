// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/Net/Resync.h"

#include <gtest/gtest.h>

#include <map>

namespace GemRB {

static std::vector<uint8_t> MakeBlob(size_t size)
{
	std::vector<uint8_t> blob(size);
	for (size_t i = 0; i < size; ++i) {
		blob[i] = uint8_t((i * 131) ^ (i >> 7));
	}
	return blob;
}

TEST(NetResyncTest, Hash)
{
	// FNV-1a 64 reference values
	EXPECT_EQ(NetHash(nullptr, 0), 0xcbf29ce484222325ULL);
	const uint8_t a[] = { 'a' };
	EXPECT_EQ(NetHash(a, 1), 0xaf63dc4c8601ec8cULL);
	const uint8_t foobar[] = { 'f', 'o', 'o', 'b', 'a', 'r' };
	EXPECT_EQ(NetHash(foobar, 6), 0x85944171f73967e8ULL);

	auto blob = MakeBlob(1000);
	uint64_t hash = NetHash(blob.data(), blob.size());
	blob[500] ^= 1;
	EXPECT_NE(NetHash(blob.data(), blob.size()), hash);
}

TEST(NetResyncTest, SafeFileNames)
{
	EXPECT_TRUE(NetIsSafeFileName("DEMO.gam"));
	EXPECT_TRUE(NetIsSafeFileName("worldmap.wmp"));
	EXPECT_TRUE(NetIsSafeFileName("AR0100_x-1.ARE"));
	EXPECT_FALSE(NetIsSafeFileName(""));
	EXPECT_FALSE(NetIsSafeFileName("."));
	EXPECT_FALSE(NetIsSafeFileName(".."));
	EXPECT_FALSE(NetIsSafeFileName(".hidden"));
	EXPECT_FALSE(NetIsSafeFileName("../evil.gam"));
	EXPECT_FALSE(NetIsSafeFileName("dir/file.gam"));
	EXPECT_FALSE(NetIsSafeFileName("dir\\file.gam"));
	EXPECT_FALSE(NetIsSafeFileName("C:file"));
	EXPECT_FALSE(NetIsSafeFileName(std::string(65, 'a')));
}

TEST(NetResyncTest, PackUnpackFiles)
{
	std::vector<NetFile> files;
	files.push_back({ "DEMO.gam", MakeBlob(1234) });
	files.push_back({ "worldmap.wmp", MakeBlob(77) });
	files.push_back({ "empty.sto", {} });
	auto packed = NetPackFiles(files);

	std::vector<NetFile> out;
	ASSERT_TRUE(NetUnpackFiles(packed.data(), packed.size(), out));
	ASSERT_EQ(out.size(), files.size());
	for (size_t i = 0; i < files.size(); ++i) {
		EXPECT_EQ(out[i].name, files[i].name);
		EXPECT_EQ(out[i].data, files[i].data);
	}

	// every truncation and trailing garbage fail
	for (size_t len = 0; len < packed.size(); len += 13) {
		EXPECT_FALSE(NetUnpackFiles(packed.data(), len, out)) << "len " << len;
	}
	auto longer = packed;
	longer.push_back(0);
	EXPECT_FALSE(NetUnpackFiles(longer.data(), longer.size(), out));

	// unsafe names are refused
	files.push_back({ "../../etc/passwd", MakeBlob(3) });
	packed = NetPackFiles(files);
	EXPECT_FALSE(NetUnpackFiles(packed.data(), packed.size(), out));
}

TEST(NetResyncTest, ChunkAndReassemble)
{
	for (size_t size : { size_t(1), NET_SAVE_CHUNK - 1, NET_SAVE_CHUNK, NET_SAVE_CHUNK + 1, 5 * NET_SAVE_CHUNK + 333 }) {
		auto blob = MakeBlob(size);
		NetSaveSender sender(9, blob, "AR0100");
		EXPECT_EQ(sender.Begin().size, size);
		EXPECT_EQ(sender.Begin().hash, NetHash(blob.data(), blob.size()));

		NetSaveReceiver receiver;
		ASSERT_EQ(receiver.Begin(sender.Begin()), NetSaveReceiver::Status::Receiving);
		size_t chunks = 0;
		NetSaveReceiver::Status status = NetSaveReceiver::Status::Receiving;
		while (!sender.Done()) {
			// round trip every chunk through the wire format too
			auto wire = NetEncode(sender.NextChunk());
			NetSaveChunk chunk;
			ASSERT_TRUE(NetDecode(wire.data(), wire.size(), chunk));
			EXPECT_LE(chunk.data.size(), NET_SAVE_CHUNK);
			status = receiver.Add(chunk);
			++chunks;
		}
		EXPECT_EQ(chunks, (size + NET_SAVE_CHUNK - 1) / NET_SAVE_CHUNK);
		ASSERT_EQ(status, NetSaveReceiver::Status::Complete) << receiver.Error();
		EXPECT_EQ(receiver.Data(), blob);
	}

	// small chunks work as well (the host's slow test mode)
	auto blob = MakeBlob(100);
	NetSaveSender sender(1, blob, "AR0100");
	NetSaveReceiver receiver;
	receiver.Begin(sender.Begin());
	while (!sender.Done()) {
		receiver.Add(sender.NextChunk(7));
	}
	EXPECT_EQ(receiver.GetStatus(), NetSaveReceiver::Status::Complete);
}

TEST(NetResyncTest, ReceiverRejectsBadTransfers)
{
	auto blob = MakeBlob(3 * NET_SAVE_CHUNK);
	NetSaveReceiver receiver;

	// corrupted data fails the hash check at the end
	{
		NetSaveSender sender(1, blob, "AR0100");
		receiver.Begin(sender.Begin());
		NetSaveReceiver::Status status = NetSaveReceiver::Status::Receiving;
		while (!sender.Done()) {
			NetSaveChunk chunk = sender.NextChunk();
			if (chunk.offset == NET_SAVE_CHUNK) chunk.data[10] ^= 0x40;
			status = receiver.Add(chunk);
		}
		EXPECT_EQ(status, NetSaveReceiver::Status::Failed);
		EXPECT_EQ(receiver.Error(), "hash mismatch");
	}
	// a missing chunk is a gap
	{
		NetSaveSender sender(2, blob, "AR0100");
		receiver.Begin(sender.Begin());
		receiver.Add(sender.NextChunk());
		sender.NextChunk();
		EXPECT_EQ(receiver.Add(sender.NextChunk()), NetSaveReceiver::Status::Failed);
	}
	// chunks of another transfer
	{
		NetSaveSender sender(3, blob, "AR0100");
		NetSaveSender other(4, blob, "AR0100");
		receiver.Begin(sender.Begin());
		EXPECT_EQ(receiver.Add(other.NextChunk()), NetSaveReceiver::Status::Failed);
	}
	// more data than announced
	{
		NetSaveSender sender(5, blob, "AR0100");
		NetSaveBegin begin = sender.Begin();
		begin.size = 10;
		receiver.Begin(begin);
		EXPECT_EQ(receiver.Add(sender.NextChunk()), NetSaveReceiver::Status::Failed);
	}
	// chunks without a transfer, and empty transfers
	receiver.Reset();
	NetSaveSender sender(6, blob, "AR0100");
	EXPECT_EQ(receiver.Add(sender.NextChunk()), NetSaveReceiver::Status::Failed);
	NetSaveBegin empty;
	EXPECT_EQ(receiver.Begin(empty), NetSaveReceiver::Status::Failed);
	// a new Begin recovers
	NetSaveSender again(7, MakeBlob(10), "AR0100");
	EXPECT_EQ(receiver.Begin(again.Begin()), NetSaveReceiver::Status::Receiving);
	EXPECT_EQ(receiver.Add(again.NextChunk()), NetSaveReceiver::Status::Complete);
}

TEST(NetResyncTest, MatchActors)
{
	// host IDs are unrelated to the client's load order
	std::vector<NetActorTableEntry> host = {
		{ 20007, 2, "AR0100", "minsc", 410, 300 },
		{ 20001, 1, "AR0100", "protagon", 400, 300 },
		{ 20050, 0, "AR0100", "rabbit", 100, 100 },
		{ 20051, 0, "AR0100", "rabbit", 900, 120 },
		{ 20060, 0, "AR0100", "guard", 500, 500 },
		{ 20061, 0, "AR0100", "guard", 500, 500 }, // stacked on the same spot
		{ 20070, 0, "AR0200", "rabbit", 100, 100 }, // same name, other area
		{ 20090, 0, "AR0100", "summon", 50, 50 }, // not saved, so the client never loads it
	};
	std::vector<NetActorTableEntry> local = {
		{ 1, 1, "AR0100", "protagon", 400, 300 },
		{ 2, 2, "AR0100", "minsc", 410, 300 },
		{ 3, 0, "AR0100", "rabbit", 905, 118 }, // nudged a little by the load
		{ 4, 0, "AR0100", "rabbit", 100, 100 },
		{ 5, 0, "AR0100", "guard", 500, 500 },
		{ 6, 0, "AR0100", "guard", 500, 500 },
		{ 7, 0, "AR0200", "rabbit", 100, 100 },
		{ 8, 0, "AR0100", "stranger", 50, 50 }, // no host counterpart
	};
	auto matches = NetMatchActors(host, local, 64);
	std::map<uint32_t, uint32_t> map(matches.begin(), matches.end());
	EXPECT_EQ(map.size(), 7U);
	EXPECT_EQ(map[20001], 1U);
	EXPECT_EQ(map[20007], 2U);
	EXPECT_EQ(map[20050], 4U);
	EXPECT_EQ(map[20051], 3U);
	EXPECT_EQ(map[20060], 5U);
	EXPECT_EQ(map[20061], 6U);
	EXPECT_EQ(map[20070], 7U);
	EXPECT_EQ(map.count(20090), 0U);

	// beyond maxDistance there is no match
	local[2].x = 2000;
	matches = NetMatchActors(host, local, 64);
	map = std::map<uint32_t, uint32_t>(matches.begin(), matches.end());
	EXPECT_EQ(map.count(20051), 0U);
	EXPECT_EQ(map[20050], 4U);
}

}
