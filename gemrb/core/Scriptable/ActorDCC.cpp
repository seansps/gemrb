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
#include "Item.h"
#include "Prompts.h"
#include "RNG.h"

#include "DCC/Combat.h"
#include "DCC/Luck.h"
#include "DCC/Magic.h"
#include "DCC/Rules.h"
#include "DCC/Stats.h"
#include "GUI/GameControl.h"
#include "Logging/Logging.h"
#include "Net/NetSession.h"
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

// looks (M2.11): the hair and beard overlays ------------------------------------------

void Actor::DCCUpdateLooks()
{
	const RulesData* rules = Rules();
	if (!anims || !rules) return;
	bool helmet = anims->HasHelmet();
	std::string hair = DCC::HairOverlay(rules->hair.Find(int(BaseStats[IE_DCC_HAIR]), false), helmet);
	std::string beard = DCC::HairOverlay(rules->hair.Find(int(BaseStats[IE_DCC_BEARD]), true), helmet);
	// cheap when nothing changed: SetHairRefs only drops the animations for a new code
	anims->SetHairRefs(AnimRef(hair.c_str()), AnimRef(beard.c_str()));
}

// encumbrance by body weight (M2.10) ------------------------------------------------

int Actor::DCCBodyWeight() const
{
	if (BaseStats[IE_DCC_WEIGHT]) return int(BaseStats[IE_DCC_WEIGHT]);
	const RulesData* rules = Rules();
	return rules ? rules->body.Average(int(BaseStats[IE_RACE])) : BodyTable().Average(int(BaseStats[IE_RACE]));
}

int Actor::DCCRollBodyWeight()
{
	const RulesData* rules = Rules();
	BodyTable fallback;
	const BodyTable& table = rules ? rules->body : fallback;
	int weight = table.Roll(int(BaseStats[IE_RACE]), [this](int sides) { return DCCDie(sides); });
	SetBase(IE_DCC_WEIGHT, ieDword(Clamp(weight, 1, 0xffff)));
	return weight;
}

LoadLimits Actor::DCCLoadLimits() const
{
	const RulesData* rules = Rules();
	BodyTable fallback;
	const BodyTable& table = rules ? rules->body : fallback;
	return LimitsFor(DCCBodyWeight(), table.Row(int(BaseStats[IE_RACE])));
}

