// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Luck.h"

#include "DCC/Classes.h"

#include <algorithm>

namespace GemRB {
namespace DCC {

static bool IsClass(int classID, int level, ClassID which)
{
	return level > 0 && classID == static_cast<int>(which);
}

BurnValue BurnValueFor(int classID, int level, int luckDie)
{
	BurnValue value;
	if (IsClass(classID, level, ClassID::Thief) && luckDie > 0) {
		value.luckDie = luckDie;
	} else if (IsClass(classID, level, ClassID::Halfling)) {
		value.perPoint = 2;
	}
	return value;
}

int BurnBonus(const BurnValue& value, int points, const Roller& roller)
{
	if (points <= 0) return 0;
	if (!value.IsDie()) return points * value.perPoint;
	int total = 0;
	for (int i = 0; i < points; ++i) {
		total += roller(value.luckDie);
	}
	return total;
}

int PointsToCover(const BurnValue& value, int gap)
{
	if (gap <= 0) return 0;
	if (!value.IsDie()) {
		int per = std::max(1, value.perPoint);
		return (gap + per - 1) / per;
	}
	// the die's average is (die + 1) / 2: points * (die + 1) >= 2 * gap
	int twice = value.luckDie + 1;
	return (2 * gap + twice - 1) / twice;
}

int MaxBurnBonus(const BurnValue& value, int points)
{
	if (points <= 0) return 0;
	return points * (value.IsDie() ? value.luckDie : value.perPoint);
}

bool CanCancelFumble(int classID, int level)
{
	return IsClass(classID, level, ClassID::Warrior) || IsClass(classID, level, ClassID::Dwarf);
}

int PolicyBurn(const LuckPolicy& policy, BurnReason reason, int gap, int luck, const BurnValue& value)
{
	if (policy.policy == BurnPolicy::Never || luck <= 1) return 0;
	// never down to 0 on its own: that is cursed by fortune
	int spendable = luck - 1;
	switch (reason) {
		case BurnReason::Fumble:
			return spendable >= FUMBLE_CANCEL_COST ? FUMBLE_CANCEL_COST : 0;
		case BurnReason::Death:
			{
				int points = PointsToCover(value, gap);
				return points > 0 && points <= spendable ? points : 0;
			}
		case BurnReason::Miss:
			{
				if (policy.policy != BurnPolicy::MissToHit) return 0;
				int points = PointsToCover(value, gap);
				spendable = std::min(spendable, luck - std::max(policy.threshold, 1));
				return points > 0 && points <= spendable ? points : 0;
			}
		default:
			return 0;
	}
}

bool IsKeyRoll(BurnReason reason, bool wouldCrit)
{
	return reason == BurnReason::Death || reason == BurnReason::Fumble || wouldCrit;
}

int NightlyLuckRecovery(int classID, int level)
{
	if (IsClass(classID, level, ClassID::Thief) || IsClass(classID, level, ClassID::Halfling)) return level;
	return 0;
}

int LuckLossAfterNight(int classID, int level, int loss)
{
	if (loss <= 0) return 0;
	return std::max(0, loss - NightlyLuckRecovery(classID, level));
}

}
}
