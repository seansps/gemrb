// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Extension.h
 * The DCC character data that doesn't fit in stats: lists that grow (the
 * spells a caster knows with their mercurial magic, manifestation and
 * whether they are lost for the day, corruptions), spellburn recovery and
 * the cleric's disapproval range; the 0-level occupation (OCCU) and the
 * character's place in the funnel (FUNL). M2.6 adds patrons as a record of
 * their own.
 *
 * It is saved as a variable-length extension of the CRE ("DCCX", see
 * docs/architecture/rules-dcc.md, "The CRE extension") and replicated to
 * co-op clients as the same bytes (NetDCCExtension). The layout, little
 * endian:
 *
 *   header  "DCCX", u16 version (1), u16 record count, u32 total size (header included)
 *   record  4 char tag, u16 record version, u16 flags (0), u32 payload size, payload
 *
 * Readers skip records they don't know and keep them (RawRecord), so an older
 * engine loads and saves a newer extension without losing anything.
 */

#ifndef DCC_EXTENSION_H
#define DCC_EXTENSION_H

#include "exports.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GemRB {
namespace DCC {

constexpr uint16_t EXT_VERSION = 1;
constexpr size_t EXT_HEADER_SIZE = 12;
constexpr size_t EXT_RECORD_HEADER_SIZE = 12;
// a sanity limit for reading (a CRE, a network message)
constexpr size_t EXT_MAX_SIZE = 64 * 1024;

// KnownSpell::flags
enum SpellFlags : uint8_t {
	SPELL_NONE = 0,
	SPELL_LOST = 1, // lost for the day: a failed wizard check, or disapproval took it
	SPELL_LUCKY = 2, // the elf's level 1 pick: its frozen Luck modifier adds to its checks
	SPELL_LEARNED = 4, // the learn-time rolls (mercurial magic, manifestation) were made
	SPELL_PENALTY = 8, // Table 5-7 8: -4 to its checks until the next rest
	SPELL_FRESH = 16, // Phase 3: learned at the last level-up, may still be swapped until the player confirms
	SPELL_REPLACED = 32 // Phase 3: the player's choice in place of a random pick (the optional rule)
};

// a spell a caster knows (the DCC base spell, see dccspell.2da)
struct KnownSpell {
	std::string spell; // the base resref, lower case
	uint8_t flags = SPELL_NONE;
	uint8_t manifestation = 0; // 1..N on the spell's manifestation list, 0 none
	std::vector<uint8_t> mercurial; // the d% results, 1-100 (several after "roll again")
	uint8_t mercurialSub = 0; // the learn-time sub-roll of an entry that has one
	int8_t luckyMod = 0; // SPELL_LUCKY: the frozen Luck modifier
	uint16_t casts = 0; // checks rolled with it so far
	uint8_t streak = 0; // mercurial 72: casts in a row; 73: the sign the next cast must take
};

// where a corruption came from
enum class CorruptionTable : uint8_t {
	Spell, // the spell's own list (row = entry)
	Minor, // Table 5-3
	Major, // Table 5-4
	Greater, // Table 5-5
	count
};

enum CorruptionFlags : uint8_t {
	CORR_NONE = 0,
	CORR_FROM_MISFIRE = 1 // given by a misfire (Table 5-6 6), not the caster's own
};

struct Corruption {
	CorruptionTable table = CorruptionTable::Minor;
	uint8_t row = 1; // the table row (1-10) or the spell's entry
	uint8_t sub = 0; // the row's own sub-roll (which eye, which tail), 0 none
	uint8_t flags = CORR_NONE;
	std::string spell; // the spell cast when it came, lower case
};

// the player's spellburn setting (a wizard's or elf's)
enum class SpellburnPolicy : uint8_t {
	Never, // no prompt, no burn
	Ask, // party members are asked before each check, with the preset as the default (others: the preset)
	Preset, // always the preset, without asking
	count
};
// when burning 1 Luck cancels a corruption (the policy's answer, and the prompt's default)
enum class CorruptionBurn : uint8_t {
	Never,
	Greater, // greater corruption only
	Major, // major and greater
	Always,
	count
};
constexpr SpellburnPolicy DEFAULT_SPELLBURN_POLICY = SpellburnPolicy::Ask;
constexpr CorruptionBurn DEFAULT_CORRUPTION_BURN = CorruptionBurn::Major;

// spellburn's lasting state: the points still burned (they come back 1 a day without spellburn)
// and the settings
struct Spellburn {
	std::array<uint8_t, 3> points {}; // Str, Agi, Sta still burned (part of the ability loss)
	uint32_t lastBurnDay = 0; // game day (1-based) of the last spellburn, 0 never
	uint8_t flavour = 0; // the last Table 5-1 roll (1-24), 0 none
	SpellburnPolicy policy = DEFAULT_SPELLBURN_POLICY;
	uint8_t preset = 0; // points a check burns by default
	CorruptionBurn corruption = DEFAULT_CORRUPTION_BURN;
	int Total() const { return points[0] + points[1] + points[2]; }
	bool IsDefault() const
	{
		return Total() == 0 && !lastBurnDay && !flavour && policy == DEFAULT_SPELLBURN_POLICY && !preset && corruption == DEFAULT_CORRUPTION_BURN;
	}
};

enum DisapprovalFlags : uint8_t {
	DISAPPROVAL_NONE = 0,
	DISAPPROVAL_NO_RESET = 1 // Table 5-7 15: the range doesn't reset at the next rest
};

struct Disapproval {
	uint8_t range = 1; // naturals up to this fail with disapproval (1: only a natural 1)
	uint8_t flags = DISAPPROVAL_NONE;
};

// Phase 3, the level-0 funnel: the occupation's generic skills (docs/rules/funnel.md, "Skill tags").
// Bits of Occupation::skills; the names are DCC::OccupationSkillName
enum OccupationSkill : uint32_t {
	SKILL_ANIMALS = 1u << 0,
	SKILL_FARMING = 1u << 1,
	SKILL_HERBALISM = 1u << 2,
	SKILL_MEDICINE = 1u << 3,
	SKILL_ALCHEMY = 1u << 4,
	SKILL_SMITHING = 1u << 5,
	SKILL_MASONRY = 1u << 6,
	SKILL_CARPENTRY = 1u << 7,
	SKILL_MINING = 1u << 8,
	SKILL_CRAFT_FINE = 1u << 9,
	SKILL_APPRAISAL = 1u << 10,
	SKILL_HAGGLE = 1u << 11,
	SKILL_LOCKS = 1u << 12,
	SKILL_STEALTH_STREET = 1u << 13,
	SKILL_DECEPTION = 1u << 14,
	SKILL_PERFORMANCE = 1u << 15,
	SKILL_LORE_RELIGION = 1u << 16,
	SKILL_LORE_ARCANE = 1u << 17,
	SKILL_LORE_STARS = 1u << 18,
	SKILL_LETTERS = 1u << 19,
	SKILL_NOBILITY = 1u << 20,
	SKILL_MILITARY = 1u << 21,
	SKILL_OUTDOORS = 1u << 22,
	SKILL_SEA = 1u << 23,
	SKILL_CLIMB = 1u << 24,
	SKILL_DIGGING = 1u << 25,
	SKILL_FILTH = 1u << 26,
	SKILL_BUTCHERY = 1u << 27,
	SKILL_ROPE = 1u << 28,
	SKILL_COOKING = 1u << 29
};
constexpr int OCCUPATION_SKILL_COUNT = 30;
/** the tag's name (funnel.md), eg. "SMITHING"; bit is 0-29; nullptr past them */
GEM_EXPORT const char* OccupationSkillName(int bit);
/** the bit of a tag name (any case), 0 if it isn't one */
GEM_EXPORT uint32_t OccupationSkillBit(const std::string& name);

// "OCCU" (Phase 3): the 0-level occupation, kept for the whole career (funnel.md)
struct Occupation {
	uint8_t id = 0; // Table 1-3 row by its lowest d100 number (dccoccup.2da), 0 none
	uint8_t sub = 0; // the row's sub-roll: farmer type 1-8, an animal 1-6, the pushcart 1-6; 0 none
	std::string tradeGood; // the trade good's item, lower case ("" when it has no item yet)
	std::string weapon; // the occupation's weapon item ("" none)
	std::string lastWeapon; // level 0: the weapon it last fought with, which it counts as trained in ("" none yet)
	uint32_t skills = 0; // OccupationSkill bits (the occupation's tags plus any acquired ones)
	bool Empty() const { return !id && !sub && tradeGood.empty() && weapon.empty() && lastWeapon.empty() && !skills; }
};

// "FUNL" (Phase 3): where a character stands in the level-0 funnel
enum class FunnelStatus : uint8_t {
	None, // never in a funnel (or a character made at level 1)
	Alive, // in the funnel, alive
	Dead, // died in the funnel
	Chosen, // survived and was chosen for the party of four (picks a class at level 1)
	Reserve, // survived, not chosen: the reserve pool
	count
};
GEM_EXPORT const char* FunnelStatusName(FunnelStatus status);

enum FunnelFlags : uint8_t {
	FUNNEL_NONE = 0,
	FUNNEL_CLASS_PENDING = 1, // reached 10 XP: its level-1 class pick is waiting
	FUNNEL_RECOVERED = 2 // was rolled over and lived (recovering the body)
};

constexpr uint8_t FUNNEL_NO_PLAYER = 0xff;

struct Funnel {
	FunnelStatus status = FunnelStatus::None;
	uint8_t order = 0; // roster order, 1-16 (the order it was rolled), 0 none
	uint8_t player = FUNNEL_NO_PLAYER; // co-op: the player slot it was dealt to (0 the host), FUNNEL_NO_PLAYER none
	uint8_t flags = FUNNEL_NONE;
	uint32_t diedAt = 0; // game time of its death, 0 alive
	std::string diedIn; // the area it died in ("" alive)
	bool Empty() const { return status == FunnelStatus::None && !order && player == FUNNEL_NO_PLAYER && !flags && !diedAt && diedIn.empty(); }
};

struct RawRecord {
	std::array<char, 4> tag {};
	uint16_t version = 0;
	std::vector<uint8_t> data;
};

struct GEM_EXPORT CharacterExt {
	std::vector<KnownSpell> spells;
	std::vector<Corruption> corruptions;
	Spellburn burn;
	Disapproval disapproval;
	Occupation occupation; // Phase 3: OCCU
	Funnel funnel; // Phase 3: FUNL
	std::vector<RawRecord> other; // records this version doesn't know, kept as they are

	/** nothing worth saving: a creature without any of it doesn't get an extension */
	bool Empty() const;
	KnownSpell* FindSpell(const std::string& resref);
	const KnownSpell* FindSpell(const std::string& resref) const;
	bool operator==(const CharacterExt& other) const;
	bool operator!=(const CharacterExt& other) const { return !(*this == other); }
};

/** the extension as bytes (header and records) */
GEM_EXPORT std::vector<uint8_t> WriteExtension(const CharacterExt& ext);
/** reads an extension; false (and out untouched) for anything that isn't a well formed one */
GEM_EXPORT bool ReadExtension(const uint8_t* data, size_t size, CharacterExt& out);
/** the total size an extension's header claims, 0 if data doesn't start with one */
GEM_EXPORT size_t ExtensionSize(const uint8_t* data, size_t size);

}
}

#endif
