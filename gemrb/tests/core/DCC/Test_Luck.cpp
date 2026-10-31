// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Burning Luck: what a point is worth by class, the burn policy as a pure decision
// (roll, target, policy, Luck -> points), key rolls, and the thief's and halfling's
// nightly recovery. docs/rules/luck.md, combat.md "Luck in combat", classes.md,
// open-questions.md (the RTwP policy)

#include "../../../core/DCC/Classes.h"
#include "../../../core/DCC/Luck.h"

#include <gtest/gtest.h>

#include <vector>

namespace GemRB {
namespace DCC {

constexpr int WARRIOR = static_cast<int>(ClassID::Warrior);
constexpr int THIEF = static_cast<int>(ClassID::Thief);
constexpr int DWARF = static_cast<int>(ClassID::Dwarf);
constexpr int HALFLING = static_cast<int>(ClassID::Halfling);
constexpr int WIZARD = static_cast<int>(ClassID::Wizard);
constexpr int LEVEL0 = static_cast<int>(ClassID::Level0);

static LuckPolicy Policy(BurnPolicy which, int threshold = 10)
{
	LuckPolicy policy;
	policy.policy = which;
	policy.threshold = threshold;
	return policy;
}

TEST(DCC_Luck, BurnValueByClass)
{
	// anyone: +1 a point
	BurnValue value = BurnValueFor(WARRIOR, 1, 0);
	EXPECT_EQ(value.perPoint, 1);
	EXPECT_FALSE(value.IsDie());
	EXPECT_EQ(BurnValueFor(WIZARD, 5, 0).perPoint, 1);
	EXPECT_EQ(BurnValueFor(LEVEL0, 0, 0).perPoint, 1);
	// a thief rolls its luck die per point (d3 at level 1, d4 at level 2: Table 1-7)
	value = BurnValueFor(THIEF, 2, 4);
	EXPECT_TRUE(value.IsDie());
	EXPECT_EQ(value.luckDie, 4);
	// halflings get +2 a point
	EXPECT_EQ(BurnValueFor(HALFLING, 1, 0).perPoint, 2);
	EXPECT_FALSE(BurnValueFor(HALFLING, 1, 0).IsDie());
	// at level 0 they are still peasants: +1
	EXPECT_EQ(BurnValueFor(HALFLING, 0, 0).perPoint, 1);
	EXPECT_FALSE(BurnValueFor(THIEF, 0, 3).IsDie());
}

TEST(DCC_Luck, BurnBonus)
{
	EXPECT_EQ(BurnBonus(BurnValueFor(WARRIOR, 1, 0), 6, nullptr), 6); // burn 6: +6
	EXPECT_EQ(BurnBonus(BurnValueFor(HALFLING, 3, 0), 3, nullptr), 6);
	EXPECT_EQ(BurnBonus(BurnValueFor(WARRIOR, 1, 0), 0, nullptr), 0);
	// the book's example: a level 2 thief burning 2 adds 2d4
	std::vector<int> sides;
	Roller roller = [&sides](int die) {
		sides.push_back(die);
		return die == 4 ? 3 : 0;
	};
	EXPECT_EQ(BurnBonus(BurnValueFor(THIEF, 2, 4), 2, roller), 6);
	EXPECT_EQ(sides, (std::vector<int> { 4, 4 }));
}

TEST(DCC_Luck, PointsToCover)
{
	EXPECT_EQ(PointsToCover(BurnValueFor(WARRIOR, 1, 0), 3), 3);
	EXPECT_EQ(PointsToCover(BurnValueFor(HALFLING, 1, 0), 3), 2); // +4 covers 3
	EXPECT_EQ(PointsToCover(BurnValueFor(HALFLING, 1, 0), 4), 2);
	EXPECT_EQ(PointsToCover(BurnValueFor(WARRIOR, 1, 0), 0), 0);
	// a thief by the die's average: d3 averages 2, so 2 needs 1 die and 3 needs 2
	EXPECT_EQ(PointsToCover(BurnValueFor(THIEF, 1, 3), 2), 1);
	EXPECT_EQ(PointsToCover(BurnValueFor(THIEF, 1, 3), 3), 2);
	// d4 averages 2.5: 5 needs 2
	EXPECT_EQ(PointsToCover(BurnValueFor(THIEF, 2, 4), 5), 2);
	EXPECT_EQ(MaxBurnBonus(BurnValueFor(THIEF, 2, 4), 3), 12);
	EXPECT_EQ(MaxBurnBonus(BurnValueFor(HALFLING, 2, 0), 3), 6);
}

TEST(DCC_Luck, PolicyNeverBurns)
{
	LuckPolicy never = Policy(BurnPolicy::Never);
	BurnValue plus1 = BurnValueFor(WARRIOR, 1, 0);
	EXPECT_EQ(PolicyBurn(never, BurnReason::Miss, 1, 18, plus1), 0);
	EXPECT_EQ(PolicyBurn(never, BurnReason::Death, 1, 18, plus1), 0);
	EXPECT_EQ(PolicyBurn(never, BurnReason::Fumble, 0, 18, plus1), 0);
}

TEST(DCC_Luck, PolicyAvoidDisaster)
{
	LuckPolicy avoid = Policy(BurnPolicy::AvoidDisaster);
	BurnValue plus1 = BurnValueFor(WARRIOR, 1, 0);
	// a plain miss: no
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Miss, 2, 18, plus1), 0);
	// a save vs death failed by 3: burn 3
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Death, 3, 12, plus1), 3);
	// only what keeps 1 Luck: failing by 12 with Luck 12 can't be bought
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Death, 12, 12, plus1), 0);
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Death, 11, 12, plus1), 11);
	// a fumble: 1 point
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Fumble, 0, 12, plus1), 1);
	// never into cursed by fortune: Luck 1 doesn't burn, 0 can't
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Fumble, 0, 1, plus1), 0);
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Death, 1, 0, plus1), 0);
	// a halfling covers 3 with 2 points
	EXPECT_EQ(PolicyBurn(avoid, BurnReason::Death, 3, 12, BurnValueFor(HALFLING, 1, 0)), 2);
}

