// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC spellcasting (DCCRules = 1) for actors: spell checks instead of memorization, the
// band's SPL, spell loss, the check total as the save DC, spellburn (a prompt before the
// check), the natural 1 (corruption with its Luck offer, misfires), mercurial magic, cleric
// disapproval, lay on hands, turn unholy and atonement. The rules are pure functions in
// core/DCC/Magic.h; Scriptable.cpp calls in here behind GFFlags::RULES_DCC. See
// docs/architecture/rules-dcc.md, "Spellcasting (M2.5)".
//
// Dice order of a cast (DCCDie, so DCCForceDice can drive it): mercurial 38's d6 (that entry
// only), the check die (two dice for mercurial 35), mercurial 97's damage, mercurial 73's d5;
// then by outcome: the Table 5-1 flavour d24 (when spellburning), the natural-1 sub-roll d6,
// the spell's corruption die and Table 5-3/5-4/5-5's d10 and sub-die, the misfire die, a
// cleric's disapproval d4s, the band's DUR and REPEAT dice, and mercurial results after the cast.

#include "ie_stats.h"

#include "DisplayMessage.h"
#include "EffectQueue.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "Map.h"
#include "Prompts.h"
#include "RNG.h"
#include "Spell.h"

#include "DCC/Classes.h"
#include "DCC/Combat.h"
#include "DCC/CombatTables.h"
#include "DCC/DiceChain.h"
#include "DCC/Luck.h"
#include "DCC/Magic.h"
#include "DCC/Rules.h"
#include "DCC/Stats.h"
#include "GUI/GameControl.h"
#include "Logging/Logging.h"
#include "Net/NetSession.h"
#include "Scriptable/Actor.h"

#include <algorithm>
#include <cctype>

namespace GemRB {

using namespace DCC;

static EffectRef fx_dcc_magic_ref = { "DCC:Magic", -1 };
static EffectRef fx_dcc_combat_magic_ref = { "DCC:Combat", -1 };
static EffectRef fx_dcc_loss_magic_ref = { "DCC:AbilityLoss", -1 };
static EffectRef fx_dcc_heal_ref = { "CurrentHPModifier", -1 };
static EffectRef fx_dcc_magic_damage_ref = { "Damage", -1 };
static EffectRef fx_dcc_death_magic_ref = { "Death", -1 };

// DCC:Magic's Parameter3 marks of the Table 5-7 penalties that end another way than a rest
constexpr ieDword MARK_MEDITATE = 11; // a Will save DC 15 a night ends it
constexpr ieDword MARK_TITHE = 14; // atonement ends it

static const RulesData* Rules()
{
	return core->GetDCCRules();
}

static int Signed(ieDword value)
{
	return static_cast<int>(static_cast<ieDwordSigned>(value));
}

static String Widen(const std::string& text)
{
	return String(text.begin(), text.end());
}

static std::string Lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

static std::string Upper(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
	return text;
}

static String Plus(int value)
{
	if (!value) return u"";
	return fmt::format(u" {:+}", value);
}

static void Feedback(String text)
{
	Actor::DCCFeedback(std::move(text), FT_COMBAT);
}

static std::vector<std::string> Split(const std::string& text, char separator)
{
	std::vector<std::string> parts;
	if (text.empty()) return parts;
	std::string::size_type start = 0;
	while (true) {
		auto end = text.find(separator, start);
		parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return parts;
}

static int Today()
{
	const Game* game = core->GetGame();
	if (!game || !core->Time.day_size) return 1;
	return int(game->GameTime / core->Time.day_size) + 1;
}

static const char16_t* AbilityShort(int ability)
{
	static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
	return names[Clamp(ability, 0, 5)];
}

// the spellbook page of a caster kind: wizards and elves the wizard book, clerics the priest one
static int BookType(CasterKind kind)
{
	return kind == CasterKind::Cleric ? IE_SPELL_TYPE_PRIEST : IE_SPELL_TYPE_WIZARD;
}

// "1d4+2", "1d3x2" (times a number), "5": rolled with roller
static int RollText(const std::string& text, const Roller& roller)
{
	std::string dice = text;
	int times = 1;
	auto x = text.find('x');
	if (x != std::string::npos) {
		dice = text.substr(0, x);
		if (!valid_signednumber(text.substr(x + 1).c_str(), times)) times = 1;
	}
	DiceExpr expr;
	if (!ParseDice(dice, expr)) return 0;
	return RollDice(expr, roller) * times;
}

// SL and CL in a term's arguments become the spell's and the caster's level
static std::string Substitute(std::string text, int spellLevel, int casterLevel)
{
	for (const auto& pair : { std::make_pair(std::string("SL"), spellLevel), std::make_pair(std::string("CL"), casterLevel) }) {
		std::string::size_type at;
		while ((at = text.find(pair.first)) != std::string::npos) {
			text.replace(at, 2, std::to_string(pair.second));
		}
	}
	return text;
}

// ---------------------------------------------------------------------------
// who casts, what is known

CasterKind Actor::GetDCCCasterKind() const
{
	return CasterFor(Signed(BaseStats[IE_CLASS]), Signed(BaseStats[IE_LEVEL]));
}

void Actor::DCCExtChanged()
{
	++dccExtRevision;
	DCCSyncSpellbook();
}

int Actor::DCCLearnSpell(const ResRef& base, bool rolls)
{
	const RulesData* rules = Rules();
	const SpellInfo* info = rules ? rules->Spell(base.c_str()) : nullptr;
	if (!info) return -1;
	std::string name = Lower(base.c_str());
	if (dccExt.FindSpell(name)) return -1;

	if (!spellbook.KnowSpell(base)) {
		const Spell* spl = gamedata->GetSpell(base, true);
		if (spl) {
			spellbook.LearnSpell(spl, 0, 0, 0);
			gamedata->FreeSpell(spl, base, false);
		}
	}

	KnownSpell known;
	known.spell = name;
	int first = 0;
	if (rolls) {
		known.flags |= SPELL_LEARNED;
		// mercurial magic (wizards and elves): d% + Luck mod x 10, again twice on 99 and 00
		if (info->kind == CasterKind::Wizard && GetDCCCasterKind() == CasterKind::Wizard) {
			int luck = GetDCCAbilityMod(Ability::Luck);
			int pending = 1;
			bool fourD20 = false;
			while (pending > 0 && known.mercurial.size() < 8) {
				--pending;
				int roll = fourD20 ? MercurialReroll(DCCDie(20) + DCCDie(20) + DCCDie(20) + DCCDie(20), luck) : MercurialRoll(DCCDie(100), luck);
				if (roll >= MERC_ROLL_TWICE) {
					pending += 2;
					fourD20 = roll == MERC_ROLL_TWICE_4D20;
				}
				known.mercurial.push_back(uint8_t(roll));
				if (!first) first = roll;
			}
			// an entry's own learn-time variant (which sound, which condition, ...)
			for (uint8_t entry : known.mercurial) {
				const MagicEntry* row = rules->magic.mercurial.Lookup(entry);
				if (row && row->subDie && !known.mercurialSub) known.mercurialSub = uint8_t(DCCDie(row->subDie));
			}
		}
		if (!info->manifestation.empty()) {
			known.manifestation = uint8_t(DCCDie(info->manifestation.back().roll));
		}
	}
	dccExt.spells.push_back(known);
	DCCExtChanged();

	String line = fmt::format(u"{} learns {}", GetName(), Widen(info->name));
	if (known.manifestation) {
		const SpellEntry* how = EntryFor(info->manifestation, known.manifestation);
		if (how) line += fmt::format(u" (it shows as {})", Widen(how->text));
	}
	for (uint8_t entry : known.mercurial) {
		const MagicEntry* row = rules->magic.mercurial.Lookup(entry);
		line += fmt::format(u"; mercurial magic {}: {}", int(entry), row ? Widen(row->text) : u"?");
	}
	Feedback(std::move(line));
	return first;
}

void Actor::DCCSyncSpellbook()
{
	const RulesData* rules = Rules();
	if (!rules) return;
	// a DCC spell the spellbook knows but the extension doesn't (an authored creature) joins it
	// without learn-time rolls
	for (int type : { IE_SPELL_TYPE_PRIEST, IE_SPELL_TYPE_WIZARD }) {
		for (unsigned int level = 0; level < spellbook.GetSpellLevelCount(type); ++level) {
			const CRESpellMemorization* sm = spellbook.GetSpellMemorization(type, level);
			if (!sm) continue;
			for (const auto& known : sm->knownSpells) {
				std::string name = Lower(known->SpellResRef.c_str());
				if (!dccExt.FindSpell(name) && rules->Spell(name)) {
					KnownSpell entry;
					entry.spell = name;
					dccExt.spells.push_back(entry);
				}
			}
		}
	}
	bool changed = false;
	for (const auto& known : dccExt.spells) {
		const SpellInfo* info = rules->Spell(known.spell);
		if (!info) continue;
		ResRef ref(known.spell);
		int type = BookType(info->kind);
		unsigned int level = unsigned(std::max(1, info->level) - 1);
		if (!spellbook.KnowSpell(ref)) {
			const Spell* spl = gamedata->GetSpell(ref, true);
			if (!spl) continue;
			spellbook.LearnSpell(spl, 0, 0, 0);
			gamedata->FreeSpell(spl, ref, false);
		}
		CRESpellMemorization* sm = spellbook.GetSpellMemorization(type, level);
		if (!sm) continue;
		// exactly one memorized copy, usable unless the spell is lost
		ieDword flags = (known.flags & SPELL_LOST) ? 0 : 1;
		bool found = false;
		for (auto it = sm->memorizedSpells.begin(); it != sm->memorizedSpells.end();) {
			if ((*it)->SpellResRef != ref) {
				++it;
				continue;
			}
			if (found) {
				it = sm->memorizedSpells.erase(it);
				changed = true;
				continue;
			}
			found = true;
			if ((*it)->Flags != flags) {
				(*it)->Flags = flags;
				changed = true;
			}
			++it;
		}
		if (!found) {
			auto memorized = MakeHolder<CREMemorizedSpell>();
			memorized->SpellResRef = ref;
			memorized->Flags = flags;
			sm->memorizedSpells.push_back(std::move(memorized));
			changed = true;
		}
		ieWord slots = ieWord(sm->memorizedSpells.size());
		if (sm->SlotCount < slots) sm->SlotCount = slots;
		if (sm->SlotCountWithBonus < slots) sm->SlotCountWithBonus = slots;
	}
	if (changed) spellbook.ClearSpellInfo();

	// the lasting look of the corruptions (IE_DCC_MUTATION with the timed ones from effects)
	uint32_t bits = MUT_NONE;
	for (const auto& corruption : dccExt.corruptions) {
		std::string effects;
		if (corruption.table == CorruptionTable::Spell) {
			const SpellInfo* info = rules->Spell(corruption.spell);
			const SpellEntry* entry = info ? EntryFor(info->corruption, corruption.row) : nullptr;
			if (entry) effects = entry->effects;
		} else {
			const MagicEntry* entry = rules->magic.corruption[size_t(corruption.table) - 1].Lookup(corruption.row);
			if (entry) effects = entry->effects;
		}
		for (const auto& term : Split(effects, ',')) {
			if (term.compare(0, 4, "MUT:") != 0) continue;
			auto parts = Split(term, ':');
			if (parts.size() == 2) bits |= MutationBit(parts[1]);
		}
	}
	dccMutations = bits;
}

bool Actor::DCCCanCast(const ResRef& base, String* why) const
{
	auto refuse = [why](String reason) {
		if (why) *why = std::move(reason);
		return false;
	};
	const RulesData* rules = Rules();
	const SpellInfo* info = rules ? rules->Spell(base.c_str()) : nullptr;
	if (!info) return refuse(u"not a DCC spell");
	CasterKind kind = GetDCCCasterKind();
	if (kind == CasterKind::None) return refuse(u"can't cast spells");
	const KnownSpell* known = dccExt.FindSpell(Lower(base.c_str()));
	if (!known) return refuse(u"doesn't know the spell");
	if (GetDCCInjuries() & INJ_AMNESIA) return refuse(u"can't remember how to cast (amnesia)");
	if (GetStat(IE_DCC_NOCAST) & BLOCK_SPELLS) return refuse(u"its spellcasting is taken away");
	if (IsDCCHeld()) return refuse(u"can't act");
	if (info->level > Signed(GetStat(IE_DCC_MAXSPELL))) return refuse(u"can't cast spells of that level");
	if (known->flags & SPELL_LOST) {
		// a lost spell only with spellburn of at least its level (magic.md §6)
		if (kind != CasterKind::Wizard) return refuse(u"lost the spell for the day");
		std::array<int, 3> current { Signed(GetStat(IE_STR)), Signed(GetStat(IE_DEX)), Signed(GetStat(IE_CON)) };
		if (dccExt.burn.policy == SpellburnPolicy::Never || SpellburnMax(current) < info->level) return refuse(u"lost the spell for the day");
	}
	return true;
}

// the default burn: the preset (at least the spell's level for a lost spell), from the highest
// current score first, never below 1
static std::array<int, 3> PresetBurn(const Actor* actor, int points)
{
	std::array<int, 3> burn {};
	std::array<int, 3> room { std::max(0, Signed(actor->GetStat(IE_STR)) - 1), std::max(0, Signed(actor->GetStat(IE_DEX)) - 1),
				  std::max(0, Signed(actor->GetStat(IE_CON)) - 1) };
	while (points > 0) {
		int best = -1;
		for (int i = 0; i < 3; ++i) {
			if (room[i] - burn[i] <= 0) continue;
			if (best < 0 || room[i] - burn[i] > room[best] - burn[best]) best = i;
		}
		if (best < 0) break;
		++burn[best];
		--points;
	}
	return burn;
}

void Actor::DCCBeginCast(const ResRef& base, bool ask)
{
	const RulesData* rules = Rules();
	const SpellInfo* info = rules ? rules->Spell(base.c_str()) : nullptr;
	if (dccCast.prompt) core->GetPrompts().Cancel(dccCast.prompt);
	ieDword castRound = dccCast.castRound;
	int casts = dccCast.castsThisRound;
	dccCast = DCCCastState();
	dccCast.castRound = castRound;
	dccCast.castsThisRound = casts;
	dccCast.spell = base;
	if (!info || GetDCCCasterKind() != CasterKind::Wizard) {
		dccCast.decided = true;
		return;
	}
	const KnownSpell* known = dccExt.FindSpell(Lower(base.c_str()));
	bool lost = known && (known->flags & SPELL_LOST);
	const Spellburn& settings = dccExt.burn;
	int preset = settings.policy == SpellburnPolicy::Never ? 0 : settings.preset;
	if (lost) preset = std::max(preset, info->level);
	dccCast.burn = PresetBurn(this, preset);
	dccCast.decided = true;
	if (dccNextBurnSet) {
		dccCast.burn = dccNextBurn;
		dccNextBurnSet = false;
		return;
	}
	if (!ask || settings.policy != SpellburnPolicy::Ask || !InParty) return;
	const NetSession* net = core->GetNetSession();
	if ((net && !net->IsHost()) || !core->GetGameControl()) return;

	// ask the owner: Str, Agi, Sta points (each score stays at 1 or more)
	std::array<int, 3> current { Signed(GetStat(IE_STR)), Signed(GetStat(IE_DEX)), Signed(GetStat(IE_CON)) };
	if (SpellburnMax(current) <= 0) return;
	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "spellburn";
	q.title = "Spellburn?";
	std::string name = fmt::format("{}", fmt::WideToChar { GetShortName() });
	q.text = fmt::format("{} casts {}. Burn Strength, Agility or Stamina for +1 a point to the spell check{}? 20 points make it a natural 20.", name,
			     info->name, lost ? fmt::format(" (the spell is lost: the first {} points pay for it)", info->level) : std::string());
	static const char* labels[] = { "Strength", "Agility", "Stamina" };
	for (int i = 0; i < 3; ++i) {
		q.fields.push_back({ labels[i], 0, std::max(0, current[i] - 1), dccCast.burn[i] });
	}
	q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));
	ieDword selfID = GetGlobalID();
	ResRef spell = base;
	uint32_t id = core->GetPrompts().Ask(this, std::move(q), [selfID, spell](const PromptResult& r) {
		Game* game = core->GetGame();
		Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
		if (!self || self->dccCast.spell != spell || !self->dccCast.prompt) return;
		self->dccCast.prompt = 0;
		if (r.outcome == PromptOutcome::Cancelled) return;
		for (int i = 0; i < 3; ++i) {
			self->dccCast.burn[i] = std::max(0, r.Value(size_t(i)));
		}
		self->dccCast.decided = true;
	});
	if (id) dccCast.prompt = id;
}

