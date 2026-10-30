// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC rules (DCCRules = 1) for actors, beside the 2e/3e code in Actor.cpp, which
// only calls in here behind GFFlags::RULES_DCC. The rules themselves are pure
// functions in core/DCC; see docs/architecture/rules-dcc.md.

#include "ie_stats.h"

#include "DisplayMessage.h"
#include "EffectQueue.h"
#include "Game.h"
#include "Interface.h"
#include "Map.h"
#include "RNG.h"

#include "DCC/Combat.h"
#include "DCC/Rules.h"
#include "DCC/Stats.h"
#include "Scriptable/Actor.h"

namespace GemRB {

using namespace DCC;

static const RulesData* Rules()
{
	return core->GetDCCRules();
}

static int Signed(ieDword value)
{
	return static_cast<int>(static_cast<ieDwordSigned>(value));
}

static uint32_t SaveFlag(Save save)
{
	switch (save) {
		case Save::Fort:
			return AF_FORT;
		case Save::Ref:
			return AF_REF;
		default:
			return AF_WILL;
	}
}

static int DCCModifier(int score)
{
	return core->GetDCCAbilityBonus(STAT_DCC_MOD, score);
}

bool Actor::IsDCCCharacter() const
{
	const RulesData* rules = Rules();
	return rules && rules->classes.IsCharacterClass(BaseStats[IE_CLASS]);
}

int Actor::GetDCCAbilityMod(Ability ability) const
{
	return DCCModifier(Signed(GetStat(AbilityStat(ability))));
}

int Actor::GetDCCAugurBonus(AugurRoll roll, uint32_t flags) const
{
	const RulesData* rules = Rules();
	if (!rules) return 0;
	return AugurBonus(rules->augurs.Target(GetStat(IE_DCC_AUGUR)), Signed(BaseStats[IE_DCC_AUGURMOD]), roll, flags);
}

ieDword Actor::GetDCCStates() const
{
	return Modified[IE_DCC_STATE];
}

bool Actor::IsDCCHeld() const
{
	return IsHeld(GetDCCStates());
}

// class table values; on load, after level up and whenever class or level change (ResetStats)
void Actor::CreateDerivedStatsDCC()
{
	dccDerivedClass = BaseStats[IE_CLASS];
	dccDerivedLevel = BaseStats[IE_LEVEL];
	const RulesData* rules = Rules();
	if (!rules) return;

	BaseStats[IE_DCC_STATE] = 0;
	BaseStats[IE_DCC_AC] = 0;
	BaseStats[IE_DCC_INIT] = 0;
	unsigned int race = BaseStats[IE_RACE];
	BaseStats[IE_DCC_SPEED] = BaseSpeed(race == 4 /* DWARF */ || race == 5 /* HALFLING */);

	const LevelRow* row = rules->classes.Row(BaseStats[IE_CLASS], BaseStats[IE_LEVEL]);
	if (!row) {
		// monsters keep their own saves, stored as DCC bonuses in the IE save fields
		// (Fort death, Ref breath, Will spell)
		for (int i = 0; i < SAVE_COUNT; ++i) {
			Save save = static_cast<Save>(i);
			BaseStats[SaveStat(save)] = BaseStats[SaveBonusStat(save)];
		}
		for (unsigned int stat : { IE_DCC_ATTACK, IE_DCC_DEEDDIE, IE_DCC_DEEDADD, IE_DCC_CRITDICE, IE_DCC_CRITDIE, IE_DCC_CRITADD,
					   IE_DCC_CRITTABLE, IE_DCC_THREAT, IE_DCC_ACTDIE1, IE_DCC_ACTDIE2, IE_DCC_ACTDIE3, IE_DCC_LUCKDIE, IE_DCC_HITDIE }) {
			BaseStats[stat] = 0;
		}
		// and their combat numbers from the CRE's DCC block: attack bonus, threat, action die,
		// initiative, and the crit die and table their HD and type give (Table 9-10)
		int hd = GetDCCHitDice();
		bool fromLevel = BaseStats[IE_DCC_HD] == HD_FROM_LEVEL;
		BaseStats[IE_DCC_ATTACK] = fromLevel ? hd : BaseStats[IE_DCC_MATTACK];
		BaseStats[IE_DCC_THREAT] = BaseStats[IE_DCC_MTHREAT] ? BaseStats[IE_DCC_MTHREAT] : 20;
		BaseStats[IE_DCC_ACTDIE1] = BaseStats[IE_DCC_MACTDIE] ? BaseStats[IE_DCC_MACTDIE] : 20;
		BaseStats[IE_DCC_INIT] = BaseStats[IE_DCC_MINIT];
		MonsterType type = static_cast<MonsterType>(std::min<ieDword>(BaseStats[IE_DCC_MTYPE], MONSTER_TYPE_COUNT - 1));
		MonsterCrit crit = rules->combat.monsterCrits.Lookup(hd, type);
		BaseStats[IE_DCC_CRITDICE] = crit.dice;
		BaseStats[IE_DCC_CRITDIE] = crit.die;
		BaseStats[IE_DCC_CRITTABLE] = static_cast<ieDword>(crit.table);
		return;
	}

	BaseStats[IE_DCC_ATTACK] = row->attack;
	BaseStats[IE_DCC_DEEDDIE] = row->deedDie;
	BaseStats[IE_DCC_DEEDADD] = row->deedAdd;
	BaseStats[IE_DCC_CRITDICE] = row->critDice;
	BaseStats[IE_DCC_CRITDIE] = row->critDie;
	BaseStats[IE_DCC_CRITADD] = row->critAdd;
	BaseStats[IE_DCC_CRITTABLE] = row->critTable;
	BaseStats[IE_DCC_THREAT] = row->threat;
	BaseStats[IE_DCC_ACTDIE1] = row->actionDice[0];
	BaseStats[IE_DCC_ACTDIE2] = row->actionDice[1];
	BaseStats[IE_DCC_ACTDIE3] = row->actionDice[2];
	BaseStats[IE_DCC_LUCKDIE] = row->luckDie;
	for (int i = 0; i < SAVE_COUNT; ++i) {
		Save save = static_cast<Save>(i);
		BaseStats[SaveStat(save)] = row->SaveBonus(save);
	}
	BaseStats[IE_DCC_HITDIE] = rules->classes.HitDie(BaseStats[IE_CLASS], BaseStats[IE_LEVEL]);
}

// runs in RefreshEffects after the effects: Modified holds the base values plus effects
void Actor::RefreshDCCStats(const stats_t& previous)
{
	const RulesData* rules = Rules();
	if (!rules) return;
	bool character = IsDCCCharacter();

	// current scores: score minus loss, floored at 0. Monsters only get the states at 0
	// for abilities they actually lost points of, since their CREs may leave scores at 0
	AbilityScores current {};
	std::array<bool, ABILITY_COUNT> apply {};
	for (int i = 0; i < ABILITY_COUNT; ++i) {
		Ability ability = static_cast<Ability>(i);
		int loss = Signed(Modified[LossStat(ability)]);
		current[i] = CurrentScore(Signed(Modified[AbilityStat(ability)]), loss);
		Modified[AbilityStat(ability)] = current[i];
		apply[i] = character || loss > 0;
	}
	auto mod = [&current, character](Ability ability) {
		return character ? DCCModifier(current[static_cast<size_t>(ability)]) : 0;
	};

	// saves: class bonus + ability mod + augur + stock save bonus effects
	for (int i = 0; i < SAVE_COUNT; ++i) {
		Save save = static_cast<Save>(i);
		unsigned int ieStat = SaveBonusStat(save);
		// 2e effects lower the save target number for a bonus, 3e-style ones raise the stat
		int effects = Signed(BaseStats[ieStat]) - Signed(Modified[ieStat]);
		if (!IsReverseToHit()) effects = -effects;
		int augur = GetDCCAugurBonus(AugurRoll::Save, SaveFlag(save));
		Modified[SaveStat(save)] = SaveTotal(Signed(Modified[SaveStat(save)]), mod(SaveAbility(save)), augur, effects);
	}

	// ascending AC from the stock AC (natural, armor, shield and magic; no dex or wisdom here)
	int armor = IsReverseToHit() ? BonusFromIEAC(AC.GetTotal()) : AC.GetTotal() - 10;
	Modified[IE_DCC_AC] = Signed(Modified[IE_DCC_AC]) + DCC::ArmorClass(armor, mod(Ability::Agi), GetDCCAugurBonus(AugurRoll::ArmorClass));

	int warriorLevel = BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Warrior) ? Signed(BaseStats[IE_LEVEL]) : 0;
	Modified[IE_DCC_INIT] = Signed(Modified[IE_DCC_INIT]) + InitiativeMod(mod(Ability::Agi), warriorLevel, GetDCCAugurBonus(AugurRoll::Initiative));
	Modified[IE_DCC_SPEED] = Speed(Signed(Modified[IE_DCC_SPEED]), GetDCCAugurBonus(AugurRoll::Speed));