DCC::Load Actor::DCCLoad() const
{
	return LoadFor(inventory.GetWeight(), DCCLoadLimits());
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
	// a severed shield arm: a shield in hand gives nothing (its +1, Table 3-3)
	if (InjuryBlocksShield(Modified[IE_DCC_INJURY]) && !IsDualWielding()) {
		int shieldSlot = inventory.GetShieldSlot();
		if (shieldSlot >= 0 && inventory.GetSlotItem(shieldSlot)) armor -= 1;
	}
	Modified[IE_DCC_AC] = Signed(Modified[IE_DCC_AC]) + DCC::ArmorClass(armor, mod(Ability::Agi), GetDCCAugurBonus(AugurRoll::ArmorClass));

	int warriorLevel = BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Warrior) ? Signed(BaseStats[IE_LEVEL]) : 0;
	Modified[IE_DCC_INIT] = Signed(Modified[IE_DCC_INIT]) + InitiativeMod(mod(Ability::Agi), warriorLevel, GetDCCAugurBonus(AugurRoll::Initiative));
	Modified[IE_DCC_SPEED] = Speed(Signed(Modified[IE_DCC_SPEED]), GetDCCAugurBonus(AugurRoll::Speed));

	// spellcasting (M2.5): caster level, the armor check penalty (wizards and elves only), the
	// highest castable spell level (class table, capped by Int or Per on Table 1-1), the mutations
	CasterKind caster = GetDCCCasterKind();
	int casterLevel = caster != CasterKind::None ? Signed(BaseStats[IE_LEVEL]) : 0;
	Modified[IE_DCC_CASTERLVL] = Signed(Modified[IE_DCC_CASTERLVL]) + casterLevel;
	int armorPenalty = 0;
	if (caster == CasterKind::Wizard) {
		const CREItem* armorItem = inventory.GetSlotItem(Inventory::GetArmorSlot());
		if (armorItem) {
			std::string animation;
			const Item* itm = gamedata->GetItem(armorItem->ItemResRef, true);
			if (itm) {
				animation = itm->AnimationType.c_str();
				gamedata->FreeItem(itm, armorItem->ItemResRef);
			}
			armorPenalty += rules->combat.armorFumbles.ArmorCheck(armorItem->ItemResRef.c_str(), animation);
		}
		int shieldSlot = IsDualWielding() ? -1 : inventory.GetShieldSlot();
		const CREItem* shieldItem = shieldSlot >= 0 ? inventory.GetSlotItem(shieldSlot) : nullptr;
		if (shieldItem) armorPenalty += rules->combat.armorFumbles.ShieldCheck(shieldItem->ItemResRef.c_str());
	}
	Modified[IE_DCC_ARMORPEN] = Signed(Modified[IE_DCC_ARMORPEN]) + armorPenalty;
	int maxSpell = 0;
	if (caster != CasterKind::None) {
		const LevelRow* row = rules->classes.Row(BaseStats[IE_CLASS], BaseStats[IE_LEVEL]);
		int score = current[static_cast<size_t>(caster == CasterKind::Cleric ? Ability::Per : Ability::Int)];
		maxSpell = std::min(row ? row->maxSpellLevel : 0, core->GetDCCAbilityBonus(STAT_DCC_MAX_SPELL_LEVEL, score));
	}
	Modified[IE_DCC_MAXSPELL] = std::max(0, maxSpell);
	Modified[IE_DCC_MUTATION] |= dccMutations;
	uint32_t tint = MutationTint(Modified[IE_DCC_MUTATION]);
	if (tint) SetColorMod(0xff, RGBModifier::TINT, -1, Color((tint >> 16) & 0xff, (tint >> 8) & 0xff, tint & 0xff, 0xff));

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

static const char16_t* SaveName(Save save)
{
	static const char16_t* names[] = { u"Fortitude", u"Reflex", u"Willpower" };
	return names[static_cast<int>(save)];
}

// a failed save against this kills: Death (slay, disintegrate) and petrification
static bool IsLethalEffect(const Effect* fx)
{
	if (!fx) return false;
	static EffectRef fx_death_ref = { "Death", -1 };
	static EffectRef fx_petrify_ref = { "State:Petrification", -1 };
	return fx->Opcode == ieDword(EffectQueue::ResolveEffect(fx_death_ref)) || fx->Opcode == ieDword(EffectQueue::ResolveEffect(fx_petrify_ref));
}

// the DC of a saving throw an effect asks for
// M2.5: effects from DCC spells carry their spell check total, which becomes the DC here
static int EffectSaveDC(const Effect* fx)
{
	if (!fx) return DEFAULT_SAVE_DC;
	if (fx->DCCSpellCheck) return static_cast<int>(fx->DCCSpellCheck);
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

	// tests: forced dice (DCCForceDice) replace the pre-rolled die, which the spell's effects share
	if (!dccForcedDice.empty()) lastSave.savingThrow[type] = static_cast<ieByte>(DCCDie(20));
	RollResult result = DCCRollSave(save, modifier, dc, lastSave.savingThrow[type], flags);

	// Luck: a burn on this save die counts for every effect that shares the die (one spell's
	// effects); a save vs death may wait for the owner's answer; else the policy may burn
	if (dccSaveBurn[type] && !result.cursed) {
		result.total += dccSaveBurn[type];
		result.success = result.total >= result.target;
	} else if (!result.success && !result.cursed) {
		bool lethal = IsLethalEffect(fx);
		if (lethal && DCCAskSaveBurn(*fx, type, result)) {
			// counts as saved until the answer comes; the effect waits in dccHeldSave
			return true;
		}
		int before = result.total;
		if (DCCPolicyRollBurn(result, lethal ? BurnReason::Death : BurnReason::Miss, u"a save")) dccSaveBurn[type] = result.total - before;
	}

	// each effect of a spell ends up here, so only report a new roll
	if (lastSave.prevType != type || lastSave.prevRoll != result.total) {
		String msg = fmt::format(u"{} save: {} {:+} = {} vs DC {}: {}", SaveName(save), result.natural, result.total - result.natural, result.total, result.target,
					 result.success ? u"success" : (result.cursed ? u"failed (cursed by fortune)" : u"failed"));
		Log(COMBAT, "DCC", "{}: {}", fmt::WideToChar { GetName() }, fmt::WideToChar { msg });
		DCCRecordLine(fmt::format(u"{}: {}", GetName(), msg));
		if (core->HasFeedback(FT_COMBAT)) displaymsg->DisplayStringName(std::move(msg), GUIColors::WHITE, this);
	}
	lastSave.prevType = type;
	lastSave.prevRoll = result.total;
	return result.success;
}

