// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC birth augurs (luck.md, Table 1-2): which lucky roll each augur's frozen
// Luck modifier applies to, and the augur table checks.

#include "../../../core/DCC/Augur.h"
#include "../../../core/DCC/Character.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

TEST(DCC_Augur, KeysRoundTrip)
{
	for (int i = 0; i < static_cast<int>(AugurTarget::count); ++i) {
		AugurTarget target = static_cast<AugurTarget>(i);
		EXPECT_EQ(ParseAugurTarget(AugurTargetKey(target)), target) << AugurTargetKey(target);
	}
	EXPECT_EQ(ParseAugurTarget("NOT_A_TARGET"), AugurTarget::None);
}

TEST(DCC_Augur, AttackAndDamage)
{
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackAll, AugurRoll::Attack, AF_MELEE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackAll, AugurRoll::Attack, AF_MISSILE));
	EXPECT_FALSE(AugurApplies(AugurTarget::AttackAll, AugurRoll::Damage, AF_MELEE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackMelee, AugurRoll::Attack, AF_MELEE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackMelee, AugurRoll::Attack, AF_MELEE | AF_UNARMED));
	EXPECT_FALSE(AugurApplies(AugurTarget::AttackMelee, AugurRoll::Attack, AF_MISSILE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackMissile, AugurRoll::Attack, AF_MISSILE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackUnarmed, AugurRoll::Attack, AF_MELEE | AF_UNARMED));
	EXPECT_FALSE(AugurApplies(AugurTarget::AttackUnarmed, AugurRoll::Attack, AF_MELEE));
	EXPECT_TRUE(AugurApplies(AugurTarget::AttackMounted, AugurRoll::Attack, AF_MELEE | AF_MOUNTED));
	EXPECT_TRUE(AugurApplies(AugurTarget::DamageAll, AugurRoll::Damage, AF_MISSILE));
	EXPECT_FALSE(AugurApplies(AugurTarget::DamageAll, AugurRoll::SpellDamage));
	EXPECT_TRUE(AugurApplies(AugurTarget::DamageMelee, AugurRoll::Damage, AF_MELEE));
	EXPECT_FALSE(AugurApplies(AugurTarget::DamageMelee, AugurRoll::Damage, AF_MISSILE));
	EXPECT_TRUE(AugurApplies(AugurTarget::DamageMissile, AugurRoll::Damage, AF_MISSILE));
	EXPECT_TRUE(AugurApplies(AugurTarget::OccupationWeapon, AugurRoll::Attack, AF_MELEE | AF_OCCUPATION_WEAPON));
	EXPECT_TRUE(AugurApplies(AugurTarget::OccupationWeapon, AugurRoll::Damage, AF_MELEE | AF_OCCUPATION_WEAPON));
	EXPECT_FALSE(AugurApplies(AugurTarget::OccupationWeapon, AugurRoll::Attack, AF_MELEE));
}

TEST(DCC_Augur, Saves)
{
	for (uint32_t save : { AF_FORT, AF_REF, AF_WILL }) {
		EXPECT_TRUE(AugurApplies(AugurTarget::SavesAll, AugurRoll::Save, save));
	}
	EXPECT_TRUE(AugurApplies(AugurTarget::SaveFort, AugurRoll::Save, AF_FORT));
	EXPECT_FALSE(AugurApplies(AugurTarget::SaveFort, AugurRoll::Save, AF_REF));
	EXPECT_TRUE(AugurApplies(AugurTarget::SaveRef, AugurRoll::Save, AF_REF));
	EXPECT_TRUE(AugurApplies(AugurTarget::SaveWill, AugurRoll::Save, AF_WILL));
	EXPECT_FALSE(AugurApplies(AugurTarget::SaveWill, AugurRoll::Save, AF_FORT));
	// guardian angel and the spider bite only count against traps and poison
	EXPECT_TRUE(AugurApplies(AugurTarget::SavesTraps, AugurRoll::Save, AF_REF | AF_TRAP));
	EXPECT_FALSE(AugurApplies(AugurTarget::SavesTraps, AugurRoll::Save, AF_REF));
	EXPECT_TRUE(AugurApplies(AugurTarget::SavesPoison, AugurRoll::Save, AF_FORT | AF_POISON));
	EXPECT_FALSE(AugurApplies(AugurTarget::SavesPoison, AugurRoll::Save, AF_FORT));
	EXPECT_FALSE(AugurApplies(AugurTarget::SavesAll, AugurRoll::SkillCheck));
}

