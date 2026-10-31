// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC spellcasting rules (core/DCC/Magic.h): docs/rules/magic.md, spell-format.md, classes.md

#include "../../../core/DCC/Classes.h"
#include "../../../core/DCC/Magic.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

// a spell with the standard bands of its class and level (spell-format.md §2), the wizard's
// natural-1 row and lost failures
static SpellInfo StandardSpell(CasterKind kind, int level)
{
	SpellInfo info;
	info.kind = kind;
	info.level = level;
	std::vector<int> mins = StandardBands(kind, level);
	int firstSuccess = kind == CasterKind::Cleric ? 10 + 2 * level : (level >= 4 ? 18 : 10 + 2 * level);
	for (size_t i = 0; i < mins.size(); ++i) {
		SpellBand band;
		band.min = mins[i];
		band.natOne = kind == CasterKind::Wizard && i == 0;
		band.success = mins[i] >= firstSuccess;
		band.lost = kind == CasterKind::Wizard && mins[i] < 12;
		band.text = std::to_string(mins[i]);
		info.bands.push_back(band);
	}
	return info;
}

static SpellCheckResult Check(int natural, int casterLevel, int abilityMod, bool cleric = false, int range = 1, int other = 0)
{
	SpellCheckInput in;
	in.natural = natural;
	in.casterLevel = casterLevel;
	in.abilityMod = abilityMod;
	in.cleric = cleric;
	in.disapprovalRange = range;
	in.other = other;
	return ResolveSpellCheck(in);
}

TEST(DCC_Magic, CasterKinds)
{
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Wizard), 1), CasterKind::Wizard);
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Elf), 3), CasterKind::Wizard);
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Cleric), 1), CasterKind::Cleric);
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Wizard), 0), CasterKind::None); // level 0 doesn't cast
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Warrior), 5), CasterKind::None);
	EXPECT_EQ(CasterFor(static_cast<int>(ClassID::Thief), 5), CasterKind::None);
}

TEST(DCC_Magic, CheckTotalAndCrit)
{
	// d20 + CL + ability mod (+ armor for arcane casters, + spellburn, + other)
	SpellCheckInput in;
	in.natural = 11;
	in.casterLevel = 2;
	in.abilityMod = 1;
	in.armorPenalty = -1;
	in.spellburn = 3;
	in.other = -2;
	EXPECT_EQ(ResolveSpellCheck(in).total, 11 + 2 + 1 - 1 + 3 - 2);
	in.cleric = true; // clerics ignore armor
	EXPECT_EQ(ResolveSpellCheck(in).total, 11 + 2 + 1 + 3 - 2);
	// a natural 20 adds CL again
	SpellCheckResult crit = Check(20, 3, 2);
	EXPECT_TRUE(crit.critical);
	EXPECT_EQ(crit.total, 20 + 3 + 2 + 3);
	// a smaller die (a second action die) can't reach 20; a bigger one crits from 20 up
	in = SpellCheckInput();
	in.die = 14;
	in.natural = 14;
	EXPECT_FALSE(ResolveSpellCheck(in).critical);
	in.die = 24;
	in.natural = 22;
	in.casterLevel = 1;
	EXPECT_TRUE(ResolveSpellCheck(in).critical);
	EXPECT_EQ(ResolveSpellCheck(in).total, 24);
	// 20 points of spellburn: a natural 20
	in = SpellCheckInput();
	in.natural = 4;
	in.casterLevel = 1;
	in.spellburn = 20;
	in.autoCrit = true;
	SpellCheckResult burned = ResolveSpellCheck(in);
	EXPECT_EQ(burned.natural, 20);
	EXPECT_TRUE(burned.critical);
	EXPECT_EQ(burned.total, 20 + 1 + 20 + 1);
}