// the action die of this cast: the round's first cast uses the first die, a second cast in the
// same round the second one (Ethren decision); then mercurial magic changes it
int Actor::DCCCheckDie(const KnownSpell* known, int& natural)
{
	const Game* game = core->GetGame();
	ieDword round = game && core->Time.round_size ? game->GameTime / core->Time.round_size : 0;
	if (round != dccCast.castRound) {
		dccCast.castRound = round;
		dccCast.castsThisRound = 0;
	}
	int index = std::min(dccCast.castsThisRound, 2);
	++dccCast.castsThisRound;
	int die = Signed(GetStat(IE_DCC_ACTDIE1 + index));
	if (die <= 0) die = Signed(GetStat(IE_DCC_ACTDIE1));
	if (die <= 0) die = 20;
	bool twoD10 = false;
	if (known && !known->mercurial.empty()) {
		bool random = std::find(known->mercurial.begin(), known->mercurial.end(), uint8_t(MERC_RANDOM_DIE)) != known->mercurial.end();
		die = MercurialDie(die, known->mercurial, random ? DCCDie(6) : 0);
		twoD10 = std::find(known->mercurial.begin(), known->mercurial.end(), uint8_t(MERC_TWO_D10)) != known->mercurial.end();
	}
	if (twoD10) {
		// two half dice (2d10 for a d20): same top, a gentler curve
		int half = std::max(1, die / 2);
		natural = DCCDie(half) + DCCDie(half);
	} else {
		natural = DCCDie(die);
	}
	return die;
}

// ---------------------------------------------------------------------------
// the check

void Actor::DCCStampSpell(Spell& spl) const
{
	if (!dccCast.total) return;
	for (auto& header : spl.ext_headers) {
		for (Effect& feature : header.features) {
			feature.DCCSpellCheck = ieDword(dccCast.total);
			if (dccCast.durationSeconds >= 0 && feature.TimingMode == FX_DURATION_INSTANT_LIMITED && feature.Duration) {
				feature.Duration = ieDword(dccCast.durationSeconds);
			}
		}
	}
}