TEST(DCC_Augur, SkillsAndTheRest)
{
	// born under the loom covers thief skills too
	EXPECT_TRUE(AugurApplies(AugurTarget::SkillChecks, AugurRoll::SkillCheck));
	EXPECT_TRUE(AugurApplies(AugurTarget::SkillChecks, AugurRoll::FindTrap));
	EXPECT_TRUE(AugurApplies(AugurTarget::SkillChecks, AugurRoll::DisableTrap));
	EXPECT_TRUE(AugurApplies(AugurTarget::SkillChecks, AugurRoll::SecretDoor));
	EXPECT_TRUE(AugurApplies(AugurTarget::Traps, AugurRoll::FindTrap));
	EXPECT_TRUE(AugurApplies(AugurTarget::Traps, AugurRoll::DisableTrap));
	EXPECT_FALSE(AugurApplies(AugurTarget::Traps, AugurRoll::SkillCheck));
	EXPECT_TRUE(AugurApplies(AugurTarget::SecretDoors, AugurRoll::SecretDoor));
	EXPECT_FALSE(AugurApplies(AugurTarget::SecretDoors, AugurRoll::FindTrap));

	const std::pair<AugurTarget, AugurRoll> single[] = {
		{ AugurTarget::SpellChecks, AugurRoll::SpellCheck },
		{ AugurTarget::SpellDamage, AugurRoll::SpellDamage },
		{ AugurTarget::TurnUnholy, AugurRoll::TurnUnholy },
		{ AugurTarget::Healing, AugurRoll::Healing },
		{ AugurTarget::ArmorClass, AugurRoll::ArmorClass },
		{ AugurTarget::Initiative, AugurRoll::Initiative },
		{ AugurTarget::HitPoints, AugurRoll::HitPoints },
		{ AugurTarget::CritTables, AugurRoll::CritTable },
		{ AugurTarget::Corruption, AugurRoll::Corruption },
		{ AugurTarget::Fumbles, AugurRoll::Fumble },
		{ AugurTarget::Languages, AugurRoll::Languages },
		{ AugurTarget::Speed, AugurRoll::Speed }
	};
	for (const auto& pair : single) {
		EXPECT_TRUE(AugurApplies(pair.first, pair.second)) << AugurTargetKey(pair.first);
		EXPECT_FALSE(AugurApplies(pair.first, AugurRoll::Attack, AF_MELEE)) << AugurTargetKey(pair.first);
		EXPECT_FALSE(AugurApplies(pair.first, AugurRoll::Save, AF_FORT)) << AugurTargetKey(pair.first);
	}
	EXPECT_FALSE(AugurApplies(AugurTarget::None, AugurRoll::Save, AF_FORT));
}

TEST(DCC_Augur, BonusIsTheFrozenModifier)
{
	EXPECT_EQ(AugurBonus(AugurTarget::SavesAll, 2, AugurRoll::Save, AF_WILL), 2);
	EXPECT_EQ(AugurBonus(AugurTarget::SavesAll, -3, AugurRoll::Save, AF_REF), -3);
	EXPECT_EQ(AugurBonus(AugurTarget::ArmorClass, 1, AugurRoll::Save, AF_REF), 0);

	// a lucky sign peasant (starting Luck 16: +2) with Sta 9, Agi 13, Per 6 at level 0
	int augur = 2;
	EXPECT_EQ(SaveTotal(0, 0, AugurBonus(AugurTarget::SavesAll, augur, AugurRoll::Save, AF_FORT), 0), 2);
	EXPECT_EQ(SaveTotal(0, 1, AugurBonus(AugurTarget::SavesAll, augur, AugurRoll::Save, AF_REF), 0), 3);
	EXPECT_EQ(SaveTotal(0, -1, AugurBonus(AugurTarget::SavesAll, augur, AugurRoll::Save, AF_WILL), 0), 1);
	// charmed house: AC; speed of the cobra: initiative; bountiful harvest: hp each level
	EXPECT_EQ(ArmorClass(0, 1, AugurBonus(AugurTarget::ArmorClass, -1, AugurRoll::ArmorClass)), 10);
	EXPECT_EQ(InitiativeMod(1, 0, AugurBonus(AugurTarget::Initiative, 3, AugurRoll::Initiative)), 4);
	EXPECT_EQ(LevelHP(1, 0, AugurBonus(AugurTarget::HitPoints, -2, AugurRoll::HitPoints)), 1);
}

TEST(DCC_Augur, TableRows)
{
	AugurTable table;
	EXPECT_TRUE(table.IsEmpty());
	EXPECT_EQ(table.Target(1), AugurTarget::None);
	EXPECT_EQ(table.Name(1), "");

	EXPECT_TRUE(table.SetRows({ { 2, "Two", AugurTarget::AttackMelee }, { 1, "One", AugurTarget::AttackAll } }));
	EXPECT_EQ(table.Count(), 2);
	EXPECT_EQ(table.Target(1), AugurTarget::AttackAll);
	EXPECT_EQ(table.Name(2), "Two");
	EXPECT_EQ(table.Target(0), AugurTarget::None);
	EXPECT_EQ(table.Target(3), AugurTarget::None);

	// gaps, duplicates, a first row other than 1 or an unknown target leave it empty
	EXPECT_FALSE(table.SetRows({ { 1, "One", AugurTarget::AttackAll }, { 3, "Three", AugurTarget::AttackAll } }));
	EXPECT_TRUE(table.IsEmpty());
	EXPECT_FALSE(table.SetRows({ { 1, "One", AugurTarget::AttackAll }, { 1, "Again", AugurTarget::AttackAll } }));
	EXPECT_FALSE(table.SetRows({ { 2, "Two", AugurTarget::AttackAll } }));
	EXPECT_FALSE(table.SetRows({ { 1, "One", AugurTarget::None } }));
	EXPECT_FALSE(table.SetRows({}));
}

}
}