TEST(DCC_Luck, PolicyMissToHit)
{
	LuckPolicy miss = Policy(BurnPolicy::MissToHit, 10);
	BurnValue plus1 = BurnValueFor(WARRIOR, 1, 0);
	// missed by 2 with Luck 14: burn 2, Luck stays at 12
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Miss, 2, 14, plus1), 2);
	// Luck may drop to the threshold, not below it
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Miss, 4, 14, plus1), 4);
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Miss, 5, 14, plus1), 0);
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Miss, 1, 10, plus1), 0);
	// threshold 0 still keeps 1 Luck
	EXPECT_EQ(PolicyBurn(Policy(BurnPolicy::MissToHit, 0), BurnReason::Miss, 13, 14, plus1), 13);
	EXPECT_EQ(PolicyBurn(Policy(BurnPolicy::MissToHit, 0), BurnReason::Miss, 14, 14, plus1), 0);
	// it covers death and fumbles too, below the threshold (down to 1)
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Death, 6, 8, plus1), 6);
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Fumble, 0, 5, plus1), 1);
	// a thief by the luck die's average: missed by 3 with a d3 is 2 points
	EXPECT_EQ(PolicyBurn(miss, BurnReason::Miss, 3, 15, BurnValueFor(THIEF, 1, 3)), 2);
	// a halfling for an ally: +2 a point
	EXPECT_EQ(PolicyBurn(Policy(BurnPolicy::MissToHit, 5), BurnReason::Miss, 2, 12, BurnValueFor(HALFLING, 1, 0)), 1);
}

TEST(DCC_Luck, FumbleCancelOnlyWarriorsAndDwarves)
{
	EXPECT_TRUE(CanCancelFumble(WARRIOR, 1));
	EXPECT_TRUE(CanCancelFumble(DWARF, 3));
	EXPECT_FALSE(CanCancelFumble(WARRIOR, 0));
	EXPECT_FALSE(CanCancelFumble(THIEF, 2));
	EXPECT_FALSE(CanCancelFumble(HALFLING, 2));
	EXPECT_FALSE(CanCancelFumble(LEVEL0, 0));
	EXPECT_EQ(FUMBLE_CANCEL_COST, 1);
}

TEST(DCC_Luck, KeyRolls)
{
	EXPECT_TRUE(IsKeyRoll(BurnReason::Death, false));
	EXPECT_TRUE(IsKeyRoll(BurnReason::Fumble, false));
	EXPECT_TRUE(IsKeyRoll(BurnReason::Miss, true)); // a burn would make it a crit
	EXPECT_FALSE(IsKeyRoll(BurnReason::Miss, false));
}

TEST(DCC_Luck, NightlyRecovery)
{
	// thieves and halflings get their level back each night
	EXPECT_EQ(NightlyLuckRecovery(THIEF, 1), 1);
	EXPECT_EQ(NightlyLuckRecovery(HALFLING, 3), 3);
	EXPECT_EQ(NightlyLuckRecovery(WARRIOR, 3), 0);
	EXPECT_EQ(NightlyLuckRecovery(THIEF, 0), 0);
	// the book's thief: Luck 11, burns 2 -> 9, then 10, then 11, never more
	EXPECT_EQ(LuckLossAfterNight(THIEF, 1, 2), 1);
	EXPECT_EQ(LuckLossAfterNight(THIEF, 1, 1), 0);
	EXPECT_EQ(LuckLossAfterNight(THIEF, 1, 0), 0);
	EXPECT_EQ(LuckLossAfterNight(HALFLING, 4, 3), 0);
	// anyone else keeps the loss
	EXPECT_EQ(LuckLossAfterNight(WARRIOR, 5, 3), 3);
	EXPECT_EQ(LuckLossAfterNight(WIZARD, 1, 7), 7);
}

TEST(DCC_Luck, Defaults)
{
	LuckPolicy policy;
	EXPECT_EQ(policy.policy, BurnPolicy::AvoidDisaster);
	EXPECT_TRUE(policy.flags & LUCK_PAUSE);
	EXPECT_TRUE(policy.flags & LUCK_ALLIES);
}

}
}