// the band's engine terms: DUR:dice[:unit] (the effects' duration, rolled once), REPEAT:dice (casts
// in all), SPREAD:feet (the repeats go round the target and foes near it), TARGETS:n:feet (n-1 more
// targets near the first), AREA:feet (every foe that near the target)
template<class CastState>
static void BandTerms(CastState& cast, Actor* caster, const std::string& effects, int casterLevel, int spellLevel)
{
	auto roller = [caster](int sides) { return caster->DCCDie(sides); };
	for (const auto& rawTerm : Split(effects, ',')) {
		std::string term = Substitute(rawTerm, spellLevel, casterLevel);
		auto parts = Split(term, ':');
		if (parts.empty()) continue;
		const std::string& key = parts[0];
		auto arg = [&parts](size_t i) { return i < parts.size() ? parts[i] : std::string(); };
		if (key == "DUR") {
			int amount = std::max(1, RollText(arg(1), roller));
			std::string unit = Upper(arg(2));
			Duration::Kind kind = Duration::Kind::Rounds;
			if (unit == "T") kind = Duration::Kind::Turns;
			if (unit == "H") kind = Duration::Kind::Hours;
			if (unit == "D") kind = Duration::Kind::Days;
			if (unit == "W") kind = Duration::Kind::Weeks;
			cast.durationSeconds = DurationSeconds(kind, amount, core->Time.round_sec);
			if (unit == "P") cast.durationSeconds = -1;
		} else if (key == "REPEAT") {
			cast.repeat = std::max(0, RollText(arg(1), roller) - 1);
		} else if (key == "SPREAD") {
			valid_signednumber(arg(1).c_str(), cast.radius);
			cast.targets = -1;
		} else if (key == "TARGETS") {
			int count = 1;
			valid_signednumber(arg(1).c_str(), count);
			cast.targets = std::max(0, count - 1);
			valid_signednumber(arg(2).c_str(), cast.radius);
			if (!cast.radius) cast.radius = 30;
		} else if (key == "AREA") {
			valid_signednumber(arg(1).c_str(), cast.radius);
			cast.area = true;
		}
	}
}

ResRef Actor::DCCCastCheck(const ResRef& base, Scriptable* target, const Point& targetPos)
{
	(void) targetPos;
	const RulesData* rules = Rules();
	const SpellInfo* info = rules ? rules->Spell(base.c_str()) : nullptr;
	if (!info) return base;
	std::string name = Lower(base.c_str());
	CasterKind kind = GetDCCCasterKind();

	// a cast that didn't start through CastSpell (an instant one): the policy decides now
	if (dccCast.spell != base || !dccCast.decided || dccCast.prompt) {
		if (dccCast.prompt && dccCast.spell == base) {
			core->GetPrompts().Cancel(dccCast.prompt);
			dccCast.prompt = 0;
		} else {
			DCCBeginCast(base, false);
		}
	}
	dccCast.total = 0;
	dccCast.durationSeconds = -1;
	dccCast.repeat = 0;
	dccCast.targets = 0;
	dccCast.radius = 0;
	dccCast.area = false;
	dccCast.band.Reset();

	DCCCastRecord record;
	record.spell = name;
	String why;
	if (!DCCCanCast(base, &why)) {
		Feedback(fmt::format(u"{} can't cast {}: {}", GetName(), Widen(info->name), why));
		record.notes.push_back(fmt::format("{}", fmt::WideToChar { why }));
		dccLastCast = record;
		return ResRef();
	}
	KnownSpell* known = dccExt.FindSpell(name);
	bool lost = known->flags & SPELL_LOST;
	int casterLevel = Signed(GetStat(IE_DCC_CASTERLVL));

	// spellburn (wizards and elves): points off Str, Agi, Sta as loss, +1 each (less the level for
	// a lost spell); 20 make it a natural 20
	int burnBonus = 0;
	int burned = 0;
	if (kind == CasterKind::Wizard) {
		std::array<int, 3> current { Signed(GetStat(IE_STR)), Signed(GetStat(IE_DEX)), Signed(GetStat(IE_CON)) };
		for (int i = 0; i < 3; ++i) {
			dccCast.burn[i] = Clamp(dccCast.burn[i], 0, std::max(0, current[i] - 1));
			burned += dccCast.burn[i];
		}
		if (lost && SpellburnBonus(burned, true, info->level) < 0) {
			Feedback(fmt::format(u"{} can't cast {}: it is lost for the day, and {} spellburn doesn't pay for it", GetName(), Widen(info->name), burned));
			record.notes.push_back("lost, not enough spellburn");
			dccLastCast = record;
			return ResRef();
		}
		if (burned > 0) {
			burnBonus = SpellburnBonus(burned, lost, info->level);
			String parts;
			for (int i = 0; i < 3; ++i) {
				if (!dccCast.burn[i]) continue;
				SetBase(LossStat(static_cast<Ability>(i)), BaseStats[LossStat(static_cast<Ability>(i))] + dccCast.burn[i]);
				dccExt.burn.points[i] = uint8_t(std::min(255, dccExt.burn.points[i] + dccCast.burn[i]));
				parts += fmt::format(u"{}{} {}", parts.empty() ? u"" : u", ", dccCast.burn[i], AbilityShort(i));
			}
			dccExt.burn.lastBurnDay = uint32_t(Today());
			dccExt.burn.flavour = uint8_t(DCCDie(24));
			const MagicEntry* how = rules->magic.spellburn.Lookup(dccExt.burn.flavour);
			Feedback(fmt::format(u"{} spellburns {} ({}{}): {}", GetName(), burned, parts, lost ? fmt::format(u", {} for the lost spell", info->level) : u"",
					     how ? Widen(how->text) : u""));
			RefreshEffects();
		}
	}
	record.burn = burned;
	record.burnBonus = burnBonus;

	// the roll
	int natural = 0;
	int die = DCCCheckDie(known, natural);
	int other = Signed(GetStat(IE_DCC_SPELLMOD)) + Signed(GetStat(IE_DCC_ROLLMOD)) + GetDCCAugurBonus(AugurRoll::SpellCheck);
	if (known->flags & SPELL_PENALTY) other -= 4;
	// mercurial magic before the roll counts: 97 drains the nearest creature, 72 casts in a row, 73 swings
	for (uint8_t entry : known->mercurial) {
		if (entry == MERC_DRAIN) {
			Map* map = GetCurrentArea();
			Actor* nearest = nullptr;
			if (map) {
				for (Actor* other : map->GetAllActorsInRadius(Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_SELF, 30, this)) {
					if (other == this) continue;
					if (!nearest || SquaredDistance(other->Pos, Pos) < SquaredDistance(nearest->Pos, Pos)) nearest = other;
				}
			}
			if (nearest) {
				int damage = 0;
				for (int i = 0; i < info->level; ++i) {
					damage += DCCDie(6);
				}
				auto fx = EffectQueue::CreateEffect(fx_dcc_magic_damage_ref, damage, 0, FX_DURATION_INSTANT_PERMANENT);
				fx->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(fx), nearest, this);
				other += damage / 2;
				record.notes.push_back(fmt::format("mercurial 97: drains {} for +{}", damage, damage / 2));
			}
		} else if (entry == MERC_STREAK) {
			int most = std::max({ 1, GetDCCAbilityMod(Ability::Int), GetDCCAbilityMod(Ability::Luck) });
			other += std::min<int>(known->streak, most);
		} else if (entry == MERC_SWING) {
			int swing = DCCDie(5);
			other += known->streak ? -swing : swing;
			known->streak = known->streak ? 0 : 1;
		}
	}

	SpellCheckInput in;
	in.natural = natural;
	in.die = die;
	in.casterLevel = casterLevel;
	in.abilityMod = GetDCCAbilityMod(kind == CasterKind::Cleric ? Ability::Per : Ability::Int);
	in.armorPenalty = Signed(GetStat(IE_DCC_ARMORPEN));
	in.spellburn = burnBonus;
	in.luck = (known->flags & SPELL_LUCKY) ? known->luckyMod : 0;
	in.other = other;
	in.cleric = kind == CasterKind::Cleric;
	in.disapprovalRange = dccExt.disapproval.range;
	in.cursed = GetDCCStates() & STATE_CURSED;
	in.autoCrit = burned >= SPELLBURN_AUTO_CRIT;
	SpellCheckResult result = ResolveSpellCheck(in);
	int bandIndex = info->BandFor(result);
	const SpellBand* band = bandIndex >= 0 ? &info->bands[size_t(bandIndex)] : nullptr;
	bool ally = target && target != this;
	bool success = band && (ally && !band->spell2.empty() ? band->success2 : band->success) && !result.disapproval && !result.cursed;

	// the log line: d20 14 +1 CL +1 Int ... = 17: 14-17, one missile
	String parts = fmt::format(u" +{} CL", casterLevel);
	if (in.abilityMod) parts += fmt::format(u" {:+} {}", in.abilityMod, kind == CasterKind::Cleric ? u"Per" : u"Int");
	if (in.armorPenalty && !in.cleric) parts += fmt::format(u" {:+} armor", in.armorPenalty);
	if (burnBonus) parts += fmt::format(u" {:+} spellburn", burnBonus);
	if (in.luck) parts += fmt::format(u" {:+} Luck", in.luck);
	if (other) parts += fmt::format(u" {:+}", other);
	if (result.critical) parts += fmt::format(u" +{} critical", casterLevel);
	String outcome;
	if (result.cursed) {
		outcome = u"fails (cursed by fortune)";
	} else if (result.disapproval) {
		outcome = fmt::format(u"natural {} in the disapproval range (1-{}): fails", result.natural, dccExt.disapproval.range);
	} else if (band) {
		String text = Widen(ally && !band->text2.empty() ? band->text2 : band->text);
		int max = bandIndex + 1 < int(info->bands.size()) ? info->bands[size_t(bandIndex) + 1].min - 1 : 0;
		String range = band->natOne ? String(u"1") : (max >= band->min ? fmt::format(u"{}-{}", band->min, max) : fmt::format(u"{}+", band->min));
		outcome = fmt::format(u"{}: {}", range, text);
		if (band->lost && kind == CasterKind::Wizard) outcome += u" (lost)";
	}
	String line = fmt::format(u"{} casts {}: d{} {}{} = {}: {}", GetName(), Widen(info->name), die, natural, parts, result.total, outcome);
	Feedback(std::move(line));

	record.die = die;
	record.natural = natural;
	record.total = result.total;
	record.bandIndex = bandIndex;
	record.bandMin = band ? band->min : 0;
	record.success = success;
	record.critical = result.critical;
	record.natOne = result.natOne;
	record.disapproval = result.disapproval;
	record.cursed = result.cursed;
	if (known->casts < 0xffff) ++known->casts;
	if (!std::count(known->mercurial.begin(), known->mercurial.end(), uint8_t(MERC_SWING))) {
		known->streak = uint8_t(std::min(255, known->streak + 1));
	}
	dccLastCast = record;

	// what the result brings
	if (kind == CasterKind::Wizard) {
		if (band && band->lost && !(known->flags & SPELL_LOST)) {
			known->flags |= SPELL_LOST;
			dccLastCast.lost = true;
		}
		if (result.natOne) {
			DCCNatOne(*info, name, burned > 0);
		} else if (burned == 0 && std::count(known->mercurial.begin(), known->mercurial.end(), uint8_t(MERC_ALWAYS_CORRUPT))) {
			// mercurial 9: corruption on every cast without spellburn
			Feedback(fmt::format(u"{}'s {} corrupts it (mercurial magic)", GetName(), Widen(info->name)));
			DCCCorrupt(CorruptionTable::Minor, info->level, name);
		}
	} else if (kind == CasterKind::Cleric) {
		if (result.disapproval) {
			DCCDisapproval(result.natural, name);
		}
		if (!success) {
			int before = dccExt.disapproval.range;
			dccExt.disapproval.range = uint8_t(DisapprovalWidened(before));
			Feedback(fmt::format(u"{}'s god is displeased: the disapproval range is now 1-{}", GetName(), int(dccExt.disapproval.range)));
		}
		dccLastCast.rangeAfter = dccExt.disapproval.range;
	}

	if (success && band) {
		dccCast.total = result.total;
		dccCast.level = info->level;
		dccCast.band = ResRef(ally && !band->spell2.empty() ? band->spell2 : band->spell);
		BandTerms(dccCast, this, ally && !band->spell2.empty() ? band->effects2 : band->effects, casterLevel, info->level);
		dccLastCast.band = dccCast.band.c_str();
		dccLastCast.repeat = dccCast.repeat;
	}
	if (kind == CasterKind::Wizard) DCCMercurialAfter(*info, known, success);
	DCCExtChanged();
	return success ? dccCast.band : ResRef();
}