TEST(DCC_Magic, WizardBandsByLevel)
{
	// level 1: natural 1 / 2-11 lost / 12-13 first success ... 32+
	SpellInfo l1 = StandardSpell(CasterKind::Wizard, 1);
	EXPECT_EQ(l1.FirstSuccess(), 12);
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(10, 1, 0)))].min, 2); // 11: lost failure
	EXPECT_TRUE(l1.bands[size_t(l1.BandFor(Check(10, 1, 0)))].lost);
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(11, 1, 0)))].min, 12);
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(16, 1, 0)))].min, 14); // 17
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(17, 1, 0)))].min, 18);
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(18, 3, 3)))].min, 24); // 24
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(19, 5, 3, false, 1, 10)))].min, 32); // 37
	// level 2: 12-13 is a failure that isn't lost
	SpellInfo l2 = StandardSpell(CasterKind::Wizard, 2);
	const SpellBand& notLost = l2.bands[size_t(l2.BandFor(Check(10, 1, 2)))];
	EXPECT_EQ(notLost.min, 12);
	EXPECT_FALSE(notLost.success);
	EXPECT_FALSE(notLost.lost);
	EXPECT_EQ(l2.FirstSuccess(), 14);
	// level 4 and 5 share the bands (first success 18)
	EXPECT_EQ(StandardBands(CasterKind::Wizard, 4), StandardBands(CasterKind::Wizard, 5));
	EXPECT_EQ(StandardSpell(CasterKind::Wizard, 5).FirstSuccess(), 18);
	EXPECT_EQ(StandardSpell(CasterKind::Wizard, 3).FirstSuccess(), 16);
}

TEST(DCC_Magic, ClericBandsByLevel)
{
	SpellInfo l1 = StandardSpell(CasterKind::Cleric, 1);
	EXPECT_EQ(l1.FirstSuccess(), 12);
	EXPECT_FALSE(l1.bands[size_t(l1.BandFor(Check(8, 1, 2, true)))].success); // 11
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(Check(9, 1, 2, true)))].min, 12);
	SpellInfo l4 = StandardSpell(CasterKind::Cleric, 4);
	EXPECT_EQ(l4.FirstSuccess(), 18);
	// cleric level 4 differs from the wizard's at 26-27 / 28
	EXPECT_EQ(StandardBands(CasterKind::Cleric, 4)[4], 26);
	EXPECT_EQ(StandardBands(CasterKind::Cleric, 4)[5], 28);
	EXPECT_EQ(StandardBands(CasterKind::Wizard, 4)[6], 26);
	EXPECT_EQ(StandardBands(CasterKind::Wizard, 4)[7], 29);
	EXPECT_EQ(StandardSpell(CasterKind::Cleric, 5).FirstSuccess(), 20);
}

TEST(DCC_Magic, NaturalOne)
{
	SpellInfo l1 = StandardSpell(CasterKind::Wizard, 1);
	// a natural 1 is the natural-1 row, whatever the modifiers
	SpellCheckResult one = Check(1, 5, 3, false, 1, 20);
	EXPECT_TRUE(one.natOne);
	EXPECT_TRUE(l1.bands[size_t(l1.BandFor(one))].natOne);
	// a total of 1 or less without a natural 1 is the 2-11 row (spell-format.md §1)
	SpellCheckResult low = Check(2, 1, -3);
	EXPECT_FALSE(low.natOne);
	EXPECT_EQ(low.total, 0);
	EXPECT_EQ(l1.bands[size_t(l1.BandFor(low))].min, 2);
	// a cleric's natural 1 is disapproval (the range starts at 1)
	EXPECT_TRUE(Check(1, 1, 2, true).disapproval);
	// the sub-roll: 1d6 + Luck, the common pattern (Magic Missile)
	std::vector<NatOneRow> rows { { -99, NAT1_CORRUPT | NAT1_TAINT | NAT1_MISFIRE }, { 1, NAT1_CORRUPT }, { 3, NAT1_TAINT }, { 4, NAT1_MISFIRE } };
	EXPECT_EQ(NatOneFor(rows, 0), uint32_t(NAT1_CORRUPT | NAT1_TAINT | NAT1_MISFIRE));
	EXPECT_EQ(NatOneFor(rows, -2), uint32_t(NAT1_CORRUPT | NAT1_TAINT | NAT1_MISFIRE));
	EXPECT_EQ(NatOneFor(rows, 2), uint32_t(NAT1_CORRUPT));
	EXPECT_EQ(NatOneFor(rows, 3), uint32_t(NAT1_TAINT));
	EXPECT_EQ(NatOneFor(rows, 9), uint32_t(NAT1_MISFIRE));
	uint32_t parsed = 0;
	EXPECT_TRUE(ParseNatOne("CORRUPT+TAINT+MISFIRE", parsed));
	EXPECT_EQ(parsed, uint32_t(NAT1_CORRUPT | NAT1_TAINT | NAT1_MISFIRE));
	EXPECT_FALSE(ParseNatOne("CORRUPT+NOPE", parsed));
}

