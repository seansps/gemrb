// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Magic.h
 * DCC spellcasting as pure functions (docs/rules/magic.md, spell-format.md,
 * classes.md; the Ethren decisions in docs/rules/open-questions.md): the spell
 * check and its result band, the natural 1, cleric disapproval, spellburn,
 * mercurial magic and corruption rolls, lay on hands and turn unholy; and the
 * tables they read: the spells' own band tables (generated from the YAML
 * sources by tools/iefmt) and the generic Tables 4-4, 5-1, 5-3 to 5-7 and
 * mercurial magic (unhardcoded/ethren).
 *
 * The engine glue is in Scriptable/ActorDCCMagic.cpp.
 */

#ifndef DCC_MAGIC_H
#define DCC_MAGIC_H

#include "exports.h"

#include "DCC/Character.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GemRB {

class TableMgr;

namespace DCC {

enum class CasterKind : uint8_t {
	None,
	Wizard, // wizards and elves: arcane, Int, spell loss, spellburn, corruption, mercurial magic
	Cleric // Per, disapproval, no armor penalty
};
/** who casts how: wizards and elves of level 1+ arcane, clerics of level 1+ divine */
GEM_EXPORT CasterKind CasterFor(int classID, int level);

// DCC:Magic (opcode 0x1c2): Parameter2 is the kind in the low byte (and COMBAT_UNTIL_HEALED /
// COMBAT_FOR_GOOD from Combat.h for lasting ones), Parameter1 its value
enum class MagicEffect : uint8_t {
	SpellMod, // Parameter1 to spell checks (IE_DCC_SPELLMOD): disapproval, mercurial magic, blessings
	Block, // DCC::MagicBlock bits (IE_DCC_NOCAST)
	LayOnHandsMod, // Parameter1 to lay on hands checks (IE_DCC_LOHMOD)
	Mutation, // DCC::Mutation bits for a while (IE_DCC_MUTATION; lasting ones come from the corruption list)
	count
};

// IE_DCC_NOCAST
enum MagicBlock : uint32_t {
	BLOCK_NONE = 0,
	BLOCK_SPELLS = 1, // no spells at all
	BLOCK_LAY_ON_HANDS = 2,
	BLOCK_TURN = 4,
	BLOCK_LOH_ONCE = 8 // lay on hands only once a creature (Table 5-7 20)
};

// IE_DCC_MUTATION: what a corruption looks like (the record screen, the tint); the MUT: key's names
enum Mutation : uint32_t {
	MUT_NONE = 0,
	MUT_PUSTULES = 1u << 0,
	MUT_SKIN = 1u << 1,
	MUT_GAIT = 1u << 2,
	MUT_EYES = 1u << 3,
	MUT_EARS = 1u << 4,
	MUT_SHAKES = 1u << 5,
	MUT_HAIR = 1u << 6,
	MUT_WITHER = 1u << 7,
	MUT_FACE = 1u << 8,
	MUT_BLOAT = 1u << 9,
	MUT_CRACKLE = 1u << 10,
	MUT_HEIGHT = 1u << 11,
	MUT_BEAST = 1u << 12,
	MUT_HORNS = 1u << 13,
	MUT_TONGUE = 1u << 14,
	MUT_TENTACLES = 1u << 15,
	MUT_CLAW = 1u << 16,
	MUT_TAIL = 1u << 17,
	MUT_SCALES = 1u << 18,
	MUT_BEAK = 1u << 19,
	MUT_MARK = 1u << 20,
	MUT_HANDS = 1u << 21, // the hands take the colour of the spell
	MUT_GHOST = 1u << 22, // translucent, ghostly
	MUT_BURNT = 1u << 23, // blackened, burnt
	MUT_SLEEPLESS = 1u << 24,
	MUT_STENCH = 1u << 25,
	MUT_AURA = 1u << 26 // something circles the caster (a force stone)
};
/** a MUT: name (PUSTULES, SKIN, ...) as its bit, 0 if unknown */
GEM_EXPORT uint32_t MutationBit(const std::string& name);
/** the bit's name for the record screen ("pustules", ...) */
GEM_EXPORT const char* MutationName(uint32_t bit);
/** the tint (0xRRGGBB) of the strongest mutation in bits, 0 for none: the visual hook until the
 * art pipeline can change sprites */
GEM_EXPORT uint32_t MutationTint(uint32_t bits);

// --- the check ------------------------------------------------------------------------------

struct SpellCheckInput {
	int natural = 0; // the die's face
	int die = 20; // the action die (after mercurial magic)
	int casterLevel = 0;
	int abilityMod = 0; // Int (wizards, elves) or Per (clerics)
	int armorPenalty = 0; // wizards and elves only (0 or less)
	int spellburn = 0; // points burned for the bonus (not the ones that pay for a lost spell)
	int luck = 0; // only where a class rule adds Luck (turning, the elf's lucky spell)
	int other = 0; // effects, mercurial magic, situational modifiers
	bool cleric = false;
	int disapprovalRange = 1; // clerics: naturals up to this fail with disapproval
	bool cursed = false; // Luck 0: every roll fails
	bool autoCrit = false; // 20 points of spellburn: counts as a natural 20
};

struct SpellCheckResult {
	int natural = 0;
	int die = 20;
	int total = 0;
	bool critical = false; // a natural 20 (or more on a bigger die): + CL again
	bool natOne = false; // the natural-1 row (wizards), always a failure
	bool disapproval = false; // clerics: the natural was in the disapproval range
	bool cursed = false;
};

/** d20 (the action die) + CL + ability mod + armor + spellburn + Luck (where it applies) + other;
 * a natural 20 or more adds CL again (magic.md §3) */
GEM_EXPORT SpellCheckResult ResolveSpellCheck(const SpellCheckInput& input);

// --- the spells' tables -----------------------------------------------------------------------

// natural-1 outcomes (bits)
enum NatOneOutcome : uint32_t {
	NAT1_NONE = 0,
	NAT1_CORRUPT = 1,
	NAT1_TAINT = 2, // patron taint; corruption for a caster without a patron (patrons: M2.6)
	NAT1_MISFIRE = 4
};
/** "CORRUPT+TAINT+MISFIRE" and the like; false for an unknown word */
GEM_EXPORT bool ParseNatOne(const std::string& text, uint32_t& outcome);

struct SpellBand {
	int min = 1; // the lowest check total of the band
	bool natOne = false; // the natural-1 row: only a natural 1 reaches it
	bool lost = false; // wizards: the spell is lost for the day
	bool success = false;
	std::string spell; // the band's SPL (the family member)
	std::string effects; // the band's engine terms (DUR, REPEAT, SPREAD, TARGETS, AREA)
	std::string text; // the log's words
	// the second column of a two-column spell (Blessing: on an ally instead of the caster)
	std::string spell2;
	std::string effects2;
	std::string text2;
	bool success2 = false;
};

struct NatOneRow {
	int min = -99; // the lowest sub-roll (1d6 + Luck) of the row
	uint32_t outcome = NAT1_NONE;
};

struct SpellEntry {
	int roll = 1; // the die result it stands for
	std::string effects;
	std::string text;
};

enum SpellInfoFlags : uint32_t {
	SPELLINFO_NONE = 0,
	SPELLINFO_SAVE_HALF = 1
};

struct GEM_EXPORT SpellInfo {
	std::string base; // the base SPL (lower case), what the caster knows
	CasterKind kind = CasterKind::Wizard;
	int level = 1;
	std::string name;
	std::vector<SpellBand> bands; // ascending
	std::vector<NatOneRow> natOne; // ascending
	std::vector<SpellEntry> corruption;
	std::vector<SpellEntry> misfire;
	std::vector<SpellEntry> manifestation;