// a lethal effect waiting for the owner's Luck decision (the game is paused meanwhile)
struct Actor::DCCHeldSave {
	ieDword type = 0; // the IE save type
	ieByte die = 0; // its pre-rolled die: further lethal effects of the same spell join in
	RollResult roll;
	std::vector<std::shared_ptr<Effect>> effects;
};

bool Actor::DCCAskSaveBurn(const Effect& fx, ieDword type, const RollResult& roll)
{
	if (dccHeldSave) {
		// the same spell's next lethal effect waits with the first
		if (dccHeldSave->type != type || dccHeldSave->die != lastSave.savingThrow[type]) return false;
		dccHeldSave->effects.push_back(std::make_shared<Effect>(fx));
		return true;
	}
	LuckPolicy policy = GetDCCLuckPolicy();
	if (!InParty || !IsDCCCharacter() || !DCCPausesOnKeyRolls()) return false;
	const NetSession* net = core->GetNetSession();
	if ((net && !net->IsHost()) || !core->GetGameControl()) return false;
	int luck = GetDCCLuck();
	int gap = roll.target - roll.total;
	BurnValue value = GetDCCBurnValue();
	if (luck <= 0 || gap <= 0 || MaxBurnBonus(value, luck) < gap) return false;

	const RulesData* rules = Rules();
	Save save = rules->saves.Get(type);
	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "luckburn";
	q.title = "Burn Luck?";
	std::string name = fmt::format("{}", fmt::WideToChar { GetShortName() });
	std::string how = value.IsDie() ? fmt::format("a d{} each", value.luckDie) : fmt::format("+{} each", value.perPoint);
	q.text = fmt::format("{} fails a {} save against death by {} ({} vs DC {}). Burn Luck ({}, {} left)?", name, fmt::WideToChar { SaveName(save) }, gap,
			     roll.total, roll.target, how, luck);
	q.fields = { { "Luck", 0, luck, PolicyBurn(policy, BurnReason::Death, gap, luck, value) } };
	q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));

	auto held = std::make_shared<DCCHeldSave>();
	held->type = type;
	held->die = lastSave.savingThrow[type];
	held->roll = roll;
	held->effects.push_back(std::make_shared<Effect>(fx));
	ieDword selfID = GetGlobalID();
	uint32_t id = core->GetPrompts().Ask(this, std::move(q), [selfID, held](const PromptResult& r) {
		Game* game = core->GetGame();
		Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
		if (!self || self->dccHeldSave != held) return;
		self->dccHeldSave.reset();
		if (self->GetStat(IE_STATE_ID) & STATE_DEAD) return;
		int points = Clamp<int>(r.Value(), 0, self->GetDCCLuck());
		RollResult result = held->roll;
		if (points > 0) {
			BurnValue value = self->GetDCCBurnValue();
			int bonus = self->DCCBurnLuck(points, value);
			result.total += bonus;
			result.success = result.total >= result.target;
			String how = value.IsDie() ? fmt::format(u"{}d{} = +{}", points, value.luckDie, bonus) : fmt::format(u"+{}", bonus);
			DCCFeedback(fmt::format(u"{} burns {} Luck on a save vs death ({}): {} -> {} vs DC {}, {} (Luck {} left)", self->GetName(), points, how, held->roll.total,
						result.total, result.target, result.success ? u"survives" : u"still a failure", self->GetDCCLuck()),
				    FT_COMBAT);
		}
		if (result.success) return;
		// no burn, or not enough: what the save was against happens now (without another save)
		Map* map = self->GetCurrentArea();
		for (const auto& effect : held->effects) {
			auto fx = std::make_unique<Effect>(*effect);
			fx->SavingThrowType = 0;
			fx->Target = FX_TARGET_PRESET;
			Scriptable* caster = map ? map->GetScriptableByGlobalID(fx->CasterID) : nullptr;
			core->ApplyEffect(std::move(fx), self, caster ? caster : self);
		}
	});
	if (!id) return false;
	dccHeldSave = std::move(held);
	DCCFeedback(fmt::format(u"{} fails a {} save against death: {} vs DC {}; burn Luck?", GetName(), SaveName(save), roll.total, roll.target), FT_COMBAT);
	return true;
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

	// the actor's dice, so tests can force them (DCCForceDice); random otherwise
	Roller rng = [this](int sides) { return DCCDie(sides); };
	int hp = RollLevelHP(info->hitDie, GetDCCAbilityMod(Ability::Sta), GetDCCAugurBonus(AugurRoll::HitPoints), roller ? *roller : rng);

	SetBase(IE_CLASS, classID);
	SetBase(IE_LEVEL, level + 1);
	SetBase(IE_MAXHITPOINTS, BaseStats[IE_MAXHITPOINTS] + hp);
	SetBase(IE_HITPOINTS, BaseStats[IE_HITPOINTS] + hp);
	CreateDerivedStatsDCC();
	RefreshEffects();
	// the funnel's class pick is made (Phase 3)
	if (dccExt.funnel.flags & FUNNEL_CLASS_PENDING) {
		dccExt.funnel.flags &= uint8_t(~FUNNEL_CLASS_PENDING);
		DCCExtChanged();
	}
	// casters learn their spells: random on Table 5-8 for wizards and elves, the god's list for
	// clerics (Phase 3; the learn window shows them and lets the player choose where the rules allow)
	if (GetDCCCasterKind() != CasterKind::None) {
		DCCLearnLevelSpells(roller);
	}
	return hp;
}

