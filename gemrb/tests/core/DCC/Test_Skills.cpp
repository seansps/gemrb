// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Occupation skill checks (M3.4): the tags a check asks for, trained (d20) vs untrained (d10), d10 + 2
// with some familiarity, ANYONE and UNTRAINABLE, the ability, augur and situational modifiers, the DC,
// the thief's Table 1-9 overlap and the occupation's cap there, the party's best placed roller, and
// Luck burned on a failed check. docs/rules/funnel.md, "Skills (Chapter 2)" and "Ethren design
// proposal: occupation skills"; classes.md, "Interplay with class skills".

#include "../../../core/DCC/Character.h"
#include "../../../core/DCC/Extension.h"
#include "../../../core/DCC/Luck.h"
#include "../../../core/DCC/Skills.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

// a small table: the rows the tests use, as dccskill.2da has them
static SkillTable Table()
{
	SkillTable table;
	auto row = [&table](uint32_t bit, Ability ability, uint32_t familiar, int thief = -1, int cap = 0, const char* label = "") {
		SkillInfo info;
		info.bit = bit;
		info.ability = ability;
		info.familiar = familiar;
		info.thief = thief;
		info.cap = cap;
		info.label = label;
		table.Set(info);
	};
	row(SKILL_FARMING, Ability::Int, SKILL_HERBALISM | SKILL_ANIMALS | SKILL_COOKING, -1, 0, "Farming");
	row(SKILL_SMITHING, Ability::Str, SKILL_MINING | SKILL_LOCKS | SKILL_MILITARY, -1, 0, "Smithing");
	row(SKILL_MINING, Ability::Int, SKILL_MASONRY | SKILL_DIGGING | SKILL_SMITHING, -1, 0, "Mining");
	row(SKILL_LOCKS, Ability::Agi, SKILL_SMITHING | SKILL_CARPENTRY, static_cast<int>(ThiefSkill::PickLock), 10, "Locks");
	row(SKILL_CLIMB, Ability::Str, SKILL_ROPE | SKILL_OUTDOORS, static_cast<int>(ThiefSkill::Climb), 0, "Climbing");
	row(SKILL_OUTDOORS, Ability::Int, SKILL_ANIMALS | SKILL_CLIMB, -1, 0, "Outdoors");
	return table;
}

static SkillPlanInput Input(uint32_t known, const char* wanted, int dc, int abilityMod = 0)
{
	SkillPlanInput in;
	in.known = known;
	in.wanted = ParseSkillTags(wanted);
	in.dc = dc;
	in.abilityMod = abilityMod;
	return in;
}

TEST(DCC_Skills, ParsesTags)
{
	SkillTags tags = ParseSkillTags("smithing|Mining");
	EXPECT_TRUE(tags.valid);
	EXPECT_EQ(tags.bits, uint32_t(SKILL_SMITHING | SKILL_MINING));
	EXPECT_FALSE(tags.anyone);
	EXPECT_EQ(SkillTagsKey(tags), "SMITHING|MINING");
	// commas and spaces separate too; the special tags
	tags = ParseSkillTags("FARMING, ANYONE");
	EXPECT_TRUE(tags.valid && tags.anyone);
	EXPECT_EQ(SkillTagsKey(tags), "FARMING|ANYONE");
	tags = ParseSkillTags("UNTRAINABLE");
	EXPECT_TRUE(tags.valid && tags.untrainable);
	EXPECT_EQ(tags.bits, 0u);
	// an unknown name or nothing at all is refused
	EXPECT_FALSE(ParseSkillTags("SMITHING|JUGGLING").valid);
	EXPECT_FALSE(ParseSkillTags("").valid);
	EXPECT_FALSE(ParseSkillTags(" | ").valid);
	EXPECT_EQ(SkillTagCount(SKILL_SMITHING | SKILL_MINING | SKILL_COOKING), 3);
}