void Actor::DCCCastExtras(Scriptable* target, int level)
{
	if (dccCast.band.IsEmpty()) return;
	Map* map = GetCurrentArea();
	if (!map) return;
	ResRef band = dccCast.band;
	std::vector<Scriptable*> targets;
	Actor* first = Scriptable::As<Actor>(target);
	if (first && (dccCast.targets != 0 || dccCast.area)) {
		// foes of the caster near the first target, nearest first (Ethren decision)
		bool good = GetStat(IE_EA) <= EA_GOODCUTOFF;
		std::vector<Actor*> near;
		for (Actor* other : map->GetAllActorsInRadius(first->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED, unsigned(std::max(5, dccCast.radius)))) {
			if (other == first || other == this) continue;
			if ((other->GetStat(IE_EA) <= EA_GOODCUTOFF) == good) continue;
			near.push_back(other);
		}
		std::sort(near.begin(), near.end(), [first](const Actor* a, const Actor* b) {
			return SquaredDistance(a->Pos, first->Pos) < SquaredDistance(b->Pos, first->Pos);
		});
		if (dccCast.targets > 0 && int(near.size()) > dccCast.targets) near.resize(size_t(dccCast.targets));
		targets.assign(near.begin(), near.end());
	}
	dccLastCast.targets = 1 + (dccCast.targets < 0 ? 0 : int(targets.size()));
	int repeat = dccCast.repeat;
	dccCast.repeat = 0;
	if (dccCast.targets >= 0) {
		// more targets: the band once on each
		for (Scriptable* other : targets) {
			DirectlyCastSpell(other, band, level, true, false);
		}
	}
	// the repeats (more missiles): on the target, or round the target and the foes near it
	std::vector<Scriptable*> cycle { target };
	if (dccCast.targets < 0) {
		cycle.insert(cycle.end(), targets.begin(), targets.end());
		dccLastCast.targets = int(cycle.size());
	}
	for (int i = 0; i < repeat && target; ++i) {
		Scriptable* who = cycle[size_t(i + 1) % cycle.size()];
		DirectlyCastSpell(who, band, level, true, false);
	}
}

void Actor::DCCEndCast(Scriptable* target, int level)
{
	if (target) DCCCastExtras(target, level);
	dccCast.total = 0;
	dccCast.band.Reset();
	dccCast.durationSeconds = -1;
	dccCast.spell.Reset();
	dccCast.decided = false;
}

// ---------------------------------------------------------------------------
// the natural 1, corruption, misfire

void Actor::DCCNatOne(const SpellInfo& info, const std::string& spell, bool burning)
{
	const RulesData* rules = Rules();
	int luck = GetDCCAbilityMod(Ability::Luck);
	int d6 = DCCDie(6);
	int sub = d6 + luck;
	uint32_t outcome = NatOneFor(info.natOne, sub);
	dccLastCast.natOneOutcome = outcome;
	String what;
	if (outcome & (NAT1_CORRUPT | NAT1_TAINT)) what += u"corruption";
	if (outcome & NAT1_TAINT) what += u" (patron taint: no patron, so corruption)";
	if (outcome & NAT1_MISFIRE) what += fmt::format(u"{}misfire", what.empty() ? u"" : u" and ");
	Feedback(fmt::format(u"{} rolls a natural 1 on {}: d6 {}{} = {}: {}", GetName(), Widen(info.name), d6, Plus(luck), sub, what));

	// spellburning on a natural 1: a point of the burned abilities goes for good (magic.md §6)
	if (burning) {
		int ability = 0;
		for (int i = 1; i < 3; ++i) {
			if (dccCast.burn[i] > dccCast.burn[ability]) ability = i;
		}
		unsigned int stat = AbilityStat(static_cast<Ability>(ability));
		SetBase(stat, std::max(1, Signed(BaseStats[stat]) - 1));
		Feedback(fmt::format(u"{} loses 1 {} for good to the spellburn", GetName(), AbilityShort(ability)));
	}

	int corruptions = ((outcome & NAT1_CORRUPT) ? 1 : 0) + ((outcome & NAT1_TAINT) ? 1 : 0);
	for (int i = 0; i < corruptions; ++i) {
		if (info.corruption.empty()) {
			DCCCorrupt(CorruptionTable::Minor, info.level, spell);
			continue;
		}
		int roll = DCCDie(info.corruption.back().roll);
		const SpellEntry* entry = EntryFor(info.corruption, roll);
		if (!entry) continue;
		std::string effects = Upper(entry->effects);
		if (effects.find("GREATER") != std::string::npos) {
			DCCCorrupt(CorruptionTable::Greater, info.level, spell);
		} else if (effects.find("MAJOR") != std::string::npos) {
			DCCCorrupt(CorruptionTable::Major, info.level, spell);
		} else if (effects.find("MINOR") != std::string::npos) {
			DCCCorrupt(CorruptionTable::Minor, info.level, spell);
		} else {
			Corruption corruption;
			corruption.table = CorruptionTable::Spell;
			corruption.row = uint8_t(entry->roll);
			corruption.spell = spell;
			dccLastCast.corruptionTable = 0;
			dccLastCast.corruptionRow = entry->roll;
			if (DCCOfferCorruptionBurn(corruption, info.level, entry->text)) dccLastCast.corruptionCancelled = true;
		}
	}
	if (outcome & NAT1_MISFIRE) {
		if (info.misfire.empty()) {
			int roll = DCCDie(8);
			const MagicEntry* entry = rules->magic.misfire.Lookup(roll);
			dccLastCast.misfireRoll = roll;
			if (entry) {
				String notes = DCCMagicEffects(entry->effects, this, info.level, entry->subDie ? DCCDie(entry->subDie) : 0);
				Feedback(fmt::format(u"{}'s spell misfires (Table 5-6, {}): {}{}", GetName(), roll, Widen(entry->text), notes.empty() ? u"" : u"; " + notes));
			}
		} else {
			int roll = DCCDie(info.misfire.back().roll);
			const SpellEntry* entry = EntryFor(info.misfire, roll);
			dccLastCast.misfireRoll = roll;
			if (entry) {
				String notes = DCCMagicEffects(entry->effects, this, info.level);
				Feedback(fmt::format(u"{}'s {} misfires ({}): {}{}", GetName(), Widen(info.name), roll, Widen(entry->text), notes.empty() ? u"" : u"; " + notes));
			}
		}
	}
}

