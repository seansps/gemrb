// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file DiceChain.h
 * The DCC dice chain: d3 d4 d5 d6 d7 d8 d10 d12 d14 d16 d20 d24 d30.
 *
 * "+1d" moves a die one step up the chain and "-1d" one step down. Steps stack,
 * ups and downs cancel out and the ends clamp (nothing above d30 or below d3).
 * Flat modifiers apply to the roll of the new die, so they are not handled here.
 *
 * Dice that are not on the chain (d2, d9, d100) move to the neighbouring chain
 * die in the direction of the step: d9 +1d is d10 and d9 -1d is d8. A die past
 * either end only moves inwards: d100 -1d is d30 but d100 +1d stays d100, and
 * d2 +1d is d3 but d2 -1d stays d2.
 *
 * Pure functions, no game data: core->Roll(1, sides, 0) rolls any of these.
 */

#ifndef DCC_DICECHAIN_H
#define DCC_DICECHAIN_H

#include "exports.h"

#include <array>

namespace GemRB {
namespace DCC {

class GEM_EXPORT DiceChain {
public:
	static constexpr std::array<int, 13> Dice { 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 30 };
	static constexpr int npos = -1;

	/** Chain index of a die (0 = d3 ... 12 = d30), or npos if it is not on the chain */
	static int IndexOf(int sides);
	static bool IsOnChain(int sides) { return IndexOf(sides) != npos; }
	/** Die at a chain index, with the index clamped to the chain */
	static int AtIndex(int index);
	/** Shift a die by steps (positive = up, negative = down), clamped at d3 and d30 */
	static int Shift(int sides, int steps);
	static int StepUp(int sides, int steps = 1) { return Shift(sides, steps); }
	static int StepDown(int sides, int steps = 1) { return Shift(sides, -steps); }

	static constexpr int Smallest() { return Dice.front(); }
	static constexpr int Largest() { return Dice.back(); }
};

}
}

#endif
