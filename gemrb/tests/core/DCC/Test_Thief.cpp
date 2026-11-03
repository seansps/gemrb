// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Thief skills in play and traps: who rolls with the Table 1-9 bonus (thieves' tools where the book
// asks for them, the halfling's sneak & hide), anyone's d20 + the ability on traps, the armour check
// penalty on physical checks, the dwarves' bonus on stone and earth, the disarm's and pick pocket's
// three results (a natural 1 or a miss by 5 or more), the hide DC by the light and the floor DC, the
// pick pocket DC, the trap table, backstab awareness and hiding again in a fight. docs/rules/traps.md,
// classes.md ("Ethren design: thief skills in real time", "Backstab, revised").

#include "../../../core/DCC/Thief.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

static ThiefCheckInput Thief(ThiefSkill skill, int bonus, Ability ability, int abilityMod, bool tools = true)
{
	ThiefCheckInput in;
	in.skill = skill;
	in.thief = true;
	in.tools = tools;
	in.tableBonus = bonus;
	in.ability = ability;
	in.abilityMod = abilityMod;
	return in;
}

TEST(DCC_Thief, SlotBits)
{
	EXPECT_EQ(TrapSlotBit(1), 1);
	EXPECT_EQ(TrapSlotBit(16), 0x8000);
	EXPECT_EQ(TrapSlotBit(0), 0); // not in the party
	EXPECT_EQ(TrapSlotBit(17), 0);
}

TEST(DCC_Thief, ThiefWithToolsRollsItsSkill)
{
	// Vex, a level 1 neutral thief: find trap +1 (Table 1-9), Int 9 (+0); disable trap +1, Agi 9 (+0)
	ThiefPlan plan = PlanThiefCheck(Thief(ThiefSkill::FindTrap, 1, Ability::Int, 0));
	EXPECT_EQ(plan.roller, ThiefRoller::Thief);
	EXPECT_EQ(plan.die, 20);
	EXPECT_EQ(plan.skill, 1);
	EXPECT_EQ(plan.Modifier(), 1);
	// Sly, level 2 neutral: find trap +3, Int 13 (+1), born under the loom +1
	ThiefCheckInput in = Thief(ThiefSkill::FindTrap, 3, Ability::Int, 1);
	in.augur = 1;
	EXPECT_EQ(PlanThiefCheck(in).Modifier(), 5);
}

TEST(DCC_Thief, ToolsNeeded)
{
	EXPECT_TRUE(SkillNeedsTools(ThiefSkill::PickLock));
	EXPECT_TRUE(SkillNeedsTools(ThiefSkill::FindTrap));
	EXPECT_TRUE(SkillNeedsTools(ThiefSkill::DisableTrap));
	EXPECT_TRUE(SkillNeedsTools(ThiefSkill::Climb));
	EXPECT_FALSE(SkillNeedsTools(ThiefSkill::PickPocket));
	EXPECT_FALSE(SkillNeedsTools(ThiefSkill::Hide));
	EXPECT_FALSE(SkillNeedsTools(ThiefSkill::Sneak));
	// a thief without tools finds and disarms as anyone: d20 + the ability, no bonus
	ThiefPlan plan = PlanThiefCheck(Thief(ThiefSkill::DisableTrap, 3, Ability::Agi, 2, false));
	EXPECT_EQ(plan.roller, ThiefRoller::Anyone);
	EXPECT_EQ(plan.skill, 0);
	EXPECT_EQ(plan.Modifier(), 2);
	// but hides with its bonus, tools or not
	EXPECT_TRUE(HasThiefBonus(Thief(ThiefSkill::Hide, 3, Ability::Agi, 0, false)));
	EXPECT_FALSE(HasThiefBonus(Thief(ThiefSkill::PickLock, 3, Ability::Agi, 0, false)));
}

TEST(DCC_Thief, AnyoneOnTraps)
{
	// a peasant: d20 + Int to find, d20 + Agi (or the trap's ability) to disarm
	ThiefCheckInput in;
	in.skill = ThiefSkill::FindTrap;
	in.ability = Ability::Int;
	in.abilityMod = -1;
	ThiefPlan plan = PlanThiefCheck(in);
	EXPECT_EQ(plan.roller, ThiefRoller::Anyone);
	EXPECT_EQ(plan.die, 20);
	EXPECT_EQ(plan.Modifier(), -1);
	EXPECT_FALSE(HasThiefBonus(in));
}