TEST(DCC_Skills, TrainedRollsD20UntrainedD10)
{
	SkillTable table = Table();
	// a blacksmith on a smithing task: trained, d20
	SkillPlan plan = PlanSkillCheck(table, Input(SKILL_SMITHING | SKILL_APPRAISAL, "SMITHING", 15, 1));
	EXPECT_EQ(plan.training, SkillTraining::Trained);
	EXPECT_EQ(plan.die, 20);
	EXPECT_EQ(plan.matched, uint32_t(SKILL_SMITHING));
	EXPECT_EQ(plan.Modifier(), 1);
	// a farmer on it: untrained, d10, no familiarity
	plan = PlanSkillCheck(table, Input(SKILL_FARMING | SKILL_ANIMALS | SKILL_OUTDOORS, "SMITHING", 15, 1));
	EXPECT_EQ(plan.training, SkillTraining::Untrained);
	EXPECT_EQ(plan.die, 10);
	EXPECT_EQ(plan.familiarity, 0);
	// any wanted tag will do
	plan = PlanSkillCheck(table, Input(SKILL_MINING, "SMITHING|MINING", 10));
	EXPECT_EQ(plan.training, SkillTraining::Trained);
	EXPECT_EQ(plan.matched, uint32_t(SKILL_MINING));
}

TEST(DCC_Skills, FamiliarIsD10PlusTwo)
{
	SkillTable table = Table();
	// a miner on smithing (SMITHING lists MINING as familiar): d10 + 2
	SkillPlan plan = PlanSkillCheck(table, Input(SKILL_MINING | SKILL_MASONRY, "SMITHING", 10, -1));
	EXPECT_EQ(plan.training, SkillTraining::Familiar);
	EXPECT_EQ(plan.die, 10);
	EXPECT_EQ(plan.familiarity, SKILL_FAMILIAR_BONUS);
	EXPECT_EQ(plan.matched, uint32_t(SKILL_MINING));
	EXPECT_FALSE(plan.capped);
	EXPECT_EQ(plan.Modifier(), 1); // +2 familiar, -1 ability
}

TEST(DCC_Skills, AnyoneAndUntrainable)
{
	SkillTable table = Table();
	// a task any adult could try: d20 for everyone, with no tags at all
	SkillPlan plan = PlanSkillCheck(table, Input(0, "ANYONE", 10));
	EXPECT_EQ(plan.training, SkillTraining::Anyone);
	EXPECT_EQ(plan.die, 20);
	// nobody is trained in the untrainable, not even with the tag, and no familiarity either
	plan = PlanSkillCheck(table, Input(SKILL_SMITHING | SKILL_MINING, "SMITHING|UNTRAINABLE", 10));
	EXPECT_EQ(plan.training, SkillTraining::Untrained);
	EXPECT_EQ(plan.die, 10);
	EXPECT_EQ(plan.familiarity, 0);
}

TEST(DCC_Skills, ModifiersAddUp)
{
	SkillTable table = Table();
	SkillPlanInput in = Input(SKILL_FARMING, "FARMING", 12, 2);
	in.augur = 1; // born under the loom: the frozen Luck modifier on skill checks
	in.situational = -2;
	SkillPlan plan = PlanSkillCheck(table, in);
	EXPECT_EQ(plan.abilityMod, 2);
	EXPECT_EQ(plan.augur, 1);
	EXPECT_EQ(plan.situational, -2);
	EXPECT_EQ(plan.Modifier(), 1);
	// the roll itself: die + modifier >= DC, no natural 1 or 20 rule on checks
	RollResult roll = ResolveRoll(RollKind::Check, 11, plan.die, plan.Modifier(), 12, false);
	EXPECT_EQ(roll.total, 12);
	EXPECT_TRUE(roll.success); // the DC itself succeeds
	roll = ResolveRoll(RollKind::Check, 10, plan.die, plan.Modifier(), 12, false);
	EXPECT_FALSE(roll.success);
	roll = ResolveRoll(RollKind::Check, 1, plan.die, 20, 12, false);
	EXPECT_TRUE(roll.success); // a natural 1 is no fumble here
	// a Luck of 0 fails everything
	roll = ResolveRoll(RollKind::Check, 20, plan.die, plan.Modifier(), 12, true);
	EXPECT_FALSE(roll.success);
}