void Actor::DCCRestHeal(bool bedRest)
{
	// injuries that get worse overnight (infection, rot), then the ones that last "until healed"
	// heal with a night's rest (Ethren decision)
	DCCNightlyInjuries();
	fxqueue.RemoveAllEffects(ResRef("DCCHEAL"));
	static EffectRef fx_dcc_combat_ref = { "DCC:Combat", -1 };
	for (ieDword kind = 0; kind <= static_cast<ieDword>(CombatEffect::Subdued); ++kind) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, kind | COMBAT_UNTIL_HEALED);
	}
	for (int i = 0; i < ABILITY_COUNT; ++i) {
		Ability ability = static_cast<Ability>(i);
		int loss = Signed(BaseStats[LossStat(ability)]);
		// spellburn's points come back their own way (DCCRestMagic), not with the night's healing
		int burned = i < 3 ? int(dccExt.burn.points[size_t(i)]) : 0;
		if (loss - burned <= 0) continue;
		SetBase(LossStat(ability), burned + LossAfterRest(ability, loss - burned, bedRest));
	}
	// thieves and halflings get burned Luck back, up to the natural maximum (luck.md)
	DCCRecoverLuck();
	// spells come back, disapproval resets, spellburn recovers (M2.5)
	DCCRestMagic();
}

}
