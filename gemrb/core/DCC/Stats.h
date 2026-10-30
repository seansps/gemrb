// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Stats.h
 * Where the DCC abilities and saves live among the actor stats (ie_stats.h).
 */

#ifndef DCC_STATS_H
#define DCC_STATS_H

#include "ie_stats.h"

#include "DCC/Character.h"

namespace GemRB {
namespace DCC {

/** the stat holding an ability's score: Str IE_STR, Agi IE_DEX, Sta IE_CON, Per IE_CHR, Int IE_INT, Luck IE_DCC_LUCK */
inline unsigned int AbilityStat(Ability ability)
{
	switch (ability) {
		case Ability::Str:
			return IE_STR;
		case Ability::Agi:
			return IE_DEX;
		case Ability::Sta:
			return IE_CON;
		case Ability::Per:
			return IE_CHR;
		case Ability::Int:
			return IE_INT;
		default:
			return IE_DCC_LUCK;
	}
}

/** the stat holding an ability's loss */
inline unsigned int LossStat(Ability ability)
{
	return IE_DCC_LOSS_STR + static_cast<unsigned int>(ability);
}

/** the stat holding a save total */
inline unsigned int SaveStat(Save save)
{
	return IE_DCC_FORT + static_cast<unsigned int>(save);
}

/** the stock IE save stat whose effect bonuses count for a DCC save: Fort death, Ref breath, Will spell */
inline unsigned int SaveBonusStat(Save save)
{
	switch (save) {
		case Save::Fort:
			return IE_SAVEVSDEATH;
		case Save::Ref:
			return IE_SAVEVSBREATH;
		default:
			return IE_SAVEVSSPELL;
	}
}

}
}

#endif