int Actor::DCCCorrupt(CorruptionTable table, int spellLevel, const std::string& spell, int roll)
{
	const RulesData* rules = Rules();
	if (!rules || table == CorruptionTable::Spell) return 0;
	const MagicTableList& list = rules->magic.corruption[size_t(table) - 1];
	if (!roll) {
		int d10 = DCCDie(10);
		int luck = GetDCCAbilityMod(Ability::Luck) + GetDCCAugurBonus(AugurRoll::Corruption);
		roll = CorruptionRoll(d10, spellLevel, luck);
	}
	const MagicEntry* entry = list.Lookup(roll);
	if (!entry) return 0;
	Corruption corruption;
	corruption.table = table;
	corruption.row = uint8_t(Clamp(roll, 1, 10));
	corruption.sub = entry->subDie ? uint8_t(DCCDie(entry->subDie)) : 0;
	corruption.spell = spell;
	dccLastCast.corruptionTable = int(table);
	dccLastCast.corruptionRow = corruption.row;
	if (DCCOfferCorruptionBurn(corruption, spellLevel, entry->text)) dccLastCast.corruptionCancelled = true;
	return corruption.row;
}

static const char16_t* TableName(CorruptionTable table)
{
	switch (table) {
		case CorruptionTable::Minor:
			return u"minor corruption";
		case CorruptionTable::Major:
			return u"major corruption";
		case CorruptionTable::Greater:
			return u"greater corruption";
		default:
			return u"corruption";
	}
}

bool Actor::DCCOfferCorruptionBurn(const Corruption& corruption, int spellLevel, const std::string& text)
{
	int luck = GetDCCLuck();
	CorruptionBurn policy = dccExt.burn.corruption;
	// the policy's answer: the spell's own corruptions count as minor
	bool wants = false;
	switch (policy) {
		case CorruptionBurn::Always:
			wants = true;
			break;
		case CorruptionBurn::Major:
			wants = corruption.table == CorruptionTable::Major || corruption.table == CorruptionTable::Greater;
			break;
		case CorruptionBurn::Greater:
			wants = corruption.table == CorruptionTable::Greater;
			break;
		default:
			break;
	}
	// it never burns its last point by itself (0 is cursed by fortune)
	if (luck <= 1) wants = false;
	String what = fmt::format(u"{} ({})", Widen(text), TableName(corruption.table));
	Feedback(fmt::format(u"{} is corrupted: {}", GetName(), what));

	LuckPolicy luckPolicy = GetDCCLuckPolicy();
	const NetSession* net = core->GetNetSession();
	bool ask = InParty && luck >= 1 && (luckPolicy.flags & LUCK_PAUSE) && !(net && !net->IsHost()) && core->GetGameControl();
	if (ask) {
		PromptQuestion q;
		q.kind = PromptKind::YesNo;
		q.tag = "corruption";
		q.title = "Burn Luck?";
		q.text = fmt::format("{} is corrupted: {}. Burn 1 Luck for good to avoid it ({} left)?", fmt::WideToChar { GetShortName() }, fmt::WideToChar { what }, luck);
		q.defaultChoice = wants ? 1 : 0;
		q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));
		ieDword selfID = GetGlobalID();
		Corruption pending = corruption;
		uint32_t id = core->GetPrompts().Ask(this, std::move(q), [selfID, pending, spellLevel](const PromptResult& r) {
			Game* game = core->GetGame();
			Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
			if (!self) return;
			if (r.Yes() && self->GetDCCLuck() >= 1) {
				self->DCCBurnLuck(1, BurnValue());
				Feedback(fmt::format(u"{} burns 1 Luck and avoids the corruption (Luck {} left)", self->GetName(), self->GetDCCLuck()));
				self->dccLastCast.corruptionCancelled = true;
				return;
			}
			self->DCCAddCorruption(pending, spellLevel);
		});
		if (id) return false;
	}
	if (wants) {
		DCCBurnLuck(1, BurnValue());
		Feedback(fmt::format(u"{} burns 1 Luck and avoids the corruption (Luck {} left)", GetName(), GetDCCLuck()));
		return true;
	}
	DCCAddCorruption(corruption, spellLevel);
	return false;
}

void Actor::DCCAddCorruption(const Corruption& corruption, int spellLevel)
{
	const RulesData* rules = Rules();
	if (!rules) return;
	std::string effects;
	std::string text;
	if (corruption.table == CorruptionTable::Spell) {
		const SpellInfo* info = rules->Spell(corruption.spell);
		const SpellEntry* entry = info ? EntryFor(info->corruption, corruption.row) : nullptr;
		if (entry) {
			effects = entry->effects;
			text = entry->text;
		}
	} else {
		const MagicEntry* entry = rules->magic.corruption[size_t(corruption.table) - 1].Lookup(corruption.row);
		if (entry) {
			effects = entry->effects;
			text = entry->text;
		}
	}
	dccExt.corruptions.push_back(corruption);
	String notes = DCCMagicEffects(effects, this, spellLevel, corruption.sub);
	DCCExtChanged();
	RefreshEffects();
	Feedback(fmt::format(u"{} now has {}{}", GetName(), Widen(text), notes.empty() ? u"" : u": " + notes));
}

// ---------------------------------------------------------------------------
// disapproval, mercurial magic after the cast

void Actor::DCCDisapproval(int natural, const std::string& spell)
{
	const RulesData* rules = Rules();
	int sum = 0;
	for (int i = 0; i < std::max(1, natural); ++i) {
		sum += DCCDie(4);
	}
	int luck = GetDCCAbilityMod(Ability::Luck);
	int roll = DisapprovalRoll(sum, luck);
	dccLastCast.disapprovalRoll = roll;
	const MagicEntry* entry = rules->magic.disapproval.Lookup(roll);
	String notes;
	if (entry) {
		// Table 5-7 8: -4 to checks with this spell (or ability) until the next rest
		if (entry->effects.find("SPELLPEN") != std::string::npos) {
			KnownSpell* known = dccExt.FindSpell(spell);
			if (known) known->flags |= SPELL_PENALTY;
		}
		notes = DCCMagicEffects(entry->effects, this, 1);
	}
	Feedback(fmt::format(u"{} rolls on the disapproval table: {}d4 {}{} = {}: {}{}", GetName(), std::max(1, natural), sum, Plus(-luck), roll,
			     entry ? Widen(entry->text) : u"?", notes.empty() ? u"" : u"; " + notes));
}

void Actor::DCCMercurialAfter(const SpellInfo& info, KnownSpell* known, bool success)
{
	const RulesData* rules = Rules();
	if (!known || known->mercurial.empty()) return;
	for (uint8_t entry : known->mercurial) {
		const MagicEntry* row = rules->magic.mercurial.Lookup(entry);
		if (!row || row->effects.empty()) continue;
		// the WHEN column isn't kept: SUCCESS entries (84, 91, 95) only fire on a success
		if (!success && (entry == 84 || entry == MERC_HEAL_AREA || entry == MERC_SPELL_BOOST)) continue;
		std::string save;
		if (entry == MERC_SLEEP_AFTER) save = fmt::format("FORT:{}", 5 + info.level);
		if (entry == 30) save = "WILL:13";
		String notes = DCCMagicEffects(row->effects, this, info.level, known->mercurialSub, save);
		Feedback(fmt::format(u"{}'s mercurial magic ({}): {}{}", GetName(), int(entry), Widen(row->text), notes.empty() ? u"" : u"; " + notes));
	}
}

// ---------------------------------------------------------------------------
// the magic effect terms

// who a term happens to: ^SELF (the subject), ^NEAREST (the nearest other creature), ^ALLY (the
// subject's nearest ally), ^NEARESTFOE<feet>, ^AREA<feet> (everyone that near, the subject too),
// ^OTHERS<feet> (everyone else that near), ^RANDOM<n> (one of the n nearest others)
static std::vector<Actor*> TermSubjects(Actor* subject, const std::string& who)
{
	std::vector<Actor*> result;
	if (who.empty() || who == "SELF") {
		result.push_back(subject);
		return result;
	}
	Map* map = subject->GetCurrentArea();
	if (!map) return result;
	auto number = [&who](size_t from, int fallback) {
		int value = fallback;
		if (who.size() > from) valid_signednumber(who.substr(from).c_str(), value);
		return value;
	};
	bool good = subject->GetStat(IE_EA) <= EA_GOODCUTOFF;
	std::vector<Actor*> near;
	int radius = 100;
	if (who.compare(0, 4, "AREA") == 0) radius = number(4, 10);
	if (who.compare(0, 6, "OTHERS") == 0) radius = number(6, 10);
	if (who.compare(0, 10, "NEARESTFOE") == 0) radius = number(10, 10);
	for (Actor* other : map->GetAllActorsInRadius(subject->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, unsigned(radius))) {
		if (other == subject) continue;
		near.push_back(other);
	}
	std::sort(near.begin(), near.end(), [subject](const Actor* a, const Actor* b) {
		return SquaredDistance(a->Pos, subject->Pos) < SquaredDistance(b->Pos, subject->Pos);
	});
	if (who.compare(0, 4, "AREA") == 0) {
		result.push_back(subject);
		result.insert(result.end(), near.begin(), near.end());
	} else if (who.compare(0, 6, "OTHERS") == 0) {
		result = near;
	} else if (who == "NEAREST") {
		if (!near.empty()) result.push_back(near.front());
	} else if (who == "ALLY") {
		for (Actor* other : near) {
			if ((other->GetStat(IE_EA) <= EA_GOODCUTOFF) == good) {
				result.push_back(other);
				break;
			}
		}
	} else if (who.compare(0, 10, "NEARESTFOE") == 0) {
		for (Actor* other : near) {
			if ((other->GetStat(IE_EA) <= EA_GOODCUTOFF) != good) {
				result.push_back(other);
				break;
			}
		}
	} else if (who.compare(0, 6, "RANDOM") == 0) {
		size_t count = size_t(std::max(1, number(6, 6)));
		if (near.size() > count) near.resize(count);
		if (!near.empty()) result.push_back(near[size_t(subject->DCCDie(int(near.size())) - 1)]);
	}
	return result;
}