TEST(DCC_Thief, HalflingSneaksAndHides)
{
	ThiefCheckInput in;
	in.halfling = true;
	in.tableBonus = 3; // Table 1-18, level 1
	in.ability = Ability::Agi;
	in.abilityMod = 1;
	for (ThiefSkill skill : { ThiefSkill::Sneak, ThiefSkill::Hide }) {
		in.skill = skill;
		ThiefPlan plan = PlanThiefCheck(in);
		EXPECT_EQ(plan.roller, ThiefRoller::Halfling);
		EXPECT_EQ(plan.Modifier(), 4);
	}
	in.skill = ThiefSkill::PickLock;
	EXPECT_FALSE(HasThiefBonus(in));
}

TEST(DCC_Thief, ArmourOnPhysicalChecks)
{
	// chainmail's -5 on an Agi or Str check (equipment.md: physical checks), not on Int
	ThiefCheckInput in = Thief(ThiefSkill::Sneak, 3, Ability::Agi, 0);
	in.armor = -5;
	EXPECT_EQ(PlanThiefCheck(in).armor, -5);
	EXPECT_EQ(PlanThiefCheck(in).Modifier(), -2);
	in.skill = ThiefSkill::DisableTrap;
	in.ability = Ability::Str; // jamming a slab
	EXPECT_EQ(PlanThiefCheck(in).armor, -5);
	in.skill = ThiefSkill::FindTrap;
	in.ability = Ability::Int;
	EXPECT_EQ(PlanThiefCheck(in).armor, 0);
	EXPECT_TRUE(PhysicalAbility(Ability::Agi));
	EXPECT_FALSE(PhysicalAbility(Ability::Per));
	in.armor = 3; // never a bonus
	in.ability = Ability::Agi;
	EXPECT_EQ(PlanThiefCheck(in).armor, 0);
}

TEST(DCC_Thief, DwarvesOnStoneAndEarth)
{
	EXPECT_TRUE(DwarfKnowsTrap(TrapKind::Stonework));
	EXPECT_TRUE(DwarfKnowsTrap(TrapKind::Earthwork));
	EXPECT_FALSE(DwarfKnowsTrap(TrapKind::Woodland));
	ThiefCheckInput in;
	in.skill = ThiefSkill::FindTrap;
	in.ability = Ability::Int;
	in.dwarf = 2;
	EXPECT_EQ(PlanThiefCheck(in).dwarf, 2);
	in.skill = ThiefSkill::DisableTrap; // only finding
	EXPECT_EQ(PlanThiefCheck(in).dwarf, 0);
}

TEST(DCC_Thief, DisarmSpringsOnFiveOrANaturalOne)
{
	EXPECT_EQ(BotchOutcome(12, 14, 14), CheckOutcome::Success);
	EXPECT_EQ(BotchOutcome(20, 30, 14), CheckOutcome::Success);
	EXPECT_EQ(BotchOutcome(9, 13, 14), CheckOutcome::Miss); // by 1
	EXPECT_EQ(BotchOutcome(6, 10, 14), CheckOutcome::Miss); // by 4
	EXPECT_EQ(BotchOutcome(5, 9, 14), CheckOutcome::Botch); // by 5
	EXPECT_EQ(BotchOutcome(2, 3, 14), CheckOutcome::Botch);
	EXPECT_EQ(BotchOutcome(1, 20, 14), CheckOutcome::Botch); // a natural 1, whatever the total
	EXPECT_EQ(BOTCH_MARGIN, 5);
}

TEST(DCC_Thief, HideDCByTheLight)
{
	StealthTable table;
	EXPECT_EQ(table.Band(0).dc, 5);
	EXPECT_EQ(table.Band(30).dc, 5);
	EXPECT_EQ(table.Band(30).label, "dim");
	EXPECT_EQ(table.Band(31).dc, 10);
	EXPECT_EQ(table.Band(50).dc, 10);
	EXPECT_EQ(table.Band(75).dc, 15);
	EXPECT_EQ(table.Band(76).dc, 20);
	EXPECT_EQ(table.Band(100).dc, 20);
	EXPECT_EQ(table.Band(140).dc, 20); // brighter than the table: the last band
	table.SetBands({ { 50, 5, "dark" }, { 100, 15, "light" } });
	EXPECT_EQ(table.Band(40).label, "dark");
	EXPECT_EQ(table.Band(60).dc, 15);
}

TEST(DCC_Thief, FloorDC)
{
	FloorTable floors;
	EXPECT_EQ(floors.AreaDC("AR0100"), FLOOR_DC_DEFAULT);
	floors.SetArea("dct01", 5);
	EXPECT_EQ(floors.AreaDC("DCT01"), 5);
	EXPECT_EQ(floors.Regions("DCT01"), nullptr);
	floors.SetRegion("DCT01", "Planks", 15);
	ASSERT_NE(floors.Regions("dct01"), nullptr);
	EXPECT_EQ(floors.Regions("dct01")->at("planks"), 15);
	EXPECT_EQ(FloorLabel(5), "cushioned");
	EXPECT_EQ(FloorLabel(10), "stone");
	EXPECT_EQ(FloorLabel(15), "creaky wood");
	EXPECT_EQ(FloorLabel(20), "very noisy");
	EXPECT_EQ(FloorLabel(12), "DC 12");
}

