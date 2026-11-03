// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Thief.h
 * Thief skills in play and traps (DCC rules) as pure functions: how a character rolls a thief skill
 * (d20 + the Table 1-9 bonus + the skill's ability; a halfling's sneak & hide; anyone else's d20 +
 * the ability on traps), what a missed disarm or pick pocket does (a natural 1 or a miss by 5 or more
 * springs the trap or gets the thief caught), the DCs of hide in shadows (the light) and sneak
 * silently (the floor), a pick pocket's DC, the trap data table and who is aware of a thief
 * (backstab). docs/rules/traps.md and classes.md ("Ethren design: thief skills in real time",
 * "Backstab, revised", "Hiding again in a fight").
 *
 * - dcctrap.2da: a trap's kind, disarm ability, deadly flag, XP and notice line, by area and object.
 * - dccstlth.2da: the light bands of hide in shadows (the lightmap's 0-100 level → DC).
 * - dccarea.2da (FLOOR) and dccfloor.2da: the floor DC of sneak silently, by area and by region.
 * - dcctools.2da: the items that count as thieves' tools.
 *
 * The engine glue is in Scriptable/ActorDCCThief.cpp; see docs/architecture/rules-dcc.md,
 * "Traps and thief skills".
 */

#ifndef DCC_THIEF_H
#define DCC_THIEF_H

#include "exports.h"

#include "DCC/Character.h"
#include "DCC/Classes.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

// traps.md, "Trap kinds": the flavour, the dwarves' bonus and which bypasses work
enum class TrapKind : uint8_t {
	Woodland,
	Earthwork,
	Stonework,
	Woodwork,
	Mechanism,
	Camp,
	Ward,
	count
};
GEM_EXPORT const char* TrapKindName(TrapKind kind); // WOODLAND .. WARD
GEM_EXPORT bool ParseTrapKind(const std::string& name, TrapKind& kind);
/** dwarves of level 1+ add their level to find these underground (traps.md) */
GEM_EXPORT bool DwarfKnowsTrap(TrapKind kind);

// a trap's or a lock's DC of this or more can't be found, disarmed or picked by a roll (traps.md, item 6)
constexpr int DC_UNROLLABLE = 100;
// a disarm or a pick pocket that misses by this much (or rolls a natural 1) springs the trap or is caught
constexpr int BOTCH_MARGIN = 5;
// a searcher rolls for the hidden traps whose outline is this near (traps.md, "Finding")
constexpr int TRAP_SEARCH_FEET = 20;
// a trap's notice line floats over the first party member this near
constexpr int TRAP_NOTICE_FEET = 15;
// party slots a trap remembers (the funnel's sixteen)
constexpr int TRAP_SLOTS = 16;

/** the bit of a party slot (InParty 1..16), 0 for anyone else */
GEM_EXPORT uint16_t TrapSlotBit(int inParty);

struct TrapInfo {
	std::string area; // upper case resref
	std::string object; // the region's, door's or container's script name, lower case
	TrapKind kind = TrapKind::Mechanism;
	bool disarmable = true; // false: only a word, a token or a spell (wards; DISARM NONE)
	Ability disarm = Ability::Agi; // Agi by default; Str to jam; Int to work out a mechanism
	bool deadly = false; // its disarm (and save) are key rolls
	int xp = 1; // to a thief of level 1+ who disarms it
	std::string notice; // floats once over the first party member within 15'
};

class GEM_EXPORT TrapTable {
public:
	/** dcctrap.2da: any row names, columns AREA, OBJECT, KIND, DISARM (AGI STR INT NONE), DEADLY (0 1), XP and
	 * NOTICE (_ for spaces, * none); false for an unknown kind or ability */
	bool Load(const TableMgr& table);
	void Set(const TrapInfo& info);
	/** the row of an area's object, nullptr if it has none (the defaults then: TrapInfo's) */
	const TrapInfo* Find(const std::string& area, const std::string& object) const;
	bool Empty() const { return rows.empty(); }

private:
	std::map<std::string, TrapInfo> rows; // "AREA/object"
};

// who rolls a thief skill, and with what
enum class ThiefRoller : uint8_t {
	Thief, // d20 + the Table 1-9 bonus (a thief of level 1+, with tools where the skill needs them)
	Halfling, // d20 + the halfling's sneak & hide bonus (level 1+)
	Anyone, // d20 + the ability: traps for everyone else (traps.md, "Who rolls what")
	Trained, // the occupation rules (funnel.md): d20
	Familiar, // d10 + 2
	Untrained, // d10
	count
};
GEM_EXPORT const char* ThiefRollerName(ThiefRoller roller); // for the log: "thief", "halfling", "anyone", "trained d20", ...

/** whether a skill needs thieves' tools for the thief's bonus (classes.md: pick lock, find and disable trap,
 * climb, forge document, handle poison) */
GEM_EXPORT bool SkillNeedsTools(ThiefSkill skill);
/** whether the armour check penalty applies to a check with this ability (physical: Str and Agi) */
GEM_EXPORT bool PhysicalAbility(Ability ability);

struct ThiefCheckInput {
	ThiefSkill skill = ThiefSkill::FindTrap;
	bool thief = false; // a thief of level 1+
	bool halfling = false; // a halfling of level 1+
	bool tools = false; // has thieves' tools
	int tableBonus = 0; // the thief's Table 1-9 bonus, or the halfling's sneak & hide
	Ability ability = Ability::Agi; // the check's ability (the skill's, or the trap's disarm ability)
	int abilityMod = 0;
	int augur = 0; // born under the loom, fox's cunning
	int armor = 0; // the armour check penalty (0 or less), for Str and Agi checks
	int dwarf = 0; // a dwarf's level on stone and earth underground (find trap)
	int situational = 0;
};

struct ThiefPlan {
	ThiefRoller roller = ThiefRoller::Anyone;
	int die = 20;
	Ability ability = Ability::Agi;
	int skill = 0; // the Table 1-9 or halfling bonus
	int familiarity = 0; // +2 (the occupation's familiar case)
	int abilityMod = 0;
	int augur = 0;
	int armor = 0;
	int dwarf = 0;
	int situational = 0;
	int Modifier() const { return skill + familiarity + abilityMod + augur + armor + dwarf + situational; }
};

/** a thief of level 1+ (with tools where needed) or a halfling on sneak and hide rolls d20 + its bonus;
 * anyone else d20 + the ability (the caller turns non-trap skills into occupation plans) */
GEM_EXPORT ThiefPlan PlanThiefCheck(const ThiefCheckInput& in);
/** whether a character rolls the skill with its class bonus (else as anyone, or by the occupation rules) */
GEM_EXPORT bool HasThiefBonus(const ThiefCheckInput& in);

// a disarm and a pick pocket have three results (traps.md, "Disarming"; classes.md, "Pick pocket")
enum class CheckOutcome : uint8_t {
	Success,
	Miss, // by 1-4: nothing happens (still armed; nothing taken, unnoticed)
	Botch // a natural 1, or a miss by 5 or more: it springs; the thief is caught
};
GEM_EXPORT CheckOutcome BotchOutcome(int natural, int total, int dc);

// a thief skill check's options, beside the occupation checks' (SkillCheckFlags: DEADLY 1, NO_LUCK 2)
enum ThiefCheckFlags : uint32_t {
	THIEF_SECRET = 0x200, // a failure logs nothing (the find roll: it mustn't say a trap is there)
	THIEF_NO_FLOAT = 0x400 // no floating line over the roller
};

// a thief skill check as rolled (Actor::DCCThiefCheck), for the log, scripts and Python
struct ThiefCheckResult {
	uint32_t roller = 0;
	ThiefSkill skill = ThiefSkill::FindTrap;
	ThiefPlan plan;
	int dc = 0;
	std::string dcLabel; // "dim", "stone"
	int natural = 0;
	int rollMod = 0; // groggy's -4
	int total = 0;
	bool success = false;
	CheckOutcome outcome = CheckOutcome::Miss;
	bool cursed = false;
	int luckBurned = 0;
	int luckBonus = 0;
	bool waiting = false; // a deadly disarm's Luck prompt is open
};

// hide in shadows by the light where the thief stands (classes.md, "Skill effects and DCs")
struct LightBand {
	int maxLight = 100; // the lightmap's level, 0-100 (Map::GetLightLevel: day/night tint included)
	int dc = 20;
	std::string label; // "dim"
};

class GEM_EXPORT StealthTable {
public:
	StealthTable(); // the default bands (dccstlth.2da has the same)
	/** dccstlth.2da: rows in order of brightness, columns LIGHT (the band's highest level), DC, LABEL */
	bool Load(const TableMgr& table);
	void SetBands(std::vector<LightBand> list);
	/** the band of a light level (the last band for anything brighter) */
	const LightBand& Band(int light) const;

private:
	std::vector<LightBand> bands;
};

// sneak silently by the floor: 5 cushioned, 10 stone, 15 creaky wood, 20 very noisy
constexpr int FLOOR_DC_DEFAULT = 10;
GEM_EXPORT std::string FloorLabel(int dc); // "cushioned", "stone", "creaky wood", "very noisy", else "DC n"

class GEM_EXPORT FloorTable {
public:
	/** dccarea.2da's FLOOR column (rows by area resref; * or missing: the default) */
	bool LoadAreas(const TableMgr& table);
	/** dccfloor.2da: any row names, columns AREA, REGION (a region's script name) and DC */
	bool LoadRegions(const TableMgr& table);
	void SetArea(const std::string& area, int dc);
	void SetRegion(const std::string& area, const std::string& region, int dc);
	int AreaDC(const std::string& area) const;
	/** the regions of an area that set their own floor DC (lower case names → DC) */
	const std::map<std::string, int>* Regions(const std::string& area) const;

private:
	std::map<std::string, int> areas;
	std::map<std::string, std::map<std::string, int>> regions;
};

// pick pocket (classes.md: DC 5 an unaware target, loose purse ... DC 20 watching its belongings)
constexpr int PICKPOCKET_DC_UNAWARE = 5;
constexpr int PICKPOCKET_DC_DEFAULT = 10;
constexpr int PICKPOCKET_DC_WATCHFUL = 20;
constexpr int PICKPOCKET_NEVER = 255; // the stock IE_PICKPOCKET value for "can't be picked"
/** the DC of a pick pocket: the target's own (its IE_PICKPOCKET, 1-254; 0 the default 10), 5 if it can't
 * see the thief or is helpless, 20 while it fights; -1 never (255) */
GEM_EXPORT int PickPocketDC(int targetStat, bool unaware, bool fighting);

// backstab (classes.md, "Backstab, revised"): why the target hasn't noticed the thief
enum class Unaware : uint8_t {
	No, // aware: no backstab
	Hidden, // the thief is in Stealth with a successful hide in shadows (or invisible)
	Unheard, // a successful sneak silently, out of the target's sight
	Unseen, // out of its sight
	NotFighting, // not in a fight yet, and its back is to the thief
};
GEM_EXPORT const char* UnawareText(Unaware why); // "hidden", "unheard", "unseen", "not yet fighting"
/** already aware (attacked by the thief, or saw it) stays aware until it loses track of it */
GEM_EXPORT Unaware TargetUnaware(bool alreadyAware, bool hidden, bool inSight, bool silent, bool targetFighting, bool behind);
/** a weapon that can backstab: any melee weapon but fists; at range only one with backstab dice (blowgun,
 * a thrown dagger) */
GEM_EXPORT bool BackstabWeapon(bool melee, bool fists, bool hasBackstabDice);
/** hiding again in a fight (classes.md): broken away for a full round, hidden, and lost for a full round */
GEM_EXPORT bool LosesTrack(int brokenAwayTicks, bool hidden, int lostTicks, int roundTicks);

}
}

#endif