TEST(DCC_Magic, CursedChecksFail)
{
	SpellInfo l1 = StandardSpell(CasterKind::Wizard, 1);
	SpellCheckInput in;
	in.natural = 19;
	in.casterLevel = 3;
	in.cursed = true;
	SpellCheckResult cursed = ResolveSpellCheck(in);
	const SpellBand& band = l1.bands[size_t(l1.BandFor(cursed))];
	EXPECT_FALSE(band.success);
	EXPECT_FALSE(band.natOne);
}

TEST(DCC_Magic, DisapprovalRange)
{
	// the range starts at 1 and widens by one with each failure, up to 20
	EXPECT_TRUE(InDisapprovalRange(1, 1));
	EXPECT_FALSE(InDisapprovalRange(2, 1));
	int range = 1;
	range = DisapprovalWidened(range);
	EXPECT_EQ(range, 2);
	EXPECT_TRUE(InDisapprovalRange(2, range));
	range = DisapprovalWidened(DisapprovalWidened(range));
	EXPECT_EQ(range, 4);
	EXPECT_EQ(DisapprovalWidened(20), 20);
	// a natural in the range fails even if the total would succeed
	SpellCheckResult inRange = Check(3, 5, 3, true, 4);
	EXPECT_TRUE(inRange.disapproval);
	SpellInfo l1 = StandardSpell(CasterKind::Cleric, 1);
	EXPECT_FALSE(l1.bands[size_t(l1.BandFor(inRange))].success);
	EXPECT_FALSE(Check(5, 1, 0, true, 4).disapproval);
	// the roll: (natural)d4 - Luck mod, clamped to 1-20 (Ethren decision)
	EXPECT_EQ(DisapprovalRoll(3, 0), 3);
	EXPECT_EQ(DisapprovalRoll(3, 2), 1);
	EXPECT_EQ(DisapprovalRoll(2, 3), 1);
	EXPECT_EQ(DisapprovalRoll(18, -3), 20);
	EXPECT_EQ(DisapprovalRoll(24, 0), 20);
	// sacrifice: 50 gp a point, never below 1
	EXPECT_EQ(DisapprovalAfterSacrifice(5, 100), 3);
	EXPECT_EQ(DisapprovalAfterSacrifice(5, 149), 3);
	EXPECT_EQ(DisapprovalAfterSacrifice(3, 1000), 1);
	EXPECT_EQ(DisapprovalAfterSacrifice(1, 50), 1);
}

TEST(DCC_Magic, Spellburn)
{
	// no ability below 1 (Ethren decision)
	EXPECT_EQ(SpellburnMax({ 10, 12, 8 }), 9 + 11 + 7);
	EXPECT_EQ(SpellburnMax({ 1, 1, 1 }), 0);
	EXPECT_EQ(SpellburnMax({ 0, 3, 1 }), 2);
	EXPECT_TRUE(SpellburnLegal({ 9, 0, 7 }, { 10, 12, 8 }));
	EXPECT_FALSE(SpellburnLegal({ 10, 0, 0 }, { 10, 12, 8 }));
	EXPECT_FALSE(SpellburnLegal({ -1, 0, 0 }, { 10, 12, 8 }));
	// +1 a point; a lost spell costs its level first
	EXPECT_EQ(SpellburnBonus(4, false, 2), 4);
	EXPECT_EQ(SpellburnBonus(4, true, 2), 2);
	EXPECT_EQ(SpellburnBonus(1, true, 2), -1);
	EXPECT_EQ(SpellburnBonus(0, true, 1), -1);
	EXPECT_EQ(SpellburnBonus(0, false, 1), 0);
	EXPECT_EQ(SPELLBURN_AUTO_CRIT, 20);
	// recovery: 1 point a day, from the most burned ability (Str first on a tie)
	EXPECT_EQ(SpellburnRecoveryIndex({ 0, 0, 0 }), -1);
	EXPECT_EQ(SpellburnRecoveryIndex({ 1, 3, 2 }), 1);
	EXPECT_EQ(SpellburnRecoveryIndex({ 2, 2, 2 }), 0);
	EXPECT_EQ(SpellburnRecoveryIndex({ 0, 0, 1 }), 2);
}