TEST(DCC_Skills, UntrainedCeiling)
{
	// funnel.md: an untrained d10 reaches DC 15 only with a high modifier or the +2
	SkillTable table = Table();
	SkillPlan untrained = PlanSkillCheck(table, Input(0, "SMITHING", 15, 3));
	EXPECT_EQ(SkillSuccesses(untrained, 15), 0); // 10 + 3 = 13
	SkillPlan familiar = PlanSkillCheck(table, Input(SKILL_MINING, "SMITHING", 15, 3));
	EXPECT_EQ(SkillSuccesses(familiar, 15), 1); // only a natural 10: 10 + 2 + 3
	SkillPlan trained = PlanSkillCheck(table, Input(SKILL_SMITHING, "SMITHING", 15, 0));
	EXPECT_EQ(SkillSuccesses(trained, 15), 6); // 15-20
	EXPECT_EQ(SkillSuccesses(trained, 1), 20);
	EXPECT_EQ(SkillSuccesses(trained, 15, -4), 2); // groggy: 19-20
}

TEST(DCC_Skills, ThiefSkillOverlap)
{
	SkillTable table = Table();
	EXPECT_EQ(SkillThiefOverlap(table, SKILL_LOCKS), static_cast<int>(ThiefSkill::PickLock));
	EXPECT_EQ(SkillThiefOverlap(table, SKILL_SMITHING | SKILL_CLIMB), static_cast<int>(ThiefSkill::Climb));
	EXPECT_EQ(SkillThiefOverlap(table, SKILL_SMITHING), -1);

	// a locksmith who isn't a thief: trained up to DC 10 (classes.md), d10 + 2 past it
	SkillPlan plan = PlanSkillCheck(table, Input(SKILL_LOCKS | SKILL_SMITHING, "LOCKS", 10));
	EXPECT_EQ(plan.training, SkillTraining::Trained);
	plan = PlanSkillCheck(table, Input(SKILL_LOCKS | SKILL_SMITHING, "LOCKS", 15));
	EXPECT_EQ(plan.training, SkillTraining::Familiar);
	EXPECT_TRUE(plan.capped);
	EXPECT_EQ(plan.familiarity, 2);
	// another wanted tag without a cap still trains it
	plan = PlanSkillCheck(table, Input(SKILL_LOCKS | SKILL_SMITHING, "LOCKS|SMITHING", 15));
	EXPECT_EQ(plan.training, SkillTraining::Trained);
	EXPECT_EQ(plan.matched, uint32_t(SKILL_SMITHING));

	// a thief of level 1+ rolls d20 + its Table 1-9 bonus there, trained by its occupation or not
	SkillPlanInput in = Input(SKILL_FARMING, "LOCKS", 15, 1);
	in.thief = true;
	in.thiefBonus = 3;
	plan = PlanSkillCheck(table, in);
	EXPECT_EQ(plan.training, SkillTraining::Thief);
	EXPECT_EQ(plan.die, 20);
	EXPECT_EQ(plan.thiefBonus, 3);
	EXPECT_EQ(plan.Modifier(), 4);
	// ... but not on a task that is no thief skill
	plan = PlanSkillCheck(table, Input(SKILL_FARMING, "SMITHING", 15));
	EXPECT_EQ(plan.training, SkillTraining::Untrained);
	// climbing has no cap: a dock worker climbs rigging trained at any DC
	plan = PlanSkillCheck(table, Input(SKILL_CLIMB, "CLIMB", 20));
	EXPECT_EQ(plan.training, SkillTraining::Trained);
}

TEST(DCC_Skills, BestPlacedRoller)
{
	SkillTable table = Table();
	SkillPlan trainedLow = PlanSkillCheck(table, Input(SKILL_SMITHING, "SMITHING", 12, -3)); // d20 - 3: 15-20, 6 of 20
	SkillPlan untrainedHigh = PlanSkillCheck(table, Input(0, "SMITHING", 12, 3)); // d10 + 3: 9-10, 2 of 10
	EXPECT_TRUE(BetterSkillPlan(trainedLow, untrainedHigh, 12));
	EXPECT_FALSE(BetterSkillPlan(untrainedHigh, trainedLow, 12));
	// an easy task: the strong untrained one has the better chance (6 of 10 against 10 of 20)
	EXPECT_TRUE(BetterSkillPlan(untrainedHigh, trainedLow, 8));
	// equal chances: the higher average total; the same plan twice is no better (party order wins)
	SkillPlan a = PlanSkillCheck(table, Input(SKILL_SMITHING, "SMITHING", 1, 0));
	SkillPlan b = PlanSkillCheck(table, Input(SKILL_SMITHING, "SMITHING", 1, 2));
	EXPECT_TRUE(BetterSkillPlan(b, a, 1)); // both always make DC 1
	EXPECT_FALSE(BetterSkillPlan(a, a, 12));
}

