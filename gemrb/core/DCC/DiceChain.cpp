// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/DiceChain.h"

#include "globals.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

constexpr std::array<int, 13> DiceChain::Dice;
constexpr int DiceChain::npos;

static constexpr int lastIndex = static_cast<int>(DiceChain::Dice.size()) - 1;

int DiceChain::IndexOf(int sides)
{
	auto it = std::find(Dice.begin(), Dice.end(), sides);
	if (it == Dice.end()) return npos;
	return static_cast<int>(it - Dice.begin());
}

int DiceChain::AtIndex(int index)
{
	return Dice[Clamp(index, 0, lastIndex)];
}

int DiceChain::Shift(int sides, int steps)
{
	if (steps == 0) return sides;

	int index = IndexOf(sides);
	if (index != npos) {
		return AtIndex(index + steps);
	}

	// off the chain: the first step lands on the neighbouring chain die
	// in that direction; past an end there is none outwards, so stay put
	if (steps > 0) {
		auto above = std::upper_bound(Dice.begin(), Dice.end(), sides);
		if (above == Dice.end()) return sides;
		return AtIndex(static_cast<int>(above - Dice.begin()) + steps - 1);
	}

	// lower_bound is the first chain die above, so one below it is the first step down
	auto above = std::lower_bound(Dice.begin(), Dice.end(), sides);
	if (above == Dice.begin()) return sides;
	return AtIndex(static_cast<int>(above - Dice.begin()) + steps);
}

}
}
