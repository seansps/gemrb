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

TEST(DCC_Extension, FunnelCauseOfDeath)
{
	// M3.2, the graveyard: who, with what, where, as text (record version 2)
	CharacterExt ext;
	ext.funnel.status = FunnelStatus::Dead;
	ext.funnel.order = 7;
	ext.funnel.diedAt = 4500;
	ext.funnel.diedIn = "fnt01";
	std::vector<uint8_t> v1 = WriteExtension(ext);
	// without a cause the record stays version 1, 16 bytes
	EXPECT_EQ(v1.size(), EXT_HEADER_SIZE + 12 + 16);
	EXPECT_EQ(v1[EXT_HEADER_SIZE + 4], 1);
	ext.funnel.killer = "Marauder";
	ext.funnel.cause = "crossbow bolt";
	ext.funnel.place = "the wooded trail";
	EXPECT_FALSE(ext.Empty());
	std::vector<uint8_t> bytes = WriteExtension(ext);
	EXPECT_EQ(bytes[EXT_HEADER_SIZE + 4], 2);
	EXPECT_EQ(bytes.size(), v1.size() + 3 + 8 + 13 + 16);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back, ext);
	EXPECT_EQ(back.funnel.killer, "Marauder");
	EXPECT_EQ(back.funnel.cause, "crossbow bolt");
	EXPECT_EQ(back.funnel.place, "the wooded trail");
	EXPECT_EQ(back.funnel.diedIn, "fnt01");
	// only a place (bled out alone): still kept
	CharacterExt alone;
	alone.funnel.status = FunnelStatus::Dead;
	alone.funnel.cause = "bled out";
	bytes = WriteExtension(alone);
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.funnel.cause, "bled out");
	EXPECT_TRUE(back.funnel.killer.empty());
	// long texts are cut at 255 bytes on a character boundary (UTF-8)
	CharacterExt longer;
	longer.funnel.status = FunnelStatus::Dead;
	longer.funnel.killer = std::string(254, 'a') + "\xc3\xa6"; // ends with a 2-byte character across the cut
	bytes = WriteExtension(longer);
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.funnel.killer, std::string(254, 'a'));
	// a truncated text is refused
	bytes = WriteExtension(ext);
	bytes.resize(bytes.size() - 4);
	bytes[8] = uint8_t(bytes.size() & 0xff);
	bytes[9] = uint8_t(bytes.size() >> 8);
	EXPECT_FALSE(ReadExtension(bytes.data(), bytes.size(), back));
}

TEST(DCC_Extension, FunnelDeeds)
{
	// M3.3, the choose-your-four cards: kills and near deaths (record version 3)
	CharacterExt ext;
	ext.funnel.status = FunnelStatus::Alive;
	ext.funnel.order = 3;
	std::vector<uint8_t> v1 = WriteExtension(ext);
	EXPECT_EQ(v1[EXT_HEADER_SIZE + 4], 1); // nothing done yet: still version 1
	ext.funnel.kills = 2;
	ext.funnel.nearDeaths = 3;
	EXPECT_FALSE(ext.Empty());
	std::vector<uint8_t> bytes = WriteExtension(ext);
	EXPECT_EQ(bytes[EXT_HEADER_SIZE + 4], 3);
	// three empty texts (a length byte each), then u16 kills, u8 near deaths, u8 0
	EXPECT_EQ(bytes.size(), v1.size() + 3 + 4);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back, ext);
	EXPECT_EQ(back.funnel.kills, 2);
	EXPECT_EQ(back.funnel.nearDeaths, 3);
	// with a cause of death too
	ext.funnel.status = FunnelStatus::Dead;
	ext.funnel.killer = "Marauder";
	ext.funnel.cause = "cudgel";
	bytes = WriteExtension(ext);
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.funnel.killer, "Marauder");
	EXPECT_EQ(back.funnel.kills, 2);
	// a version 2 reader's view: the texts end where version 2 ends; the deeds are only in 3
	ext.funnel.kills = 0;
	ext.funnel.nearDeaths = 0;
	bytes = WriteExtension(ext);
	EXPECT_EQ(bytes[EXT_HEADER_SIZE + 4], 2);
}