TEST(DCC_Skills, LuckBurnOnAFailedCheck)
{
	// a trained check missed by 2 (natural 9 + 1 vs DC 12): the "miss to hit" policy burns 2 points
	SkillTable table = Table();
	SkillPlan plan = PlanSkillCheck(table, Input(SKILL_SMITHING, "SMITHING", 12, 1));
	RollResult roll = ResolveRoll(RollKind::Check, 9, plan.die, plan.Modifier(), 12, false);
	ASSERT_FALSE(roll.success);
	int gap = roll.target - roll.total;
	EXPECT_EQ(gap, 2);
	LuckPolicy policy;
	policy.policy = BurnPolicy::MissToHit;
	policy.threshold = 8;
	BurnValue anyone = BurnValueFor(static_cast<int>(ClassID::Level0), 0, 0);
	int points = PolicyBurn(policy, BurnReason::Miss, gap, 12, anyone);
	EXPECT_EQ(points, 2);
	EXPECT_GE(roll.total + BurnBonus(anyone, points, [](int) { return 1; }), roll.target);
	// never below the threshold: Luck 9 with a threshold of 8 burns nothing (it would end at 7)
	EXPECT_EQ(PolicyBurn(policy, BurnReason::Miss, gap, 9, anyone), 0);
	// "avoid disaster" leaves an ordinary check alone, but saves a deadly one
	policy.policy = BurnPolicy::AvoidDisaster;
	EXPECT_EQ(PolicyBurn(policy, BurnReason::Miss, gap, 12, anyone), 0);
	EXPECT_EQ(PolicyBurn(policy, BurnReason::Death, gap, 12, anyone), 2);
	// a halfling of level 1+ gets +2 a point: one point covers it
	BurnValue halfling = BurnValueFor(static_cast<int>(ClassID::Halfling), 1, 0);
	policy.policy = BurnPolicy::MissToHit;
	EXPECT_EQ(PolicyBurn(policy, BurnReason::Miss, gap, 12, halfling), 1);
	// a burn can't reach past an untrained ceiling it can't cover: Luck 3, a gap of 5
	EXPECT_EQ(PolicyBurn(policy, BurnReason::Death, 5, 3, anyone), 0);
}

TEST(DCC_Skills, TableHelpers)
{
	SkillTable table = Table();
	EXPECT_FALSE(table.Empty());
	ASSERT_TRUE(table.Find(SKILL_LOCKS));
	EXPECT_EQ(table.Find(SKILL_LOCKS)->cap, 10);
	EXPECT_EQ(table.Find(SKILL_LOCKS | SKILL_SMITHING), nullptr); // one bit only
	EXPECT_EQ(table.Find(SKILL_COOKING), nullptr); // not in this table
	EXPECT_EQ(table.Familiar(SKILL_SMITHING | SKILL_FARMING), uint32_t(SKILL_MINING | SKILL_LOCKS | SKILL_MILITARY | SKILL_HERBALISM | SKILL_ANIMALS | SKILL_COOKING));
	EXPECT_EQ(table.DefaultAbility(SKILL_SMITHING), Ability::Str);
	EXPECT_EQ(table.DefaultAbility(SKILL_LOCKS | SKILL_SMITHING), Ability::Str); // the first tag's, in bit order
	EXPECT_EQ(table.DefaultAbility(0), Ability::Int);
	EXPECT_EQ(table.Labels(SKILL_FARMING | SKILL_SMITHING), "Farming, Smithing");
	EXPECT_STREQ(SkillTrainingName(SkillTraining::Familiar), "familiar");
}

}
}
