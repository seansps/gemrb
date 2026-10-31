// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Extension.h
 * The DCC character data that doesn't fit in stats: lists that grow (the
 * spells a caster knows with their mercurial magic, manifestation and
 * whether they are lost for the day, corruptions), spellburn recovery and
 * the cleric's disapproval range. M2.6 adds patrons, Phase 3 the occupation
 * and the funnel, as records of their own.
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
	SPELL_PENALTY = 8 // Table 5-7 8: -4 to its checks until the next rest
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