// a DCC:Magic effect for a table duration (PERM, HEAL = until the next rest, rounds, units)
static String ApplyMagic(MagicEffect kind, int value, const std::string& durationText, Actor* who, Actor* caster, ieDword mark = 0)
{
	Duration duration;
	if (!ParseDuration(durationText, duration)) {
		duration = Duration();
		duration.dice.add = 1;
	}
	ieDword p2 = ieDword(kind);
	std::unique_ptr<Effect> fx;
	String what;
	if (duration.kind == Duration::Kind::Permanent || duration.kind == Duration::Kind::UntilHealed) {
		p2 |= duration.kind == Duration::Kind::Permanent ? COMBAT_FOR_GOOD : COMBAT_UNTIL_HEALED;
		fx = EffectQueue::CreateEffect(fx_dcc_magic_ref, ieDword(value), p2, FX_DURATION_INSTANT_PERMANENT_AFTER_BONUSES);
		what = duration.kind == Duration::Kind::Permanent ? u"for good" : u"until the next rest";
	} else {
		int amount = std::max(1, RollDice(duration.dice, [caster](int sides) { return caster->DCCDie(sides); }));
		fx = EffectQueue::CreateEffect(fx_dcc_magic_ref, ieDword(value), p2, FX_DURATION_INSTANT_LIMITED);
		fx->Duration = DurationSeconds(duration.kind, amount, core->Time.round_sec);
		static const char16_t* units[] = { u"round", u"turn", u"hour", u"day", u"week" };
		what = fmt::format(u"for {} {}{}", amount, units[static_cast<int>(duration.kind)], amount == 1 ? u"" : u"s");
	}
	if (!fx) return what;
	fx->Parameter3 = mark;
	fx->Target = FX_TARGET_PRESET;
	core->ApplyEffect(std::move(fx), who, caster);
	return what;
}

static int AbilityKey(const std::string& key)
{
	static const char* keys[] = { "STR", "AGI", "STA", "PER", "INT", "LCK" };
	for (int i = 0; i < 6; ++i) {
		if (key == keys[i]) return i;
	}
	return -1;
}

String Actor::DCCMagicEffects(const std::string& effects, Actor* subject, int spellLevel, int sub, const std::string& save)
{
	if (!subject || effects.empty() || effects == "*") return u"";
	int casterLevel = std::max(1, Signed(GetStat(IE_DCC_CASTERLVL)));
	auto roller = [this](int sides) { return DCCDie(sides); };
	std::vector<String> notes;
	int saved = -1; // the subject's save against a SAVE given: -1 not rolled, 0 failed, 1 passed
	std::string saveText = save;
	for (std::string term : Split(effects, ',')) {
		if (term.empty() || term == "*") continue;
		// N#: only on the sub-roll N
		auto hash = term.find('#');
		if (hash != std::string::npos && hash > 0 && hash < 3) {
			int only = 0;
			if (!valid_signednumber(term.substr(0, hash).c_str(), only) || only != sub) continue;
			term = term.substr(hash + 1);
		}
		// ^WHO
		std::string who;
		auto caret = term.find('^');
		if (caret != std::string::npos) {
			who = term.substr(caret + 1);
			term = term.substr(0, caret);
		}
		// ?: only if the subject fails the save
		bool onFail = !term.empty() && term[0] == '?';
		if (onFail) term = term.substr(1);
		auto parts = Split(term, ':');
		std::string key = parts.empty() ? std::string() : parts[0];
		for (size_t i = 1; i < parts.size(); ++i) {
			if (key != "TODO") parts[i] = Substitute(parts[i], spellLevel, casterLevel);
		}
		auto arg = [&parts](size_t i) { return i < parts.size() ? parts[i] : std::string(); };

		for (Actor* who2 : TermSubjects(subject, who)) {
			if (onFail && !saveText.empty()) {
				if (saved < 0) {
					auto spec = Split(saveText, ':');
					int dc = 10;
					if (spec.size() > 1) valid_signednumber(spec[1].c_str(), dc);
					Save kind = spec[0] == "FORT" ? Save::Fort : (spec[0] == "REF" ? Save::Ref : Save::Will);
					RollResult roll = who2->DCCRollSave(kind, 0, dc, who2->DCCDie(20));
					saved = roll.success ? 1 : 0;
					notes.push_back(fmt::format(u"{} {} save {} vs DC {}: {}", who2->GetName(), Widen(spec[0]), roll.total, dc, roll.success ? u"saved" : u"failed"));
				}
				if (saved == 1) continue;
			}
			if (key == "MUT") {
				// the lasting look comes from the corruption list; a timed one is an effect
				if (parts.size() > 2) {
					uint32_t bit = MutationBit(arg(1));
					if (bit) notes.push_back(fmt::format(u"{} {}", Widen(MutationName(bit)), ApplyMagic(MagicEffect::Mutation, int(bit), arg(2), who2, this)));
				}
			} else if (key == "SPELLMOD") {
				int value = 0;
				valid_signednumber(arg(1).c_str(), value);
				ieDword mark = arg(3) == "MEDITATE" ? MARK_MEDITATE : (arg(3) == "TITHE" ? MARK_TITHE : 0);
				notes.push_back(fmt::format(u"{:+} to spell checks {}", value, ApplyMagic(MagicEffect::SpellMod, value, arg(2), who2, this, mark)));
			} else if (key == "LOHMOD") {
				int value = 0;
				valid_signednumber(arg(1).c_str(), value);
				notes.push_back(fmt::format(u"{:+} to lay on hands {}", value, ApplyMagic(MagicEffect::LayOnHandsMod, value, arg(2), who2, this)));
			} else if (key == "BLOCK") {
				std::string what = arg(1);
				int bit = what == "SPELLS" ? BLOCK_SPELLS : (what == "LOH" ? BLOCK_LAY_ON_HANDS : (what == "TURN" ? BLOCK_TURN : (what == "LOHONCE" ? BLOCK_LOH_ONCE : 0)));
				static const std::map<int, const char16_t*> names { { BLOCK_SPELLS, u"no spells" }, { BLOCK_LAY_ON_HANDS, u"no lay on hands" }, { BLOCK_TURN, u"no turning" }, { BLOCK_LOH_ONCE, u"lay on hands once a creature" } };
				if (bit) notes.push_back(fmt::format(u"{} {}", names.at(bit), ApplyMagic(MagicEffect::Block, bit, arg(2), who2, this)));
			} else if (key == "NORESET") {
				who2->dccExt.disapproval.flags |= DISAPPROVAL_NO_RESET;
				who2->DCCExtChanged();
				notes.push_back(u"the range won't reset at the next rest");
			} else if (key == "SPELLPEN") {
				notes.push_back(u"-4 to that spell until the next rest");
			} else if (key == "LOSESPELLS") {
				// random known spells (of a level) are lost until the next rest
				int count = std::max(1, RollText(arg(1), roller));
				int level = 0;
				valid_signednumber(arg(2).c_str(), level);
				std::vector<KnownSpell*> pool;
				const RulesData* rules = Rules();
				for (auto& known : who2->dccExt.spells) {
					const SpellInfo* info = rules->Spell(known.spell);
					if (!info || (known.flags & SPELL_LOST)) continue;
					if (level && info->level != level) continue;
					pool.push_back(&known);
				}
				int lostCount = 0;
				while (count-- > 0 && !pool.empty()) {
					size_t pick = size_t(DCCDie(int(pool.size())) - 1);
					pool[pick]->flags |= SPELL_LOST;
					pool.erase(pool.begin() + std::ptrdiff_t(pick));
					++lostCount;
				}
				who2->DCCExtChanged();
				notes.push_back(fmt::format(u"loses {} spell{} until the next rest", lostCount, lostCount == 1 ? u"" : u"s"));
			} else if (key == "MINOR" || key == "MAJOR" || key == "GREATER") {
				CorruptionTable table = key == "MINOR" ? CorruptionTable::Minor : (key == "MAJOR" ? CorruptionTable::Major : CorruptionTable::Greater);
				who2->DCCCorrupt(table, spellLevel, dccLastCast.spell);
			} else if (key == "MINORROLL") {
				// a fixed roll on Table 5-3 for someone else (generic misfire 6), no Luck offer
				const RulesData* rules = Rules();
				int roll = Clamp(RollText(arg(1), roller), 1, 10);
				const MagicEntry* entry = rules->magic.corruption[0].Lookup(roll);
				Corruption corruption;
				corruption.table = CorruptionTable::Minor;
				corruption.row = uint8_t(roll);
				corruption.flags = CORR_FROM_MISFIRE;
				corruption.spell = dccLastCast.spell;
				if (entry && entry->subDie) corruption.sub = uint8_t(DCCDie(entry->subDie));
				who2->DCCAddCorruption(corruption, spellLevel);
				notes.push_back(fmt::format(u"{} is corrupted ({})", who2->GetName(), entry ? Widen(entry->text) : u"?"));
			} else if (key == "DMG") {
				int damage = std::max(0, RollText(arg(1), roller));
				if (damage) {
					auto fx = EffectQueue::CreateEffect(fx_dcc_magic_damage_ref, ieDword(damage), 0, FX_DURATION_INSTANT_PERMANENT);
					fx->Target = FX_TARGET_PRESET;
					core->ApplyEffect(std::move(fx), who2, this);
				}
				notes.push_back(fmt::format(u"{} takes {} damage", who2->GetName(), damage));
			} else if (key == "HEAL") {
				int heal = std::max(0, RollText(arg(1), roller));
				auto fx = EffectQueue::CreateEffect(fx_dcc_heal_ref, ieDword(heal), 0, FX_DURATION_INSTANT_PERMANENT);
				fx->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(fx), who2, this);
				notes.push_back(fmt::format(u"{} heals {}", who2->GetName(), heal));
			} else if (key == "ABIL") {
				// ABIL:<ability>:<points>[:PERM|duration]; negative points raise the score for good
				int ability = AbilityKey(arg(1));
				if (ability < 0) continue;
				int points = RollText(arg(2), roller);
				if (arg(3) == "PERM" || points < 0) {
					unsigned int stat = AbilityStat(static_cast<Ability>(ability));
					who2->SetBase(stat, std::max(0, Signed(who2->GetBase(stat)) - points));
					notes.push_back(fmt::format(u"{:+} {} for good", -points, AbilityShort(ability)));
				} else if (!arg(3).empty()) {
					notes.push_back(DCCApplyTableEffects(fmt::format("ABIL:{}:{}:{}", arg(1), points, arg(3)), who2));
				} else {
					auto fx = EffectQueue::CreateEffect(fx_dcc_loss_magic_ref, ieDword(points), ieDword(ability), FX_DURATION_INSTANT_PERMANENT);
					fx->Target = FX_TARGET_PRESET;
					core->ApplyEffect(std::move(fx), who2, this);
					notes.push_back(fmt::format(u"-{} {}", points, AbilityShort(ability)));
				}
			} else if (key == "TODO") {
				std::string what = arg(1);
				std::replace(what.begin(), what.end(), '_', ' ');
				notes.push_back(u"(not in the engine yet: " + Widen(what) + u")");
				Log(MESSAGE, "DCC", "Not in the engine yet: {}", what);
			} else if (!key.empty()) {
				// the crit table grammar: UNCON, MUTE, ROLLMOD, AC, STUN, SLEEP, ...
				std::string rebuilt = key;
				for (size_t i = 1; i < parts.size(); ++i) {
					rebuilt += ":" + parts[i];
				}
				String note = DCCApplyTableEffects(rebuilt, who2, "", casterLevel);
				if (!note.empty()) notes.push_back(note);
			}
		}
	}
	String joined;
	for (const auto& note : notes) {
		if (note.empty()) continue;
		joined += (joined.empty() ? u"" : u", ") + note;
	}
	return joined;
}