	// the states at 0 (combat.md, Ethren design proposal); the dead and dying are past them
	uint32_t states = StatesAtZero(current, apply);
	if (Modified[IE_DCC_BLEED]) states |= STATE_DYING;
	Modified[IE_DCC_STATE] = states;
	if ((InternalFlags & IF_REALLYDIED) || (Modified[IE_STATE_ID] & STATE_DEAD)) {
		return;
	}
	if (states & STATE_IMMOBILE) {
		Modified[IE_HELD] = 1;
		Modified[IE_STATE_ID] |= STATE_HELPLESS;
		AddPortraitIcon(PI::HELD);
	}
	if (states & STATE_UNCONSCIOUS) {
		Modified[IE_STATE_ID] |= STATE_HELPLESS | STATE_SLEEP;
		Modified[IE_EXTSTATE_ID] |= EXTSTATE_DOESNT_AWAKEN_ON_DAMAGE;
		AddPortraitIcon(PI::SLEEP);
	}
	if (states & STATE_WITLESS) {
		// can't act, cast or talk, and stands where it is
		Modified[IE_HELD] = 1;
		Modified[IE_STATE_ID] |= STATE_HELPLESS;
		AddPortraitIcon(PI::FEEBLEMIND);
	}
	if (states & STATE_CURSED) {
		AddPortraitIcon(PI::HOPELESS);
	}
	if (states & STATE_DYING) {
		// down at 0 hp: helpless, can't act, and damage doesn't wake them
		Modified[IE_HELD] = 1;
		Modified[IE_STATE_ID] |= STATE_HELPLESS | STATE_SLEEP;
		Modified[IE_EXTSTATE_ID] |= EXTSTATE_DOESNT_AWAKEN_ON_DAMAGE;
		AddPortraitIcon(PI::SLEEP);
	}
	if (IsHeld(states)) {
		InterruptCasting = true;
	}