TEST(DCC_Magic, MercurialRoll)
{
	// d% + Luck mod x 10, clamped to 1-100 (Ethren decision)
	EXPECT_EQ(MercurialRoll(50, 0), 50);
	EXPECT_EQ(MercurialRoll(50, 2), 70);
	EXPECT_EQ(MercurialRoll(50, -3), 20);
	EXPECT_EQ(MercurialRoll(5, -3), 1);
	EXPECT_EQ(MercurialRoll(95, 3), 100);
	EXPECT_EQ(MercurialReroll(40, 1), 50);
	EXPECT_EQ(MercurialReroll(80, 3), 100);
	// the die: steps on the dice chain from the action die
	EXPECT_EQ(MercurialDie(20, {}, 0), 20);
	EXPECT_EQ(MercurialDie(20, { MERC_DIE_UP1 }, 0), 24);
	EXPECT_EQ(MercurialDie(20, { MERC_DIE_UP2 }, 0), 30);
	EXPECT_EQ(MercurialDie(20, { MERC_DIE_DOWN1 }, 0), 16);
	EXPECT_EQ(MercurialDie(20, { MERC_DIE_DOWN2 }, 0), 14);
	EXPECT_EQ(MercurialDie(14, { MERC_DIE_UP1 }, 0), 16); // a second action die moves too
	EXPECT_EQ(MercurialDie(20, { MERC_DIE_DOWN1, MERC_DIE_UP2 }, 0), 24);
	// 38: a d6 picks the die each cast, whatever else
	EXPECT_EQ(MercurialDie(20, { MERC_RANDOM_DIE, MERC_DIE_UP2 }, 1), 16);
	EXPECT_EQ(MercurialDie(20, { MERC_RANDOM_DIE }, 4), 20);
	EXPECT_EQ(MercurialDie(20, { MERC_RANDOM_DIE }, 5), 24);
	EXPECT_EQ(MercurialDie(14, { MERC_RANDOM_DIE }, 6), 30);
}

TEST(DCC_Magic, CorruptionRoll)
{
	// 1d10 - spell level + Luck mod, 1 or less row 1, 10 or more row 10
	EXPECT_EQ(CorruptionRoll(5, 1, 0), 4);
	EXPECT_EQ(CorruptionRoll(5, 1, 2), 6);
	EXPECT_EQ(CorruptionRoll(1, 3, -1), 1);
	EXPECT_EQ(CorruptionRoll(10, 1, 3), 10);
	EXPECT_EQ(CorruptionRoll(10, 0, 0), 10);
}

TEST(DCC_Magic, SpellLists)
{
	std::vector<SpellEntry> list { { 1, "MUT:HANDS", "a" }, { 2, "MINOR", "b" }, { 5, "MAJOR", "c" } };
	EXPECT_EQ(EntryFor(list, 1)->text, "a");
	EXPECT_EQ(EntryFor(list, 4)->text, "b"); // a gap reads the entry below
	EXPECT_EQ(EntryFor(list, 9)->text, "c");
	EXPECT_EQ(EntryFor(list, 0)->text, "a");
	EXPECT_EQ(EntryFor({}, 1), nullptr);
	MagicTableList table;
	table.Set({ { 1, 0, "A", "one" }, { 41, 0, "", "no change" }, { 61, 0, "B", "sixty-one" }, { 100, 0, "", "top" } });
	EXPECT_EQ(table.Lookup(50)->text, "no change");
	EXPECT_EQ(table.Lookup(-5)->text, "one");
	EXPECT_EQ(table.Lookup(200)->text, "top");
}

