// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// the DCC character extension (core/DCC/Extension.h): the CRE extension and NetDCCExtension bytes,
// and the storage limits M2.5 raised (MAX_STATS, NET_MAX_STATS)

#include "ie_stats.h"

#include "../../../core/DCC/Extension.h"
#include "../../../core/Net/Protocol.h"
#include "../../../core/Scriptable/Actor.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

static CharacterExt Sample()
{
	CharacterExt ext;
	KnownSpell missile;
	missile.spell = "dwmmis";
	missile.flags = SPELL_LEARNED | SPELL_LOST;
	missile.manifestation = 3;
	missile.mercurial = { 99, 12, 96 };
	missile.mercurialSub = 4;
	missile.casts = 7;
	missile.streak = 2;
	KnownSpell sleep;
	sleep.spell = "dwslee";
	sleep.flags = SPELL_LUCKY;
	sleep.luckyMod = -2;
	ext.spells = { missile, sleep };
	Corruption horns;
	horns.table = CorruptionTable::Major;
	horns.row = 9;
	horns.spell = "dwslee";
	Corruption hands;
	hands.table = CorruptionTable::Spell;
	hands.row = 2;
	hands.sub = 5;
	hands.flags = CORR_FROM_MISFIRE;
	hands.spell = "dwmmis";
	ext.corruptions = { horns, hands };
	ext.burn.points = { 2, 0, 1 };
	ext.burn.lastBurnDay = 12;
	ext.burn.flavour = 17;
	ext.burn.policy = SpellburnPolicy::Preset;
	ext.burn.preset = 3;
	ext.burn.corruption = CorruptionBurn::Always;
	ext.disapproval.range = 4;
	ext.disapproval.flags = DISAPPROVAL_NO_RESET;
	return ext;
}

TEST(DCC_Extension, RoundTrip)
{
	CharacterExt ext = Sample();
	std::vector<uint8_t> bytes = WriteExtension(ext);
	EXPECT_EQ(ExtensionSize(bytes.data(), bytes.size()), bytes.size());
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back, ext);
	ASSERT_EQ(back.spells.size(), 2u);
	EXPECT_EQ(back.spells[0].mercurial, (std::vector<uint8_t> { 99, 12, 96 }));
	EXPECT_EQ(back.spells[1].luckyMod, -2);
	EXPECT_EQ(back.corruptions[1].table, CorruptionTable::Spell);
	EXPECT_EQ(back.corruptions[1].sub, 5);
	EXPECT_EQ(back.burn.points[2], 1);
	EXPECT_EQ(back.burn.policy, SpellburnPolicy::Preset);
	EXPECT_EQ(back.disapproval.range, 4);
	ASSERT_NE(back.FindSpell("DWMMIS"), nullptr); // case doesn't matter
	EXPECT_EQ(back.FindSpell("nope"), nullptr);
}

TEST(DCC_Extension, FunnelRecords)
{
	// Phase 3: the 0-level occupation (OCCU) and the funnel (FUNL)
	CharacterExt ext;
	ext.occupation.id = 39; // a farmer
	ext.occupation.sub = 3; // turnips
	ext.occupation.tradeGood = "fntturn";
	ext.occupation.weapon = "fntpfork";
	ext.occupation.skills = SKILL_FARMING | SKILL_ANIMALS | SKILL_OUTDOORS;
	EXPECT_FALSE(ext.Empty());
	ext.funnel.status = FunnelStatus::Alive;
	ext.funnel.order = 12;
	ext.funnel.player = 2;
	ext.funnel.flags = FUNNEL_CLASS_PENDING;
	std::vector<uint8_t> bytes = WriteExtension(ext);
	// header, OCCU (12 + 32), FUNL (12 + 16)
	EXPECT_EQ(bytes.size(), EXT_HEADER_SIZE + 44 + 28);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back, ext);
	EXPECT_EQ(back.occupation.id, 39);
	EXPECT_EQ(back.occupation.tradeGood, "fntturn");
	EXPECT_EQ(back.occupation.skills, uint32_t(SKILL_FARMING | SKILL_ANIMALS | SKILL_OUTDOORS));
	EXPECT_TRUE(back.occupation.lastWeapon.empty());
	EXPECT_EQ(back.funnel.status, FunnelStatus::Alive);
	EXPECT_EQ(back.funnel.order, 12);
	EXPECT_EQ(back.funnel.player, 2);
	EXPECT_TRUE(back.other.empty());
	// a death keeps its time and place, and the rest of a full extension stays as it was
	CharacterExt full = Sample();
	full.funnel.status = FunnelStatus::Dead;
	full.funnel.diedAt = 9000;
	full.funnel.diedIn = "fnt01";
	full.occupation = ext.occupation;
	bytes = WriteExtension(full);
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back, full);
	EXPECT_EQ(back.funnel.diedIn, "fnt01");
	EXPECT_EQ(back.funnel.player, FUNNEL_NO_PLAYER);
	EXPECT_EQ(back.spells.size(), 2u);
	// a funnel status past the known ones is refused
	CharacterExt dead;
	dead.funnel.status = FunnelStatus::Reserve;
	bytes = WriteExtension(dead);
	bytes[EXT_HEADER_SIZE + EXT_RECORD_HEADER_SIZE] = uint8_t(FunnelStatus::count);
	EXPECT_FALSE(ReadExtension(bytes.data(), bytes.size(), back));
	// tag names, both ways
	EXPECT_STREQ(OccupationSkillName(5), "SMITHING");
	EXPECT_EQ(OccupationSkillName(OCCUPATION_SKILL_COUNT), nullptr);
	EXPECT_EQ(OccupationSkillBit("cooking"), uint32_t(SKILL_COOKING));
	EXPECT_EQ(OccupationSkillBit("JUGGLING"), 0u);
	EXPECT_STREQ(FunnelStatusName(FunnelStatus::Reserve), "reserve");
}

