// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/DCC/DiceChain.h"

#include <gtest/gtest.h>

namespace GemRB {
namespace DCC {

// docs/rules/abilities.md, "Dice chain"
static const std::array<int, 13> chain { 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 30 };

TEST(DCC_DiceChain, Order)
{
	EXPECT_EQ(DiceChain::Dice, chain);
	EXPECT_EQ(DiceChain::Smallest(), 3);
	EXPECT_EQ(DiceChain::Largest(), 30);
}

TEST(DCC_DiceChain, IndexOf)
{
	for (int i = 0; i < int(chain.size()); ++i) {
		EXPECT_EQ(DiceChain::IndexOf(chain[i]), i) << "d" << chain[i];
		EXPECT_TRUE(DiceChain::IsOnChain(chain[i]));
		EXPECT_EQ(DiceChain::AtIndex(i), chain[i]);
	}
	for (int sides : { -4, 0, 1, 2, 9, 11, 13, 15, 18, 22, 100 }) {
		EXPECT_EQ(DiceChain::IndexOf(sides), DiceChain::npos) << "d" << sides;
		EXPECT_FALSE(DiceChain::IsOnChain(sides));
	}
	EXPECT_EQ(DiceChain::AtIndex(-1), 3);
	EXPECT_EQ(DiceChain::AtIndex(13), 30);
	EXPECT_EQ(DiceChain::AtIndex(1000), 30);
}

TEST(DCC_DiceChain, SingleSteps)
{
	for (size_t i = 0; i + 1 < chain.size(); ++i) {
		EXPECT_EQ(DiceChain::StepUp(chain[i]), chain[i + 1]);
		EXPECT_EQ(DiceChain::StepDown(chain[i + 1]), chain[i]);
	}
	// examples from docs/rules/combat.md
	EXPECT_EQ(DiceChain::StepDown(16), 14); // second action die with -1d
	EXPECT_EQ(DiceChain::StepDown(8), 7); // subdual longsword
	EXPECT_EQ(DiceChain::StepDown(20), 16); // two-handed initiative die is one step down
}

TEST(DCC_DiceChain, MultipleSteps)
{
	EXPECT_EQ(DiceChain::Shift(20, 0), 20);
	EXPECT_EQ(DiceChain::Shift(20, 2), 30);
	EXPECT_EQ(DiceChain::Shift(20, -2), 14);
	EXPECT_EQ(DiceChain::Shift(6, 3), 10);
	EXPECT_EQ(DiceChain::Shift(10, -4), 5); // d8 d7 d6 d5
	EXPECT_EQ(DiceChain::StepUp(4, 5), 10); // d5 d6 d7 d8 d10
	EXPECT_EQ(DiceChain::StepDown(24, 3), 14);
	// steps stack and cancel out
	EXPECT_EQ(DiceChain::Shift(DiceChain::Shift(8, 2), -2), 8);
	EXPECT_EQ(DiceChain::Shift(8, +1 - 1), 8);
	EXPECT_EQ(DiceChain::Shift(8, +2 - 1), 10);
}

TEST(DCC_DiceChain, Clamping)
{
	EXPECT_EQ(DiceChain::StepUp(30), 30);
	EXPECT_EQ(DiceChain::StepUp(24, 5), 30);
	EXPECT_EQ(DiceChain::StepUp(3, 100), 30);
	EXPECT_EQ(DiceChain::StepDown(3), 3);
	EXPECT_EQ(DiceChain::StepDown(5, 4), 3);
	EXPECT_EQ(DiceChain::StepDown(30, 100), 3);
	EXPECT_EQ(DiceChain::Shift(20, -12), 3);
	EXPECT_EQ(DiceChain::Shift(3, 12), 30);
}

TEST(DCC_DiceChain, OffChainDice)
{
	// between chain dice: the first step lands on the neighbour in that direction
	EXPECT_EQ(DiceChain::StepUp(9), 10);
	EXPECT_EQ(DiceChain::StepDown(9), 8);
	EXPECT_EQ(DiceChain::StepUp(9, 2), 12);
	EXPECT_EQ(DiceChain::StepDown(9, 2), 7);
	EXPECT_EQ(DiceChain::StepUp(18), 20);
	EXPECT_EQ(DiceChain::StepDown(18), 16);
	EXPECT_EQ(DiceChain::StepUp(11, 100), 30);
	EXPECT_EQ(DiceChain::StepDown(11, 100), 3);
	// past the ends: inwards only
	EXPECT_EQ(DiceChain::StepUp(2), 3);
	EXPECT_EQ(DiceChain::StepUp(2, 2), 4);
	EXPECT_EQ(DiceChain::StepDown(2), 2);
	EXPECT_EQ(DiceChain::StepDown(100), 30);
	EXPECT_EQ(DiceChain::StepDown(100, 2), 24);
	EXPECT_EQ(DiceChain::StepUp(100), 100);
	// no steps, no change
	EXPECT_EQ(DiceChain::Shift(9, 0), 9);
	EXPECT_EQ(DiceChain::Shift(100, 0), 100);
}

}
}