TEST(DCC_Magic, LayOnHands)
{
	// classes.md: 12-13, 14-19, 20-21, 22+ by same / adjacent / opposed, capped by the target's HD
	EXPECT_EQ(LayOnHandsDice(11, Kinship::Same, 10), 0);
	EXPECT_EQ(LayOnHandsDice(12, Kinship::Same, 10), 2);
	EXPECT_EQ(LayOnHandsDice(13, Kinship::Adjacent, 10), 1);
	EXPECT_EQ(LayOnHandsDice(19, Kinship::Same, 10), 3);
	EXPECT_EQ(LayOnHandsDice(14, Kinship::Opposed, 10), 1);
	EXPECT_EQ(LayOnHandsDice(21, Kinship::Adjacent, 10), 3);
	EXPECT_EQ(LayOnHandsDice(25, Kinship::Same, 10), 5);
	EXPECT_EQ(LayOnHandsDice(25, Kinship::Opposed, 10), 3);
	EXPECT_EQ(LayOnHandsDice(25, Kinship::Same, 2), 2);
	EXPECT_EQ(KinshipFor(Alignment::Lawful, Alignment::Lawful), Kinship::Same);
	EXPECT_EQ(KinshipFor(Alignment::Lawful, Alignment::Neutral), Kinship::Adjacent);
	EXPECT_EQ(KinshipFor(Alignment::Neutral, Alignment::Chaotic), Kinship::Adjacent);
	EXPECT_EQ(KinshipFor(Alignment::Chaotic, Alignment::Lawful), Kinship::Opposed);
}

TEST(DCC_Magic, TurnCells)
{
	TurnCell cell;
	EXPECT_TRUE(ParseTurnCell("NE", cell));
	EXPECT_EQ(cell.kind, TurnCell::Kind::None);
	EXPECT_TRUE(ParseTurnCell("T1", cell));
	EXPECT_EQ(cell.kind, TurnCell::Kind::Turn);
	EXPECT_EQ(cell.Count(3, [](int) { return 1; }), 1);
	EXPECT_TRUE(ParseTurnCell("T1d4+CL", cell));
	EXPECT_EQ(cell.Count(2, [](int sides) { return sides; }), 4 + 2);
	EXPECT_TRUE(ParseTurnCell("T1d8+CL;D1d4;NS", cell));
	EXPECT_TRUE(cell.noSave);
	EXPECT_EQ(cell.damageDice, 1);
	EXPECT_EQ(cell.damageSides, 4);
	EXPECT_TRUE(ParseTurnCell("K2d6+CL", cell));
	EXPECT_EQ(cell.kind, TurnCell::Kind::Kill);
	EXPECT_TRUE(cell.noSave);
	EXPECT_FALSE(ParseTurnCell("X3", cell));
	EXPECT_EQ(TurnColumn(0), 0);
	EXPECT_EQ(TurnColumn(1), 0);
	EXPECT_EQ(TurnColumn(2), 1);
	EXPECT_EQ(TurnColumn(4), 2);
	EXPECT_EQ(TurnColumn(6), 3);
	EXPECT_EQ(TurnColumn(12), 6);
	EXPECT_EQ(TurnColumn(20), 6);
}

TEST(DCC_Magic, Mutations)
{
	EXPECT_EQ(MutationBit("SCALES"), uint32_t(MUT_SCALES));
	EXPECT_EQ(MutationBit("hands"), uint32_t(MUT_HANDS));
	EXPECT_EQ(MutationBit("NOPE"), uint32_t(MUT_NONE));
	EXPECT_STREQ(MutationName(MUT_HORNS), "horns");
	EXPECT_EQ(MutationTint(0), 0u);
	EXPECT_NE(MutationTint(MUT_SCALES), 0u);
	// the most striking wins
	EXPECT_EQ(MutationTint(MUT_SCALES | MUT_HAIR), MutationTint(MUT_SCALES));
}

}
}