	bool wasOut = previous[IE_DCC_STATE] & (STATE_UNCONSCIOUS | STATE_DYING);
	bool isOut = states & (STATE_UNCONSCIOUS | STATE_DYING);
	if (isOut && !wasOut) {
		SetStance(IE_ANI_SLEEP);
	} else if (!isOut && wasOut) {
		SetStance(IE_ANI_GET_UP);
	}
}

RollResult Actor::DCCRoll(RollKind kind, int modifier, int target, int natural, int dieSides, int threat) const
{
	if (dieSides < 1) dieSides = 20;
	if (natural <= 0) natural = RAND(1, dieSides);
	bool cursed = GetDCCStates() & STATE_CURSED;
	// the hour after recovering the body: -4 to all rolls
	modifier += Signed(GetStat(IE_DCC_ROLLMOD));
	return ResolveRoll(kind, natural, dieSides, modifier, target, cursed, threat);
}

RollResult Actor::DCCRollSave(Save save, int modifier, int dc, int natural, uint32_t augurFlags) const
{
	// the save totals already hold the augurs for all saves and this save; add the ones
	// that depend on what is being saved against (traps, poison)
	int context = GetDCCAugurBonus(AugurRoll::Save, augurFlags) - GetDCCAugurBonus(AugurRoll::Save);
	int total = Signed(GetStat(SaveStat(save))) + modifier + context;
	return DCCRoll(RollKind::Save, total, dc, natural);
}

RollResult Actor::DCCRollLuckCheck(int natural) const
{
	if (natural <= 0) natural = RAND(1, 20);
	bool cursed = GetDCCStates() & STATE_CURSED;
	return ResolveLuckCheck(natural, Signed(GetStat(IE_DCC_LUCK)), cursed);
}