TEST(DCC_Extension, ClericsGod)
{
	// M3.3: the CHAR record's god (version 3, only with a god)
	CharacterExt ext;
	ext.body.weight = 140;
	std::vector<uint8_t> v2 = WriteExtension(ext);
	EXPECT_EQ(v2[EXT_HEADER_SIZE + 4], 2);
	ext.body.god = 9;
	std::vector<uint8_t> bytes = WriteExtension(ext);
	EXPECT_EQ(bytes[EXT_HEADER_SIZE + 4], 3);
	EXPECT_EQ(bytes.size(), v2.size() + 4);
	CharacterExt back;
	ASSERT_TRUE(ReadExtension(bytes.data(), bytes.size(), back));
	EXPECT_EQ(back.body.god, 9);
	EXPECT_EQ(back.body.weight, 140);
	CharacterExt godOnly;
	godOnly.body.god = 1;
	EXPECT_FALSE(godOnly.Empty());
}

static GameRecord SampleGraves()
{
	GameRecord record;
	Grave rolf;
	rolf.name = "Rolf";
	rolf.occupation = 51;
	rolf.order = 12;
	rolf.player = 1;
	rolf.level = 0;
	rolf.classID = 8;
	rolf.kills = 1;
	rolf.diedAt = 4500;
	rolf.diedIn = "fnt01";
	rolf.cause = "Slay";
	rolf.place = "the glade";
	record.graves.push_back(rolf);
	Grave mira;
	mira.name = "Mira";
	mira.occupation = 97;
	mira.order = 2;
	mira.level = 1;
	mira.classID = 1;
	mira.diedAt = 9000;
	mira.diedIn = "fnt02";
	mira.killer = "Marauder";
	mira.cause = "bled out";
	mira.place = "the cave";
	record.graves.push_back(mira);
	return record;
}

TEST(DCC_GameRecord, Graveyard)
{
	// M3.3: the game's own record, the graveyard, in the GAM and on the net
	GameRecord empty;
	EXPECT_TRUE(empty.Empty());
	EXPECT_EQ(WriteGameRecord(empty).size(), EXT_HEADER_SIZE);
	GameRecord record = SampleGraves();
	EXPECT_FALSE(record.Empty());
	std::vector<uint8_t> bytes = WriteGameRecord(record);
	EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "DCCG");
	GameRecord back;
	ASSERT_TRUE(ReadGameRecord(bytes.data(), bytes.size(), back));
	ASSERT_EQ(back.graves.size(), 2u);
	EXPECT_TRUE(back.graves[0] == record.graves[0]);
	EXPECT_TRUE(back.graves[1] == record.graves[1]);
	EXPECT_EQ(back.graves[1].killer, "Marauder");
	EXPECT_EQ(back.graves[0].player, 1);
	EXPECT_EQ(back.graves[1].player, FUNNEL_NO_PLAYER);
	EXPECT_EQ(WriteGameRecord(back), bytes);
	// found and taken back by order and time of death (a body rolled over)
	ASSERT_TRUE(back.FindGrave(12, 4500));
	EXPECT_EQ(back.FindGrave(12, 4500)->name, "Rolf");
	EXPECT_FALSE(back.FindGrave(12, 4501));
	EXPECT_TRUE(back.RemoveGrave(12, 4500));
	EXPECT_FALSE(back.RemoveGrave(12, 4500));
	EXPECT_EQ(back.graves.size(), 1u);
	// unknown records are kept
	RawRecord later;
	later.tag = { 'R', 'S', 'R', 'V' };
	later.version = 2;
	later.data = { 9, 8, 7 };
	record.other.push_back(later);
	bytes = WriteGameRecord(record);
	ASSERT_TRUE(ReadGameRecord(bytes.data(), bytes.size(), back));
	ASSERT_EQ(back.other.size(), 1u);
	EXPECT_EQ(back.other[0].data, later.data);
	EXPECT_EQ(WriteGameRecord(back), bytes);
}

TEST(DCC_GameRecord, BadDataIsRefused)
{
	std::vector<uint8_t> bytes = WriteGameRecord(SampleGraves());
	GameRecord out;
	out.graves.resize(5);
	// a character extension isn't a game record
	std::vector<uint8_t> ext = WriteExtension(Sample());
	EXPECT_FALSE(ReadGameRecord(ext.data(), ext.size(), out));
	// cut short, or a size past the data
	EXPECT_FALSE(ReadGameRecord(bytes.data(), bytes.size() - 5, out));
	std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 5);
	cut[8] = uint8_t(cut.size() & 0xff);
	cut[9] = uint8_t(cut.size() >> 8);
	EXPECT_FALSE(ReadGameRecord(cut.data(), cut.size(), out));
	EXPECT_FALSE(ReadGameRecord(nullptr, 0, out));
	EXPECT_EQ(out.graves.size(), 5u); // untouched
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