// ---------------------------------------------------------------------------
// rest, settings

void Actor::DCCRestMagic()
{
	bool changed = false;
	for (auto& known : dccExt.spells) {
		if (known.flags & (SPELL_LOST | SPELL_PENALTY)) {
			known.flags &= uint8_t(~(SPELL_LOST | SPELL_PENALTY));
			changed = true;
		}
	}
	// clerics regain their spells: the range resets unless Table 5-7 15 holds it a day
	if (dccExt.disapproval.flags & DISAPPROVAL_NO_RESET) {
		dccExt.disapproval.flags &= uint8_t(~DISAPPROVAL_NO_RESET);
		changed = true;
	} else if (dccExt.disapproval.range > 1) {
		dccExt.disapproval.range = 1;
		changed = true;
	}
	// spellburn comes back 1 point a day on which there was no spellburn (Ethren decision: in all,
	// not per ability; a night's rest is the day)
	int index = SpellburnRecoveryIndex(dccExt.burn.points);
	if (index >= 0 && dccExt.burn.lastBurnDay != uint32_t(Today())) {
		--dccExt.burn.points[size_t(index)];
		unsigned int loss = LossStat(static_cast<Ability>(index));
		if (BaseStats[loss] > 0) SetBase(loss, BaseStats[loss] - 1);
		Feedback(fmt::format(u"{} recovers 1 {} of spellburn ({} still burned)", GetName(), AbilityShort(index), dccExt.burn.Total()));
		changed = true;
	}
	dccExt.burn.lastBurnDay = 0;
	dccLaidHands.clear();
	// "until tomorrow": the DCC:Magic effects until the next rest; Table 5-7 11 ends on a Will save DC 15
	for (ieDword kind = 0; kind < ieDword(MagicEffect::count); ++kind) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_magic_ref, kind | COMBAT_UNTIL_HEALED);
	}
	auto iter = fxqueue.GetFirstEffect();
	const Effect* fx = fxqueue.GetNextEffect(iter);
	bool meditating = false;
	while (fx) {
		if (fx->Opcode == ieDword(EffectQueue::ResolveEffect(fx_dcc_magic_ref)) && fx->Parameter3 == MARK_MEDITATE) meditating = true;
		fx = fxqueue.GetNextEffect(iter);
	}
	if (meditating) {
		RollResult roll = DCCRollSave(Save::Will, 0, 15, DCCDie(20));
		Feedback(fmt::format(u"{} meditates on its god's displeasure: Will save {} vs DC 15: {}", GetName(), roll.total, roll.success ? u"forgiven" : u"not yet"));
		if (roll.success) {
			auto it = fxqueue.GetFirstEffect();
			Effect* each;
			while ((each = const_cast<Effect*>(fxqueue.GetNextEffect(it)))) {
				if (each->Opcode == ieDword(EffectQueue::ResolveEffect(fx_dcc_magic_ref)) && each->Parameter3 == MARK_MEDITATE) {
					each->TimingMode = FX_DURATION_JUST_EXPIRED;
				}
			}
		}
	}
	if (changed) DCCExtChanged();
}

void Actor::SetDCCSpellburn(int policy, int preset, int corruption)
{
	if (policy >= 0) dccExt.burn.policy = SpellburnPolicy(std::min(policy, int(SpellburnPolicy::count) - 1));
	if (preset >= 0) dccExt.burn.preset = uint8_t(std::min(preset, 60));
	if (corruption >= 0) dccExt.burn.corruption = CorruptionBurn(std::min(corruption, int(CorruptionBurn::count) - 1));
	DCCExtChanged();
}

// ---------------------------------------------------------------------------
// cleric: lay on hands, turn unholy, atonement

// a cleric's check (lay on hands, turning): d20 + Per + CL + extra, with the disapproval range
// and its widening on a failure; returns the result (natural forced if > 0)
static SpellCheckResult ClericCheck(Actor* cleric, int natural, int extra, int luck, const char16_t* what, int successFrom, bool& success)
{
	SpellCheckInput in;
	in.die = Signed(cleric->GetStat(IE_DCC_ACTDIE1));
	if (in.die <= 0) in.die = 20;
	in.natural = natural > 0 ? natural : cleric->DCCDie(in.die);
	in.casterLevel = Signed(cleric->GetStat(IE_DCC_CASTERLVL));
	in.abilityMod = cleric->GetDCCAbilityMod(Ability::Per);
	in.luck = luck;
	in.other = extra + Signed(cleric->GetStat(IE_DCC_ROLLMOD));
	in.cleric = true;
	in.disapprovalRange = cleric->dccExt.disapproval.range;
	in.cursed = cleric->GetDCCStates() & STATE_CURSED;
	SpellCheckResult result = ResolveSpellCheck(in);
	success = !result.disapproval && !result.cursed && result.total >= successFrom;
	String parts = fmt::format(u" +{} CL", in.casterLevel);
	if (in.abilityMod) parts += fmt::format(u" {:+} Per", in.abilityMod);
	if (luck) parts += fmt::format(u" {:+} Luck", luck);
	if (in.other) parts += fmt::format(u" {:+}", in.other);
	if (result.critical) parts += fmt::format(u" +{} critical", in.casterLevel);
	String outcome = result.disapproval ? fmt::format(u"natural {} in the disapproval range: fails", result.natural)
					    : (success ? String(u"success") : String(u"fails"));
	Feedback(fmt::format(u"{} {}: d{} {}{} = {}: {}", cleric->GetName(), what, in.die, result.natural, parts, result.total, outcome));
	if (result.disapproval) cleric->DCCDisapprovalPublic(result.natural);
	if (!success) {
		cleric->dccExt.disapproval.range = uint8_t(DisapprovalWidened(cleric->dccExt.disapproval.range));
		Feedback(fmt::format(u"{}'s god is displeased: the disapproval range is now 1-{}", cleric->GetName(), int(cleric->dccExt.disapproval.range)));
		cleric->DCCExtChanged();
	}
	return result;
}

void Actor::DCCDisapprovalPublic(int natural)
{
	DCCDisapproval(natural, std::string());
}