// the DC of a saving throw an effect asks for
// M2.5: effects from DCC spells carry their spell check total, which becomes the DC here
static int EffectSaveDC(const Effect* fx)
{
	if (!fx) return DEFAULT_SAVE_DC;
	return DefaultSaveDC(fx->SpellLevel);
}

bool Actor::GetSavingThrowDCC(ieDword type, int modifier, const Effect* fx)
{
	const RulesData* rules = Rules();
	if (!rules) return false;

	Save save = rules->saves.Get(type);
	int dc = EffectSaveDC(fx);
	uint32_t flags = AF_NONE;
	if (fx) {
		// traps are effects from something other than an actor
		const Scriptable* caster = area ? area->GetScriptableByGlobalID(fx->CasterID) : nullptr;
		if (caster && caster->Type != ST_ACTOR) flags |= AF_TRAP;
		static EffectRef fx_poisoned_state_ref = { "State:Poisoned", -1 };
		if (fx->Opcode == ieDword(EffectQueue::ResolveEffect(fx_poisoned_state_ref))) flags |= AF_POISON;
	}

	RollResult result = DCCRollSave(save, modifier, dc, lastSave.savingThrow[type], flags);

	// each effect of a spell ends up here, so only report a new roll
	if (core->HasFeedback(FT_COMBAT) && (lastSave.prevType != type || lastSave.prevRoll != result.total)) {
		static const char16_t* names[] = { u"Fortitude", u"Reflex", u"Willpower" };
		String msg = fmt::format(u"{} save: {} {:+} = {} vs DC {}: {}", names[static_cast<int>(save)], result.natural,
					 result.total - result.natural, result.total, result.target,
					 result.success ? u"success" : (result.cursed ? u"failed (cursed by fortune)" : u"failed"));
		displaymsg->DisplayStringName(std::move(msg), GUIColors::WHITE, this);
	}
	lastSave.prevType = type;
	lastSave.prevRoll = result.total;
	return result.success;
}

int Actor::DCCLevelForXP() const
{
	const RulesData* rules = Rules();
	return rules ? rules->xp.LevelForXP(Signed(BaseStats[IE_XP])) : 0;
}

bool Actor::DCCCanLevelUp() const
{
	return IsDCCCharacter() && DCCLevelForXP() > Signed(BaseStats[IE_LEVEL]);
}

int Actor::DCCLevelUp(int classID, const Roller* roller)
{
	const RulesData* rules = Rules();
	if (!rules || !IsDCCCharacter()) return -1;

	int level = Signed(BaseStats[IE_LEVEL]);
	int current = BaseStats[IE_CLASS];
	if (level >= rules->xp.MaxLevel() || level >= MAX_LEVEL) return -1;

	const ClassInfo* info = rules->classes.Class(classID);
	if (!info || classID == static_cast<int>(ClassID::Level0)) return -1;
	if (level == 0 || current == static_cast<int>(ClassID::Level0)) {
		// the class choice at level 1; dwarves, elves and halflings must take their own class
		// and the human classes need a human (dccclass.2da RACE)
		if (info->race && info->race != Signed(BaseStats[IE_RACE])) return -1;
	} else if (classID != current) {
		return -1;
	}

	Roller rng = [](int sides) { return RAND(1, sides); };
	int hp = RollLevelHP(info->hitDie, GetDCCAbilityMod(Ability::Sta), GetDCCAugurBonus(AugurRoll::HitPoints), roller ? *roller : rng);

	SetBase(IE_CLASS, classID);
	SetBase(IE_LEVEL, level + 1);
	SetBase(IE_MAXHITPOINTS, BaseStats[IE_MAXHITPOINTS] + hp);
	SetBase(IE_HITPOINTS, BaseStats[IE_HITPOINTS] + hp);
	CreateDerivedStatsDCC();
	RefreshEffects();
	return hp;
}

void Actor::DCCRestHeal(bool bedRest)
{
	// crit injuries that last "until healed" heal with a night's rest (Ethren decision)
	fxqueue.RemoveAllEffects(ResRef("DCCHEAL"));
	for (int i = 0; i < ABILITY_COUNT; ++i) {
		Ability ability = static_cast<Ability>(i);
		int loss = Signed(BaseStats[LossStat(ability)]);
		if (loss <= 0) continue;
		SetBase(LossStat(ability), LossAfterRest(ability, loss, bedRest));
	}
}

}