	/** the band of a check: the natural-1 row on a natural 1, the failure band on a cursed or
	 * disapproved check, else the highest band whose minimum the total reaches; -1 if none */
	int BandFor(const SpellCheckResult& check) const;
	/** the first success band's minimum (the spell's DC for "success"), 0 if there is none */
	int FirstSuccess() const;
	/** loads a spell's table (INFO: level, class and name; B*: bands, BN the natural-1 row; N*: the natural-1 sub-roll; C*: corruption,
	 * F*: misfire, M*: manifestation; see tools/iefmt/spells.py) */
	bool Load(const std::string& base, const TableMgr& table);
};

/** the natural-1 sub-roll's outcome (rows ascending by min; the last that the roll reaches) */
GEM_EXPORT uint32_t NatOneFor(const std::vector<NatOneRow>& rows, int subroll);
/** the standard band minimums of a class and spell level (spell-format.md §2), for checks */
GEM_EXPORT std::vector<int> StandardBands(CasterKind kind, int level);
/** an entry of a list by its die result (the nearest below for a gap, the first for less) */
GEM_EXPORT const SpellEntry* EntryFor(const std::vector<SpellEntry>& list, int roll);

// --- corruption, misfire, mercurial magic ----------------------------------------------------

/** 1d10 - spell level + Luck mod, read on Tables 5-3/5-4/5-5: 1 or less is row 1, 10 or more row 10 */
GEM_EXPORT int CorruptionRoll(int d10, int spellLevel, int luckMod);
/** d% + Luck mod x 10, clamped to 1-100 (Ethren decision) */
GEM_EXPORT int MercurialRoll(int d100, int luckMod);
/** 4d20 + Luck mod x 10 for the 00 entry's re-rolls, clamped to 1-100 */
GEM_EXPORT int MercurialReroll(int fourD20, int luckMod);
// the mercurial entries the engine reads at cast time
enum MercurialEffect : int {
	MERC_DIE_DOWN2 = 2,
	MERC_STA_COST = 4,
	MERC_DIE_DOWN1 = 5,
	MERC_NEXT_ROUND_PENALTY = 6,
	MERC_ROLLS_PENALTY = 7,
	MERC_ALWAYS_CORRUPT = 9,
	MERC_CHECK_PENALTY = 12,
	MERC_SLOW = 13,
	MERC_SLEEP_AFTER = 14,
	MERC_SILENCED = 16,
	MERC_DAMAGE_SELF = 23,
	MERC_TWO_D10 = 35,
	MERC_MUTE = 36,
	MERC_RANDOM_DIE = 38,
	MERC_NO_CHANGE_LOW = 41,
	MERC_NO_CHANGE_HIGH = 60,
	MERC_GLOW = 67,
	MERC_TRANSPARENT = 69,
	MERC_STREAK = 72,
	MERC_SWING = 73,
	MERC_PLANTS = 74,
	MERC_ARMOR_AFTER = 85,
	MERC_HEAL_AREA = 91,
	MERC_SPELL_BOOST = 95,
	MERC_DIE_UP1 = 96,
	MERC_DRAIN = 97,
	MERC_DIE_UP2 = 98,
	MERC_ROLL_TWICE = 99,
	MERC_ROLL_TWICE_4D20 = 100
};
/** the die steps an entry adds to the spell check die (dice chain) */
GEM_EXPORT int MercurialDieSteps(int entry);
/** the spell check die with every die-changing entry applied to the action die (d20 -> ...);
 * 38's die (rolled per cast with d6: 1-2 d16, 3-4 d20, 5 d24, 6 d30) overrides it all */
GEM_EXPORT int MercurialDie(int actionDie, const std::vector<uint8_t>& entries, int d6ForEntry38);

// --- spellburn ---------------------------------------------------------------------------------

/** the most points that can be burned from these current scores (Str, Agi, Sta): none goes
 * below 1 (Ethren decision) */
GEM_EXPORT int SpellburnMax(const std::array<int, 3>& current);
/** points (Str, Agi, Sta) are burnable from current */
GEM_EXPORT bool SpellburnLegal(const std::array<int, 3>& points, const std::array<int, 3>& current);
/** points that turn a check into a natural 20 */
constexpr int SPELLBURN_AUTO_CRIT = 20;
/** what a burn adds to the check: +1 a point, less the spell level if it pays to cast a lost spell
 * (then less than the level is no cast at all: -1) */
GEM_EXPORT int SpellburnBonus(int points, bool recastsLost, int spellLevel);
/** a day's recovery without spellburn: 1 point in all (Ethren decision), from the ability with
 * the most burned points (Str before Agi before Sta on a tie); returns the ability index, -1 none */
GEM_EXPORT int SpellburnRecoveryIndex(const std::array<uint8_t, 3>& points);

// --- cleric disapproval ------------------------------------------------------------------------

constexpr int DISAPPROVAL_MAX = 20;
/** a natural in the range fails with disapproval */
GEM_EXPORT bool InDisapprovalRange(int natural, int range);
/** (natural)d4 - Luck mod, read on Table 5-7: clamped to 1-20 (Ethren decision) */
GEM_EXPORT int DisapprovalRoll(int d4Sum, int luckMod);
/** the range after a failed check: +1, at most DISAPPROVAL_MAX */
GEM_EXPORT int DisapprovalWidened(int range);
/** gold to sacrifice for each point of range */
constexpr int ATONEMENT_GOLD = 50;
/** the range after sacrificing gold: 1 point per 50 gp, never below 1 */
GEM_EXPORT int DisapprovalAfterSacrifice(int range, int gold);

// --- lay on hands and turn unholy ----------------------------------------------------------------

enum class Kinship : uint8_t {
	Same, // the same alignment
	Adjacent, // one step apart (neutral and lawful or chaotic)
	Opposed // lawful and chaotic
};
GEM_EXPORT Kinship KinshipFor(Alignment cleric, Alignment target);
/** the healing dice of a lay on hands check (classes.md): 0 for a failure; capped by the
 * target's HD or level */
GEM_EXPORT int LayOnHandsDice(int total, Kinship kinship, int targetHD);

struct TurnCell {
	enum class Kind : uint8_t {
		None, // NE
		Turn, // T: count creatures flee
		Kill // K: count destroyed
	} kind = Kind::None;
	int dice = 0; // the count: dice d sides + (CL if addCL) + bonus
	int sides = 0;
	int bonus = 0;
	bool addCL = false;
	int damageDice = 0; // D: damage the turned take
	int damageSides = 0;
	bool noSave = false;
	int Count(int casterLevel, const Roller& roller) const;
};
/** "NE", "T1", "T1d3+CL", "T1d8+CL;D1d4;NS", "K2d6+CL"; false for a malformed cell */
GEM_EXPORT bool ParseTurnCell(const std::string& text, TurnCell& cell);

struct TurnRow {
	int min = 1;
	int range = 0; // feet
	int smiteDice = 0; // holy smite: extra damage to every unholy creature in range, no save
	int smiteSides = 0;
	std::array<TurnCell, 8> cells; // 1, 2, 3-4, 5-6, 7-8, 9-10, 11-12 HD (and 13+: as 11-12)
};
/** the column of a creature's HD: 0 for 1 HD (or less), 1 for 2, 2 for 3-4, ... 6 for 11-12+ */
GEM_EXPORT int TurnColumn(int hd);

// --- the tables ----------------------------------------------------------------------------------

struct MagicEntry {
	int roll = 1;
	int subDie = 0; // the row's own sub-roll (which eye, which animal), 0 none
	std::string effects;
	std::string text;
};

class GEM_EXPORT MagicTableList {
public:
	/** a 2DA with rows named by roll (or ranges "1-40") and EFFECTS, TEXT and an optional SUB column */
	bool Load(const TableMgr& table);
	void Set(std::vector<MagicEntry> list) { rows = std::move(list); }
	/** the row for a roll, clamped to the table's ends */
	const MagicEntry* Lookup(int roll) const;
	const std::vector<MagicEntry>& Rows() const { return rows; }
	bool IsEmpty() const { return rows.empty(); }

private:
	std::vector<MagicEntry> rows; // ascending
};

class GEM_EXPORT MagicTables {
public:
	std::array<MagicTableList, 3> corruption; // Tables 5-3, 5-4, 5-5
	MagicTableList misfire; // Table 5-6
	MagicTableList disapproval; // Table 5-7
	MagicTableList mercurial; // mercurial magic, d% (1-100)
	MagicTableList spellburn; // Table 5-1
	std::vector<TurnRow> turn; // Table 4-4

	bool LoadTurn(const TableMgr& table);
	const TurnRow* TurnFor(int total) const;
};

}
}

#endif
