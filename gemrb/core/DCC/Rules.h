// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Rules.h
 * The DCC tables a game with DCCRules loads at startup (core->GetDCCRules()),
 * plus the mapping from the five IE saving throw types to Fort/Ref/Will.
 *
 * dccsave.2da has rows SPELL, BREATH, DEATH, WAND, POLYMORPH and DEFAULT
 * and a SAVE column (FORT, REF or WILL). Stock IE effects name 2e save types,
 * so this decides which DCC save they roll.
 */

#ifndef DCC_RULES_H
#define DCC_RULES_H

#include "exports.h"

#include "DCC/Augur.h"
#include "DCC/Character.h"
#include "DCC/Classes.h"

#include <array>

namespace GemRB {

class TableMgr;

namespace DCC {

// the IE save types in the order of Effect::SavingThrowType bits and Actor::GetSavingThrow
enum class IESave : uint8_t {
	Spell,
	Breath,
	Death,
	Wand,
	Polymorph,
	count
};
constexpr int IE_SAVE_COUNT = static_cast<int>(IESave::count);

class GEM_EXPORT SaveMap {
public:
	SaveMap();
	/** reads a 2DA with SPELL..POLYMORPH (and optional DEFAULT) rows and a SAVE column;
	 * missing rows take the DEFAULT row, or Will */
	bool Load(const TableMgr& table);
	void Set(IESave type, Save save) { map[static_cast<size_t>(type)] = save; }
	Save Get(IESave type) const;
	Save Get(unsigned int ieType) const;
	Save Default() const { return defaultSave; }

	/** FORT, REF, WILL; false for anything else */
	static bool ParseSave(const std::string& key, Save& save);
	static const char* SaveKey(Save save);

private:
	std::array<Save, IE_SAVE_COUNT> map;
	Save defaultSave = Save::Will;
};

class GEM_EXPORT RulesData {
public:
	XPTable xp;
	ClassTable classes;
	AugurTable augurs;
	SaveMap saves;
	ThiefSkillTable thiefSkills;

	/** loads every table through gamedata; logs and returns false if one is missing or bad */
	bool Load();
};

}
}

#endif