TEST(DCC_Extension, EmptyAndDefaults)
{
	CharacterExt ext;
	EXPECT_TRUE(ext.Empty());
	std::vector<uint8_t> bytes = WriteExtension(ext);
	EXPECT_EQ(bytes.size(), EXT_HEADER_SIZE); // no records
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_TRUE(back.Empty());
	EXPECT_EQ(back.burn.policy, DEFAULT_SPELLBURN_POLICY);
	ext.burn.preset = 2;
	EXPECT_FALSE(ext.Empty());
	ext = CharacterExt();
	ext.disapproval.range = 2;
	EXPECT_FALSE(ext.Empty());
}

TEST(DCC_Extension, UnknownRecordsAreKept)
{
	// a record of a later version (M2.6's patron, Phase 3's occupation) survives a load and save
	CharacterExt ext = Sample();
	RawRecord patron;
	patron.tag = { 'P', 'A', 'T', 'R' };
	patron.version = 3;
	patron.data = { 1, 2, 3, 4, 5 };
	ext.other.push_back(patron);
	std::vector<uint8_t> bytes = WriteExtension(ext);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	ASSERT_EQ(back.other.size(), 1u);
	EXPECT_EQ(back.other[0].tag, patron.tag);
	EXPECT_EQ(back.other[0].version, 3);
	EXPECT_EQ(back.other[0].data, patron.data);
	EXPECT_EQ(WriteExtension(back), bytes);
}

TEST(DCC_Extension, BadDataIsRefused)
{
	std::vector<uint8_t> bytes = WriteExtension(Sample());
	CharacterExt out;
	out.disapproval.range = 9;
	// not an extension
	std::vector<uint8_t> junk(bytes);
	junk[0] = 'X';
	EXPECT_EQ(ExtensionSize(junk.data(), junk.size()), 0u);
	EXPECT_FALSE(ReadExtension(junk.data(), junk.size(), out));
	// cut short
	EXPECT_FALSE(ReadExtension(bytes.data(), bytes.size() - 3, out));
	// a record longer than the extension
	std::vector<uint8_t> broken(bytes);
	broken[EXT_HEADER_SIZE + 8] = 0xff;
	broken[EXT_HEADER_SIZE + 9] = 0xff;
	EXPECT_FALSE(ReadExtension(broken.data(), broken.size(), out));
	EXPECT_EQ(out.disapproval.range, 9); // untouched
	EXPECT_EQ(ExtensionSize(nullptr, 0), 0u);
}

// M2.5 raised the actor stats from 320 to 512: every DCC stat fits, and the net carries them all
TEST(DCC_StorageLimits, Stats)
{
	EXPECT_EQ(MAX_STATS, 512);
	EXPECT_LT(IE_DCC_LAST, MAX_STATS);
	EXPECT_LE(size_t(MAX_STATS), NET_MAX_STATS);
	EXPECT_EQ(std::tuple_size<Actor::stats_t>::value, size_t(MAX_STATS));
	// a full stats message, the highest stat included, round trips
	NetActorStats msg;
	msg.netID = 7;
	for (unsigned int stat = 0; stat < MAX_STATS; ++stat) {
		msg.stats.push_back({ uint16_t(stat), stat * 3, stat * 5 });
	}
	auto data = NetEncode(msg);
	NetActorStats back;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), back));
	ASSERT_EQ(back.stats.size(), size_t(MAX_STATS));
	EXPECT_EQ(back.stats.back().stat, MAX_STATS - 1);
	EXPECT_EQ(back.stats.back().modified, (MAX_STATS - 1) * 5u);
}

TEST(DCC_StorageLimits, ExtensionOverTheNet)
{
	NetDCCExtension msg;
	msg.netID = 42;
	msg.data = WriteExtension(Sample());
	auto data = NetEncode(msg);
	NetDCCExtension back;
	ASSERT_TRUE(NetDecode(data.data(), data.size(), back));
	EXPECT_EQ(back.netID, 42u);
	CharacterExt ext;
	ASSERT_TRUE(ReadExtension(back.data.data(), back.data.size(), ext));
	EXPECT_EQ(ext, Sample());
	// the extension's limit fits a message
	EXPECT_LE(EXT_MAX_SIZE, NET_MAX_DCC_EXTENSION);
}

}
}