// un-dead and demons are unholy to every Ethren cleric (Ethren decision; the gods' own lists wait
// for the gods); a creature's DCC monster type, else the IE general/race of un-dead
static bool IsUnholy(const Actor* actor)
{
	ieDword type = actor->GetStat(IE_DCC_MTYPE);
	if (type == ieDword(MonsterType::Undead) || type == ieDword(MonsterType::Demon)) return true;
	return actor->GetStat(IE_GENERAL) == 4; // general.ids UNDEAD
}

static bool IsLiving(const Actor* actor)
{
	if (IsUnholy(actor)) return false;
	return !(actor->GetStat(IE_STATE_ID) & STATE_DEAD);
}

int Actor::DCCLayOnHands(Actor* target, int natural)
{
	dccLastCast = DCCCastRecord();
	dccLastCast.spell = "layonhands";
	if (!target || GetDCCCasterKind() != CasterKind::Cleric) return -1;
	ieDword blocks = GetStat(IE_DCC_NOCAST);
	if (blocks & BLOCK_LAY_ON_HANDS) {
		Feedback(fmt::format(u"{} can't lay on hands: its god won't let it", GetName()));
		return -1;
	}
	if ((blocks & BLOCK_LOH_ONCE) && dccLaidHands.count(target->GetGlobalID())) {
		Feedback(fmt::format(u"{} can't lay on hands on {} again today", GetName(), target->GetName()));
		return -1;
	}
	if (!IsLiving(target)) {
		Feedback(fmt::format(u"{} can't lay on hands on {}: only the living can be healed", GetName(), target->GetName()));
		return -1;
	}
	bool success = false;
	SpellCheckResult check = ClericCheck(this, natural, Signed(GetStat(IE_DCC_LOHMOD)) + Signed(GetStat(IE_DCC_SPELLMOD)) + GetDCCAugurBonus(AugurRoll::Healing),
					    0, fmt::format(u"lays on hands on {}", target->GetName()).c_str(), 12, success);
	dccLastCast.natural = check.natural;
	dccLastCast.total = check.total;
	dccLastCast.success = success;
	dccLastCast.disapproval = check.disapproval;
	dccLastCast.rangeAfter = dccExt.disapproval.range;
	if (!success) return 0;
	dccLaidHands.insert(target->GetGlobalID());
	Kinship kinship = KinshipFor(AlignmentFromIE(GetStat(IE_ALIGNMENT)), AlignmentFromIE(target->GetStat(IE_ALIGNMENT)));
	int hd = target->IsDCCCharacter() ? std::max(1, Signed(target->GetStat(IE_LEVEL))) : std::max(1, target->GetDCCHitDice());
	int dice = LayOnHandsDice(check.total, kinship, hd);
	int sides = target->IsDCCCharacter() ? Signed(target->GetStat(IE_DCC_HITDIE)) : 8;
	if (sides <= 0) sides = 8;
	int heal = 0;
	for (int i = 0; i < dice; ++i) {
		heal += DCCDie(sides);
	}
	auto fx = EffectQueue::CreateEffect(fx_dcc_heal_ref, ieDword(heal), 0, FX_DURATION_INSTANT_PERMANENT);
	fx->Target = FX_TARGET_PRESET;
	core->ApplyEffect(std::move(fx), target, this);
	static const char16_t* kin[] = { u"same alignment", u"adjacent alignment", u"opposed alignment" };
	Feedback(fmt::format(u"{} heals {}: {}d{} ({}) = {} hp", GetName(), target->GetName(), dice, sides, kin[static_cast<int>(kinship)], heal));
	dccLastCast.repeat = dice;
	return heal;
}

int Actor::DCCTurnUnholy(int natural)
{
	dccLastCast = DCCCastRecord();
	dccLastCast.spell = "turnunholy";
	const RulesData* rules = Rules();
	if (!rules || GetDCCCasterKind() != CasterKind::Cleric) return -1;
	if (GetStat(IE_DCC_NOCAST) & BLOCK_TURN) {
		Feedback(fmt::format(u"{} can't turn the unholy: its god won't let it", GetName()));
		return -1;
	}
	int luck = GetDCCAbilityMod(Ability::Luck);
	bool success = false;
	SpellCheckResult check = ClericCheck(this, natural, Signed(GetStat(IE_DCC_SPELLMOD)) + GetDCCAugurBonus(AugurRoll::TurnUnholy), luck, u"turns the unholy", 12, success);
	dccLastCast.natural = check.natural;
	dccLastCast.total = check.total;
	dccLastCast.success = success;
	dccLastCast.disapproval = check.disapproval;
	dccLastCast.rangeAfter = dccExt.disapproval.range;
	if (!success) return 0;
	const TurnRow* row = rules->magic.TurnFor(check.total);
	Map* map = GetCurrentArea();
	if (!row || !map) return 0;
	int casterLevel = Signed(GetStat(IE_DCC_CASTERLVL));
	auto roller = [this](int sides) { return DCCDie(sides); };

	// the unholy within range, by HD column, nearest first
	std::array<std::vector<Actor*>, 8> byColumn;
	for (Actor* other : map->GetAllActorsInRadius(Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED, unsigned(std::max(5, row->range)), this)) {
		if (other == this || !IsUnholy(other)) continue;
		byColumn[size_t(TurnColumn(other->GetDCCHitDice()))].push_back(other);
	}
	int affected = 0;
	std::vector<String> notes;
	for (size_t column = 0; column < byColumn.size(); ++column) {
		auto& list = byColumn[column];
		if (list.empty()) continue;
		std::sort(list.begin(), list.end(), [this](const Actor* a, const Actor* b) { return SquaredDistance(a->Pos, Pos) < SquaredDistance(b->Pos, Pos); });
		const TurnCell& cell = row->cells[column];
		int count = cell.Count(casterLevel, roller);
		for (Actor* other : list) {
			if (count-- <= 0) break;
			if (cell.kind == TurnCell::Kind::Kill) {
				auto fx = EffectQueue::CreateEffect(fx_dcc_death_magic_ref, 0, 0, FX_DURATION_INSTANT_PERMANENT);
				fx->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(fx), other, this);
				notes.push_back(fmt::format(u"{} is destroyed", other->GetName()));
				++affected;
				continue;
			}
			if (!cell.noSave) {
				RollResult save = other->DCCRollSave(Save::Will, 0, check.total, other->DCCDie(20));
				if (save.success) {
					notes.push_back(fmt::format(u"{} resists (Will {} vs DC {})", other->GetName(), save.total, check.total));
					continue;
				}
			}
			// turned: flees for 3d10 minutes (Fleeing, the stock panic)
			int minutes = DCCDie(10) + DCCDie(10) + DCCDie(10);
			auto fx = EffectQueue::CreateEffect(fx_dcc_combat_magic_ref, 1, ieDword(CombatEffect::Fleeing), FX_DURATION_INSTANT_LIMITED);
			fx->Duration = ieDword(minutes * 60);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), other, this);
			other->Panic(this, PanicMode::RunAway);
			String note = fmt::format(u"{} flees for {} minutes", other->GetName(), minutes);
			if (cell.damageDice) {
				int damage = 0;
				for (int i = 0; i < cell.damageDice; ++i) {
					damage += DCCDie(cell.damageSides);
				}
				auto hurt = EffectQueue::CreateEffect(fx_dcc_magic_damage_ref, ieDword(damage), 0, FX_DURATION_INSTANT_PERMANENT);
				hurt->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(hurt), other, this);
				note += fmt::format(u" and takes {} damage", damage);
			}
			notes.push_back(std::move(note));
			++affected;
		}
	}
	// holy smite: extra damage to every unholy creature in range, no save
	if (row->smiteDice) {
		for (auto& list : byColumn) {
			for (Actor* other : list) {
				int damage = 0;
				for (int i = 0; i < row->smiteDice; ++i) {
					damage += DCCDie(row->smiteSides);
				}
				auto hurt = EffectQueue::CreateEffect(fx_dcc_magic_damage_ref, ieDword(damage), 0, FX_DURATION_INSTANT_PERMANENT);
				hurt->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(hurt), other, this);
				notes.push_back(fmt::format(u"holy smite: {} takes {}", other->GetName(), damage));
			}
		}
	}
	String joined;
	for (const auto& note : notes) {
		joined += (joined.empty() ? u"" : u"; ") + note;
	}
	Feedback(fmt::format(u"{} turns the unholy ({}'): {}", GetName(), row->range, joined.empty() ? String(u"nothing unholy near") : joined));
	dccLastCast.targets = affected;
	return affected;
}

int Actor::DCCAtone(int gold)
{
	if (GetDCCCasterKind() != CasterKind::Cleric || gold <= 0) return 0;
	int range = dccExt.disapproval.range;
	Game* game = core->GetGame();
	int have = InParty && game ? int(game->PartyGold) : Signed(BaseStats[IE_GOLD]);
	gold = std::min(gold, have);
	int after = DisapprovalAfterSacrifice(range, gold);
	int points = range - after;
	if (points <= 0) {
		Feedback(fmt::format(u"{} has nothing to atone for that gold can mend (range 1-{})", GetName(), range));
		return 0;
	}
	int spent = points * ATONEMENT_GOLD;
	if (InParty && game) {
		game->AddGold(-spent);
	} else {
		SetBase(IE_GOLD, BaseStats[IE_GOLD] - ieDword(spent));
	}
	dccExt.disapproval.range = uint8_t(after);
	DCCExtChanged();
	Feedback(fmt::format(u"{} sacrifices {} gp to its god: the disapproval range is now 1-{}", GetName(), spent, after));
	return points;
}

}
