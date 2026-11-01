// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Skills.h
 * Occupation skill checks (M3.4) as pure functions: DCC has no skill list, a check is trained (d20)
 * when the character's 0-level occupation covers the task and untrained (d10) otherwise
 * (docs/rules/funnel.md, "Skills (Chapter 2)"). Ethren turns "does the background cover it?" into
 * data (funnel.md, "Ethren design proposal: occupation skills"): each occupation carries skill tags
 * (the OCCU record's bits, Extension.h), a check names the tags it wants, an ability and a DC.
 *
 * - dccskill.2da: per tag its default ability, the adjacent tags that give some familiarity (d10 + 2),
 *   the Table 1-9 thief skill it overlaps and the highest DC occupation training reaches there.
 * - dccoccup.2da (Table 1-3): an occupation's name and tags, for the log and characters whose
 *   OCCU record has no tags.
 *
 * The engine glue (rolling with the actor's dice, Luck, the log, scripts) is in
 * Scriptable/ActorDCCSkills.cpp; see docs/architecture/rules-dcc.md, "Occupation skills (M3.4)".
 */

#ifndef DCC_SKILLS_H
#define DCC_SKILLS_H

#include "exports.h"

#include "DCC/Character.h"
#include "DCC/Classes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// what a check asks for: tags (OccupationSkill bits) and the two special tags (funnel.md)
struct SkillTags {
	uint32_t bits = 0;
	bool anyone = false; // ANYONE: a task any adult could try, trained for everyone
	bool untrainable = false; // UNTRAINABLE: nobody is trained, everyone rolls d10
	bool valid = false; // false: empty or an unknown name
};
/** "SMITHING|MINING" (any case, | or , or spaces between them; ANYONE, UNTRAINABLE) */
GEM_EXPORT SkillTags ParseSkillTags(const std::string& text);
/** the tags as the scripts write them, "SMITHING|MINING" (the special tags last) */
GEM_EXPORT std::string SkillTagsKey(const SkillTags& tags);
/** the number of tags set in bits */
GEM_EXPORT int SkillTagCount(uint32_t bits);

// how a check is rolled
enum class SkillTraining : uint8_t {
	Untrained, // d10
	Familiar, // d10 + 2: an adjacent tag (the book's "miller knows a little about seeds"), or past the occupation's DC cap
	Trained, // d20: the occupation (or an acquired tag) covers it
	Anyone, // d20: any adult could do it
	Thief, // d20 + the Table 1-9 bonus of the thief skill the tag overlaps (a thief of level 1+)
	count
};
GEM_EXPORT const char* SkillTrainingName(SkillTraining training); // "untrained", "familiar", "trained", "anyone", "thief"
constexpr int SKILL_FAMILIAR_BONUS = 2;
constexpr int SKILL_TRAINED_DIE = 20;
constexpr int SKILL_UNTRAINED_DIE = 10;
// the highest DC an occupation's training reaches in a thief's skill (classes.md: a locksmith who
// isn't a thief picks simple locks trained, "only up to DC 10"); dccskill.2da's CAP per tag
constexpr int OCCUPATION_THIEF_CAP = 10;
// acquired tags (story events: an apprenticeship, years of downtime) a character may have beyond its occupation's
constexpr int ACQUIRED_SKILL_LIMIT = 2;

// how ability is given to a check (dccabil.ids): the tag's own (dccskill.2da), or an ability
enum SkillAbilityCode : int {
	SKILL_ABILITY_DEFAULT = 0,
	SKILL_ABILITY_STR = 1, // .. 6 Luck: DCC::Ability + 1
	SKILL_ABILITY_LUCK = 6
};

// a check's options (dccskfl.ids)
enum SkillCheckFlags : uint32_t {
	SKILL_NONE = 0,
	SKILL_DEADLY = 1, // failure is a disaster: a key roll (the owner may be asked to burn Luck), and an "avoid disaster" policy burns too
	SKILL_NO_LUCK = 2, // Luck can't help (like a Luck check)
	SKILL_PARTY = 0x100 // (the engine's own) the best placed of the party rolls for everyone
};

// what a script action's variable holds afterwards
enum SkillCheckOutcome : int {
	SKILL_OUTCOME_NONE = 0,
	SKILL_OUTCOME_PASSED = 1,
	SKILL_OUTCOME_FAILED = 2,
	SKILL_OUTCOME_WAITING = 3 // a Luck prompt is open; 1 or 2 follows with the answer
};

struct SkillInfo {
	uint32_t bit = 0;
	Ability ability = Ability::Int; // the default ability (funnel.md, "Default abilities for common dungeon actions")
	uint32_t familiar = 0; // tags that give some familiarity (d10 + 2)
	int thief = -1; // the ThiefSkill it overlaps, -1 none
	int cap = 0; // the highest DC occupation training reaches (0: no limit)
	std::string label; // "Craft (fine)"
	std::string uses; // "jewelry, glass, clothing, leather"
};

class GEM_EXPORT SkillTable {
public:
	/** dccskill.2da: rows by tag name (all 30, OccupationSkill's names), columns ABILITY (STR AGI STA
	 * PER INT LUCK), FAMILIAR (tags, | between, * none), THIEF (a Table 1-9 column, * none), CAP (* none),
	 * NAME and USES (_ for spaces); false for an unknown tag, ability or skill, or a tag missing */
	bool Load(const TableMgr& table);
	void Set(const SkillInfo& info);
	/** the tag's row, nullptr for an unknown bit (exactly one bit) */
	const SkillInfo* Find(uint32_t bit) const;
	/** the tags that give some familiarity with any of these */
	uint32_t Familiar(uint32_t bits) const;
	/** the first tag's default ability (Int when there is none) */
	Ability DefaultAbility(uint32_t bits) const;
	/** "Smithing, Appraisal" */
	std::string Labels(uint32_t bits) const;
	bool Empty() const;

private:
	std::array<SkillInfo, 32> rows {};
};

// an occupation of Table 1-3 (dccoccup.2da): id is the row's lowest d100 number, as OCCU
struct OccupationInfo {
	int id = 0;
	int race = 1;
	uint32_t skills = 0;
	std::string name; // "Wizard's apprentice"
};

class GEM_EXPORT OccupationTable {
public:
	/** dccoccup.2da: rows by id, columns RACE, SKILLS (tags, | between), NAME (_ for spaces); false for an
	 * unknown tag or an empty table */
	bool Load(const TableMgr& table);
	void Set(const OccupationInfo& info);
	const OccupationInfo* Find(int id) const;
	bool Empty() const { return list.empty(); }

private:
	std::vector<OccupationInfo> list;
};

// what a check is rolled with, before the die
struct SkillPlanInput {
	uint32_t known = 0; // the character's tags (OCCU)
	SkillTags wanted;
	int dc = 10;
	int abilityMod = 0; // the check's ability
	int augur = 0; // born under the loom: the frozen Luck modifier on skill checks
	int situational = 0;
	bool thief = false; // a thief of level 1+
	int thiefBonus = 0; // its Table 1-9 bonus for the overlapped skill (thiefSkill)
	int thiefSkill = -1; // the ThiefSkill the wanted tags overlap, -1 none (SkillTable::Plan fills it)
};

struct SkillPlan {
	SkillTraining training = SkillTraining::Untrained;
	int die = SKILL_UNTRAINED_DIE;
	int familiarity = 0; // +2
	int thiefBonus = 0; // a thief's Table 1-9 bonus
	int abilityMod = 0;
	int augur = 0;
	int situational = 0;
	uint32_t matched = 0; // the tags that trained it (or gave the familiarity)
	bool capped = false; // trained by the occupation, but past its DC cap in a thief's skill
	int Modifier() const { return familiarity + thiefBonus + abilityMod + augur + situational; }
};

/** how a character with known tags rolls a check (funnel.md, "Resolution rule"): ANYONE trains
 * everyone, UNTRAINABLE nobody; a wanted tag it has is trained, a familiar one d10 + 2. A thief of
 * level 1+ rolls d20 plus its Table 1-9 bonus where a tag overlaps a thief skill; anyone else's
 * occupation training stops at the tag's cap there (d10 + 2 past it). */
GEM_EXPORT SkillPlan PlanSkillCheck(const SkillTable& table, const SkillPlanInput& in);
/** the ThiefSkill a set of tags overlaps (the first tag's that does), -1 none */
GEM_EXPORT int SkillThiefOverlap(const SkillTable& table, uint32_t bits);

// a check as rolled, for the log, scripts and Python (Actor::DCCSkillCheck)
struct SkillCheckResult {
	uint32_t roller = 0; // the global ID of who rolled, 0 nobody (a party check nobody could try)
	std::string tags; // SkillTagsKey
	Ability ability = Ability::Int;
	SkillPlan plan;
	int dc = 0;
	int natural = 0;
	int rollMod = 0; // IE_DCC_ROLLMOD (groggy after a body was rolled over: -4)
	int total = 0;
	bool success = false;
	bool cursed = false; // a Luck of 0 fails every roll
	int luckBurned = 0;
	int luckBonus = 0;
	bool waiting = false; // a Luck prompt is open; the result comes with the answer
	bool party = false; // the best placed of the party rolled
};

/** how many naturals of the plan's die make dc (with rollMod, eg. groggy's -4), 0 to die */
GEM_EXPORT int SkillSuccesses(const SkillPlan& plan, int dc, int rollMod = 0);
/** true if a's chance of making dc beats b's: more successes per side, then the higher average
 * total; equal is false (the earlier in party order keeps it) */
GEM_EXPORT bool BetterSkillPlan(const SkillPlan& a, const SkillPlan& b, int dc);

}
}

#endif