TEST(DCC_Thief, PickPocketDC)
{
	EXPECT_EQ(PickPocketDC(0, false, false), PICKPOCKET_DC_DEFAULT);
	EXPECT_EQ(PickPocketDC(0, true, false), 5); // unaware: a loose purse
	EXPECT_EQ(PickPocketDC(12, false, false), 12); // the target's own
	EXPECT_EQ(PickPocketDC(12, false, true), 20); // fighting: watching its belongings
	EXPECT_EQ(PickPocketDC(0, true, true), 20);
	EXPECT_EQ(PickPocketDC(255, true, false), -1); // the stock "can't be picked"
	// caught on a natural 1 or a miss by 5, as a disarm
	EXPECT_EQ(BotchOutcome(3, 6, 10), CheckOutcome::Miss);
	EXPECT_EQ(BotchOutcome(1, 5, 10), CheckOutcome::Botch);
}

TEST(DCC_Thief, TrapTable)
{
	TrapTable table;
	EXPECT_TRUE(table.Empty());
	TrapInfo info;
	info.area = "dct01";
	info.object = "DctPit";
	info.kind = TrapKind::Earthwork;
	info.deadly = true;
	info.xp = 2;
	table.Set(info);
	const TrapInfo* found = table.Find("DCT01", "dctpit");
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->kind, TrapKind::Earthwork);
	EXPECT_TRUE(found->deadly);
	EXPECT_EQ(table.Find("DCT02", "dctpit"), nullptr);
	TrapKind kind = TrapKind::Mechanism;
	EXPECT_TRUE(ParseTrapKind("woodwork", kind));
	EXPECT_EQ(kind, TrapKind::Woodwork);
	EXPECT_FALSE(ParseTrapKind("laser", kind));
	EXPECT_STREQ(TrapKindName(TrapKind::Ward), "WARD");
	// the defaults of a trap without a row: a mechanism, Agi, not deadly, 1 XP
	TrapInfo defaults;
	EXPECT_EQ(defaults.disarm, Ability::Agi);
	EXPECT_TRUE(defaults.disarmable);
	EXPECT_FALSE(defaults.deadly);
	EXPECT_EQ(defaults.xp, 1);
}

TEST(DCC_Thief, BackstabUnawareOnly)
{
	// (already aware, hidden, in sight, silent, fighting, behind)
	EXPECT_EQ(TargetUnaware(false, true, true, false, true, false), Unaware::Hidden);
	EXPECT_EQ(TargetUnaware(false, false, false, true, true, false), Unaware::Unheard);
	EXPECT_EQ(TargetUnaware(false, false, false, false, true, false), Unaware::Unseen);
	EXPECT_EQ(TargetUnaware(false, false, true, false, false, true), Unaware::NotFighting);
	// from behind a target that fights another foe no longer counts
	EXPECT_EQ(TargetUnaware(false, false, true, false, true, true), Unaware::No);
	EXPECT_EQ(TargetUnaware(false, false, true, false, false, false), Unaware::No);
	// once attacked by the thief (or it saw the thief) it is aware, hidden or not
	EXPECT_EQ(TargetUnaware(true, true, false, true, false, true), Unaware::No);
	EXPECT_STREQ(UnawareText(Unaware::Hidden), "hidden");
	EXPECT_STREQ(UnawareText(Unaware::NotFighting), "not yet fighting");
}

TEST(DCC_Thief, BackstabWeapons)
{
	EXPECT_TRUE(BackstabWeapon(true, false, false)); // a short sword
	EXPECT_TRUE(BackstabWeapon(true, false, true)); // a dagger
	EXPECT_FALSE(BackstabWeapon(true, true, false)); // fists
	EXPECT_TRUE(BackstabWeapon(false, false, true)); // a thrown dagger, the blowgun (1d5)
	EXPECT_FALSE(BackstabWeapon(false, false, false)); // a sling, a bow
}

TEST(DCC_Thief, HidingAgainInAFight)
{
	const int round = 90;
	EXPECT_TRUE(LosesTrack(90, true, 90, round));
	EXPECT_TRUE(LosesTrack(200, true, 120, round));
	EXPECT_FALSE(LosesTrack(89, true, 90, round)); // not broken away a full round
	EXPECT_FALSE(LosesTrack(90, false, 90, round)); // not hidden
	EXPECT_FALSE(LosesTrack(90, true, 45, round)); // not lost a full round
	EXPECT_FALSE(LosesTrack(0, true, 90, round)); // still in reach
}

}
}
