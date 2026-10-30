// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC combat (DCCRules = 1) for actors: the attack roll, damage, crits, fumbles,
// two-weapon fighting, initiative within the IE round, bleeding out and recovering
// the body. Actor.cpp only calls in here behind GFFlags::RULES_DCC; the rules are
// pure functions in core/DCC (Combat.h, CombatTables.h). See
// docs/architecture/rules-dcc.md, "Combat (M2.3)".
//
// Dice: every die here goes through Actor::DCCDie, so tests can queue the results
// (DCCForceDice). An attack action uses the attacker's dice in this order: the deed
// die (scripted attacks only; real rounds roll it in InitRoundDCC, after the
// initiative die of a new fight), the attack die, the off-hand die (two weapons);
// then for each hand that hits its damage dice, and on a crit the crit dice and the
// crit result's dice (extra damage, durations, a free attack's rolls); on a fumble
// the fumble die and the result's dice. Saves against a result use the dice of the
// creature saving.

#include "damages.h"
#include "ie_stats.h"

#include "DisplayMessage.h"
#include "EffectQueue.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "Item.h"
#include "Map.h"
#include "RNG.h"

#include "DCC/Combat.h"
#include "DCC/CombatTables.h"
#include "DCC/Rules.h"
#include "DCC/Stats.h"
#include "GUI/GameControl.h"
#include "Logging/Logging.h"
#include "Scriptable/Actor.h"

#include <deque>

namespace GemRB {

using namespace DCC;

static EffectRef fx_dcc_combat_ref = { "DCC:Combat", -1 };
static EffectRef fx_dcc_loss_ref = { "DCC:AbilityLoss", -1 };
static EffectRef fx_dcc_damage_ref = { "Damage", -1 };
static EffectRef fx_dcc_stun_ref = { "State:Stun", -1 };
static EffectRef fx_dcc_blind_ref = { "State:Blind", -1 };
static EffectRef fx_dcc_deaf_ref = { "State:Deafness", -1 };
static EffectRef fx_dcc_silence_ref = { "State:Silenced", -1 };
static EffectRef fx_dcc_sleep_ref = { "State:Sleep", -1 };
static EffectRef fx_dcc_hold_ref = { "State:Hold", -1 };
static EffectRef fx_dcc_movement_ref = { "MovementRateModifier", -1 };
static EffectRef fx_dcc_death_ref = { "Death", -1 };

// "until healed" crit injuries end with the next night's rest (DCCRestHeal), permanent ones never
static const ResRef UntilHealedSource = "DCCHEAL";
static const ResRef PermanentSource = "DCCPERM";

static const RulesData* Rules()
{
	return core->GetDCCRules();
}

static int Signed(ieDword value)
{
	return static_cast<int>(static_cast<ieDwordSigned>(value));
}

// the combat log: shown like the stock roll feedback (and so replicated to co-op
// clients with it), logged, and kept for GemRB.GetDCCCombatLog (tests)
static std::deque<String> combatLog;

static void Feedback(String text, int type)
{
	Log(COMBAT, "DCC", "{}", fmt::WideToChar { text });
	combatLog.push_back(text);
	if (combatLog.size() > 300) combatLog.pop_front();
	const GameControl* gc = core->GetGameControl();
	if (core->HasFeedback(type) && !(gc && gc->InDialog())) {
		displaymsg->DisplayString(text);
	}
}

std::vector<String> Actor::DCCCombatLog(bool clear)
{
	std::vector<String> lines(combatLog.begin(), combatLog.end());
	if (clear) combatLog.clear();
	return lines;
}

static String Name(const Actor* actor)
{
	return actor ? actor->GetName() : u"";
}

static String Widen(const std::string& text)
{
	return String(text.begin(), text.end());
}

// " +2", " -1", or nothing for 0
static String Plus(int value)
{
	if (!value) return u"";
	return fmt::format(u" {:+}", value);
}

static DiceExpr WeaponDice(const ITMExtHeader* header)
{
	DiceExpr dice;
	dice.count = header ? header->DiceThrown : 1;
	dice.sides = header ? header->DiceSides : 3;
	dice.add = header ? header->DamageBonus : 0;
	return dice;
}

int Actor::GetDCCHitDice() const
{
	if (IsDCCCharacter() || BaseStats[IE_DCC_HD] == HD_FROM_LEVEL) return Signed(BaseStats[IE_LEVEL]);
	return Signed(BaseStats[IE_DCC_HD]);
}

int Actor::DCCDie(int sides)
{
	if (sides < 1) return 0;
	if (!dccForcedDice.empty()) {
		int value = dccForcedDice.front();
		dccForcedDice.erase(dccForcedDice.begin());
		return Clamp(value, 1, sides);
	}
	return RAND(1, sides);
}

void Actor::DCCForceDice(std::vector<int> dice)
{
	dccForcedDice = std::move(dice);
}

int Actor::DCCNumberOfAttacks() const
{
	if (!IsDCCCharacter()) return GetStat(IE_NUMBEROFATTACKS);
	// one attack per action die; a wizard only attacks with the first (classes.md)
	int dice = 0;
	for (unsigned int stat : { IE_DCC_ACTDIE1, IE_DCC_ACTDIE2, IE_DCC_ACTDIE3 }) {
		if (GetStat(stat)) ++dice;
	}
	if (BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Wizard)) dice = std::min(dice, 1);
	return 2 * std::max(dice, 1);
}

// the action die of the attack with this index in the round
static int ActionDie(const Actor* actor, int index)
{
	static const unsigned int dice[] = { IE_DCC_ACTDIE1, IE_DCC_ACTDIE2, IE_DCC_ACTDIE3 };
	int die = 0;
	if (actor->IsDCCCharacter() && index > 0) die = actor->GetStat(dice[std::min(index, 2)]);
	if (!die) die = actor->GetStat(IE_DCC_ACTDIE1);
	return die ? die : 20;
}

void Actor::InitRoundDCC(ieDword gameTime)
{
	if (!Rules()) return;
	// initiative is rolled once per fight (combat.md): a new one starts after a round without attacking
	unsigned int roundTicks = core->Time.attack_round_size;
	bool fresh = !dccInitiativeTime || gameTime > dccInitiativeTime + 2 * roundTicks;
	dccInitiativeTime = gameTime;
	if (fresh) {
		bool twoHanded = weaponInfo[0].itemflags & IE_INV_ITEM_TWOHANDED;
		int die = InitiativeDie(twoHanded);
		int natural = DCCDie(die);
		int mod = Signed(GetStat(IE_DCC_INIT));
		dccInitiative.total = natural + mod;
		dccInitiative.agility = Signed(GetStat(IE_DEX));
		dccInitiative.hitDice = GetDCCHitDice();
		Feedback(fmt::format(u"{} rolls initiative: d{} {}{} = {}", Name(this), die, natural, Plus(mod), dccInitiative.total), FT_TOHIT);
	}
	// the deed die: once per round, for every attack in it (open-questions.md)
	int deedDie = GetStat(IE_DCC_DEEDDIE);
	dccDeedRoll = deedDie ? DCCDie(deedDie) : 0;
}

int Actor::DCCInitiativeDelay() const
{
	return InitiativeDelay(dccInitiative, core->Time.attack_round_size, attacksperround, GetStat(IE_DCC_INITBOTTOM));
}

// ---------------------------------------------------------------------------
// the attack

struct Actor::DCCSwing {
	int hand = 0;
	const WeaponInfo* wi = nullptr;
	AttackInput in;
	bool missile = false;
	int abilityMod = 0; // Str (melee) or Agi (missile), added to the roll
	int deed = 0; // deed die + its fixed part, added to the roll and the damage
	int damageBonus = 0; // everything flat on the damage
	DiceExpr extraOnHit; // a free attack's bonus damage (Crit Table I, 16)
	bool offHandFumble = true;
	AttackRecord record;
	int damage = 0;
	int extraDamage = 0; // from the crit
};

static bool HasWeapon(const Actor* actor, bool droppable)
{
	if (actor->inventory.FistsEquipped()) return false;
	int slot = actor->inventory.GetEquippedSlot();
	const CREItem* item = actor->inventory.GetSlotItem(slot);
	if (!item) return false;
	return !droppable || !(item->Flags & IE_INV_ITEM_UNDROPPABLE);
}

static const CREItem* ShieldItem(const Actor* actor)
{
	if (actor->IsDualWielding()) return nullptr;
	int slot = actor->inventory.GetShieldSlot();
	return slot < 0 ? nullptr : actor->inventory.GetSlotItem(slot);
}

// a weapon or shield broken by a crit or fumble: gone, and the best other weapon comes out.
// Not Inventory::BreakItemSlot, which also drops the item from the resource cache while
// other creatures may still wield the same item
static void DestroySlotItem(Actor* holder, int slot)
{
	delete holder->inventory.RemoveItem(slot, 0);
	if (!holder->inventory.EquipBestWeapon(EQUIP_MELEE)) {
		holder->inventory.CacheAllWeaponInfo();
	}
}

static const CREItem* ArmorItem(const Actor* actor)
{
	return actor->inventory.GetSlotItem(Inventory::GetArmorSlot());
}

// Table 4-1 conditions the engine can tell
static uint32_t AttackConditionsFor(const Actor* attacker, const Actor* target, bool missile, unsigned int range)
{
	uint32_t conditions = ATK_NONE;
	ieDword state = target->GetStat(IE_STATE_ID);
	if (attacker->IsInvisibleTo(target)) conditions |= ATK_INVISIBLE;
	if (attacker->GetStat(IE_ENTANGLE)) conditions |= ATK_ENTANGLED;
	if (state & STATE_BLIND) conditions |= DEF_BLINDED;
	if (target->GetStat(IE_ENTANGLE)) conditions |= DEF_ENTANGLED;
	if (target->GetStat(IE_DCC_PRONE)) {
		conditions |= DEF_PRONE;
	} else if (target->Immobile() || (state & (STATE_HELPLESS | STATE_SLEEP)) || target->IsDCCHeld()) {
		conditions |= DEF_HELPLESS;
	}
	if (missile && range) {
		// the IE range is the long range; the thirds are short, medium and long (equipment.md)
		if (!WithinPersonalRange(attacker, target, 2 * range / 3)) {
			conditions |= RANGE_LONG;
		} else if (!WithinPersonalRange(attacker, target, range / 3)) {
			conditions |= RANGE_MEDIUM;
		}
	}
	return conditions;
}

void Actor::DCCAttack(Actor* target, bool viaProjectile)
{
	const RulesData* rules = Rules();
	if (!rules || !target) return;
	dccLastAttack.clear();
	++dccAttackSerial;

	// immobile, witless, unconscious, dying, prone or a crit's "can't attack"
	if (IsDCCHeld() || Immobile() || GetStat(IE_DCC_NOATTACK) || GetStat(IE_DCC_PRONE)) {
		Feedback(fmt::format(u"{} can't attack", Name(this)), FT_COMBAT);
		return;
	}
	if (!viaProjectile) {
		int deedDie = GetStat(IE_DCC_DEEDDIE);
		dccDeedRoll = deedDie ? DCCDie(deedDie) : 0;
	}

	bool character = IsDCCCharacter();
	bool twoWeapons = IsDualWielding() && weaponInfo[1].extHeader;
	bool halfling = character && BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Halfling) && BaseStats[IE_LEVEL] > 0;
	int index = viaProjectile ? std::max(0, int(attacksperround) - int(attackcount) - 1) : 0;
	TwoWeaponDice twoDice;
	if (twoWeapons) twoDice = rules->combat.twoWeapons.Lookup(Signed(GetStat(IE_DEX)), halfling);

	std::vector<DCCSwing> swings(twoWeapons ? 2 : 1);
	for (int hand = 0; hand < int(swings.size()); ++hand) {
		DCCSwing& swing = swings[hand];
		swing.hand = hand;
		swing.wi = &weaponInfo[hand];
		const ITMExtHeader* header = swing.wi->extHeader;
		if (!header) return;
		swing.missile = swing.wi->wflags & WEAPON_RANGED;
		bool fists = inventory.FistsEquipped();

		uint32_t augurFlags = swing.missile ? AF_MISSILE : AF_MELEE;
		if (fists) augurFlags |= AF_UNARMED;
		swing.abilityMod = character ? GetDCCAbilityMod(swing.missile ? Ability::Agi : Ability::Str) : 0;
		int deedDie = GetStat(IE_DCC_DEEDDIE);
		swing.deed = deedDie ? dccDeedRoll + Signed(GetStat(IE_DCC_DEEDADD)) : 0;
		int augur = character ? GetDCCAugurBonus(AugurRoll::Attack, augurFlags) : 0;

		// stock to-hit effects and magic weapons count as flat bonuses
		int effects = ToHit.GetFxBonus() + Signed(GetStat(IE_HITBONUS)) + header->THAC0Bonus + swing.wi->launcherTHAC0Bonus;
		effects += Signed(GetStat(swing.missile ? IE_MISSILEHITBONUS : (fists ? IE_FISTHIT : IE_MELEETOHIT)));
		effects += Signed(GetStat(IE_DCC_ATKMOD)) + Signed(GetStat(IE_DCC_ROLLMOD));

		uint32_t conditions = AttackConditionsFor(this, target, swing.missile, GetWeaponRange(hand));
		Situational situational = SituationalModifiers(conditions, swing.missile);

		swing.in.actionDie = ActionDie(this, index);
		swing.in.dieSteps = situational.dieSteps;
		swing.in.modifier = Signed(GetStat(IE_DCC_ATTACK)) + swing.abilityMod + swing.deed + augur + situational.flat + effects;
		swing.in.targetAC = Signed(target->GetStat(IE_DCC_AC));
		swing.in.threat = Signed(GetStat(IE_DCC_THREAT));
		swing.in.cursed = GetDCCStates() & STATE_CURSED;
		if (twoWeapons) {
			swing.in.dieSteps += hand ? twoDice.offSteps : twoDice.primarySteps;
			swing.in.critRule = hand ? twoDice.offCrit : twoDice.primaryCrit;
			swing.in.threat = 20; // the expanded threat range is lost with two weapons
			// halflings only fumble when both dice show 1 (classes.md); decided after both rolls
			swing.in.fumbles = !halfling;
		}

		// damage: the weapon's dice + Str (melee and thrown) + deed + augur + magic + effects
		bool strength = !swing.missile || (swing.wi->wflags & (WEAPON_USESTRENGTH | WEAPON_USESTRENGTH_DMG));
		int damageAugur = character ? GetDCCAugurBonus(AugurRoll::Damage, augurFlags) : 0;
		int strMod = character && strength ? GetDCCAbilityMod(Ability::Str) : 0;
		swing.damageBonus = strMod + swing.deed + damageAugur + header->DamageBonus + swing.wi->launcherDmgBonus + Signed(GetStat(IE_DAMAGEBONUS));
	}

	// one-shot modifiers are in the totals now: the attacker's "next attack" ones and the
	// target's "next attack against" ones end (a fumble below may add new ones)
	fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::NextAttack));
	target->fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::NextDefense));

	// roll both dice together, so the halfling double-1 rule can see them
	for (auto& swing : swings) {
		swing.record.natural = DCCDie(AttackDie(swing.in.actionDie, swing.in.dieSteps));
	}
	if (twoWeapons && halfling && swings[0].record.natural == 1 && swings[1].record.natural == 1) {
		swings[0].in.fumbles = true; // one fumble roll for the pair
	}

	int totalDamage = 0;
	int hitHand = -1;
	for (auto& swing : swings) {
		DCCResolveSwing(swing, target, 0);
		if (swing.record.hit) {
			totalDamage += swing.damage;
			if (hitHand < 0) hitHand = swing.hand;
		}
	}

	// the damage: on the weapon's projectile like a stock attack, or at once
	if (viaProjectile) {
		const WeaponInfo& wi = weaponInfo[hitHand < 0 ? 0 : hitHand];
		int header = wi.wflags & WEAPON_RANGED ? -2 : -1;
		if (hitHand < 0) {
			if (wi.wflags & WEAPON_RANGED) UseItem(wi.slot, -2, target, UI_MISS | UI_NOAURA);
		} else {
			bool crit = swings[hitHand].record.crit;
			UseItem(wi.slot, header, target, (crit ? UI_CRITICAL : 0) | UI_NOAURA, totalDamage);
		}
	} else if (hitHand >= 0) {
		const ITMExtHeader* header = weaponInfo[hitHand].extHeader;
		auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, totalDamage, ConvertDamageType(header ? header->DamageType : 0) << 16, FX_DURATION_INSTANT_LIMITED);
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), target, this);
	}

}

// ---------------------------------------------------------------------------
// crit and fumble results

namespace {

struct RowContext {
	Actor* attacker = nullptr;
	Actor* target = nullptr; // the defender
	Actor* subject = nullptr; // who the result happens to: the target of a crit, the attacker of a fumble
	bool crit = true;
	int level = 0; // the attacker's level or HD, for DCs
	const SaveSpec* save = nullptr;
	int saved = -1; // -1 not rolled yet, 0 failed, 1 passed
	int extraDamage = 0; // added to this hit (crits)
	std::vector<String> notes;
	std::vector<std::pair<std::string, std::vector<std::string>>> extraAttacks; // EXTRA terms, run after the row
};

}

static int AbilityIndex(const std::string& key)
{
	static const char* const keys[] = { "STR", "AGI", "STA", "PER", "INT", "LCK" };
	for (int i = 0; i < ABILITY_COUNT; ++i) {
		if (key == keys[i]) return i;
	}
	return -1;
}

static bool ConditionHolds(const std::string& condition, const RowContext& ctx)
{
	if (condition.empty()) return true;
	const Actor* who = ctx.subject;
	if (condition == "NOHELM") return !who->inventory.GetSlotItem(Inventory::GetHeadSlot());
	if (condition == "NOSHIELD") return !ShieldItem(who);
	if (condition == "ARMORED") return ArmorItem(who) != nullptr;
	if (condition == "HASWEAPON") return HasWeapon(who, false);
	return false;
}

// whether a term can happen at all (the next lower crit result is used otherwise)
static bool TermApplies(const TableTerm& term, const RowContext& ctx)
{
	if (!ConditionHolds(term.condition, ctx)) return false;
	const std::string& key = term.key;
	if (key == "DISARM" || key == "SUNDER") return HasWeapon(ctx.target, true);
	if (key == "WEAPONDMG") return HasWeapon(ctx.target, false) && ctx.target->weaponInfo[0].extHeader;
	if (key == "DISARMSELF" || key == "SUNDERSELF") return HasWeapon(ctx.attacker, true);
	if (key == "SHIELD") return ShieldItem(ctx.target) != nullptr;
	return true;
}

// a crit entry can't happen when it has no damage of its own and none of its effects can
// (results the engine doesn't have yet, TODO:..., don't count either way)
static bool RowApplies(const ResultRow& row, const RowContext& ctx)
{
	if (!row.dice.IsNone()) return true;
	bool any = false;
	for (const auto& alternatives : row.effects) {
		if (alternatives.size() == 1 && alternatives[0].key == "TODO") continue;
		any = true;
		for (const auto& term : alternatives) {
			if (TermApplies(term, ctx)) return true;
		}
	}
	return !any;
}

static Duration TermDuration(const TableTerm& term, size_t arg)
{
	Duration duration;
	if (!ParseDuration(arg < term.args.size() ? term.args[arg] : std::string(), duration)) {
		duration = Duration();
		duration.dice.add = 1;
	}
	return duration;
}

// applies a stock or DCC:Combat effect for a table duration; returns the rolled amount for the log
static String ApplyFor(EffectRef& ref, ieDword p1, ieDword p2, const Duration& duration, Actor* who, Actor* caster, ieDword p3 = 0)
{
	std::unique_ptr<Effect> fx;
	String what;
	if (duration.kind == Duration::Kind::Permanent || duration.kind == Duration::Kind::UntilHealed) {
		fx = EffectQueue::CreateEffect(ref, p1, p2, FX_DURATION_INSTANT_PERMANENT_AFTER_BONUSES);
		fx->SourceRef = duration.kind == Duration::Kind::Permanent ? PermanentSource : UntilHealedSource;
		what = duration.kind == Duration::Kind::Permanent ? u"for good" : u"until healed";
	} else {
		int amount = RollDice(duration.dice, [caster](int sides) { return caster->DCCDie(sides); });
		amount = std::max(1, amount);
		fx = EffectQueue::CreateEffect(ref, p1, p2, FX_DURATION_INSTANT_LIMITED);
		fx->Duration = DurationSeconds(duration.kind, amount, core->Time.round_sec);
		static const char16_t* units[] = { u"round", u"turn", u"hour", u"day", u"week" };
		what = fmt::format(u"for {} {}{}", amount, units[static_cast<int>(duration.kind)], amount == 1 ? u"" : u"s");
	}
	if (!fx) return what;
	fx->Parameter3 = p3;
	fx->Target = FX_TARGET_PRESET;
	core->ApplyEffect(std::move(fx), who, caster);
	return what;
}

static String ApplyCombat(CombatEffect kind, int value, const Duration& duration, Actor* who, Actor* caster)
{
	return ApplyFor(fx_dcc_combat_ref, static_cast<ieDword>(value), static_cast<ieDword>(kind), duration, who, caster);
}

static void DirectDamage(Actor* who, int damage, Actor* hitter)
{
	if (damage <= 0) return;
	auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, damage, DAMAGE_CRUSHING << 16, FX_DURATION_INSTANT_LIMITED);
	fx->Target = FX_TARGET_PRESET;
	core->ApplyEffect(std::move(fx), who, hitter);
}

// the creature on the attacker's side nearest to (or random around) a point, not the attacker
static Actor* FindAlly(const Actor* attacker, const Actor* near, unsigned int radius, bool random, Actor* roller)
{
	const Map* map = attacker->GetCurrentArea();
	if (!map) return nullptr;
	bool good = attacker->GetStat(IE_EA) <= EA_GOODCUTOFF;
	std::vector<Actor*> allies;
	for (Actor* other : map->GetAllActorsInRadius(near->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED, radius)) {
		if (other == attacker || other == near) continue;
		if ((other->GetStat(IE_EA) <= EA_GOODCUTOFF) != good) continue;
		if (other->GetDCCStates() & STATE_DYING) continue;
		allies.push_back(other);
	}
	if (allies.empty()) return nullptr;
	if (random) return allies[roller->DCCDie(int(allies.size())) - 1];
	return *std::min_element(allies.begin(), allies.end(), [near](const Actor* a, const Actor* b) {
		return SquaredDistance(a->Pos, near->Pos) < SquaredDistance(b->Pos, near->Pos);
	});
}

// the attacker's own weapon damage: its dice + Str (fumbles that hit yourself or an ally)
static int WeaponDamageRoll(Actor* attacker, const WeaponInfo& wi)
{
	const ITMExtHeader* header = wi.extHeader;
	if (!header) return 1;
	DiceExpr dice = WeaponDice(header);
	int str = attacker->IsDCCCharacter() ? attacker->GetDCCAbilityMod(Ability::Str) : 0;
	return AttackDamage(RollDice(dice, [attacker](int sides) { return attacker->DCCDie(sides); }), str);
}

static bool FailedSave(RowContext& ctx)
{
	if (!ctx.save || ctx.save->IsNone()) return true;
	if (ctx.saved < 0) {
		int dc = ctx.save->dc + (ctx.save->plusLevel ? ctx.level : 0);
		RollResult roll = ctx.subject->DCCRollSave(ctx.save->save, 0, dc, ctx.subject->DCCDie(20));
		ctx.saved = roll.success ? 1 : 0;
		static const char16_t* names[] = { u"Fort", u"Ref", u"Will" };
		ctx.notes.push_back(fmt::format(u"{} {} save {}{} = {} vs DC {}: {}", Name(ctx.subject), names[static_cast<int>(ctx.save->save)],
						roll.natural, Plus(roll.total - roll.natural), roll.total, dc, roll.success ? u"saved" : u"failed"));
	}
	return ctx.saved == 0;
}

static void ApplyTerm(const TableTerm& term, RowContext& ctx, const WeaponInfo& wi)
{
	if (term.gate == '?' && !FailedSave(ctx)) return;
	if (term.gate == '!' && FailedSave(ctx)) return;

	Actor* who = ctx.subject;
	Actor* attacker = ctx.attacker;
	auto roller = [attacker](int sides) { return attacker->DCCDie(sides); };
	auto arg = [&term](size_t i) { return i < term.args.size() ? term.args[i] : std::string(); };
	auto argInt = [&arg](size_t i, int fallback) {
		int value = fallback;
		valid_signednumber(arg(i).c_str(), value);
		return value;
	};
	auto rollDice = [&](const std::string& text) {
		DiceExpr dice;
		ParseDice(text, dice);
		return RollDice(dice, roller);
	};
	const std::string& key = term.key;
	String note;

	if (key == "NONE") {
		return;
	} else if (key == "DMG") {
		int extra = rollDice(arg(0));
		ctx.extraDamage += extra;
		note = fmt::format(u"+{} = {} damage", Widen(arg(0)), extra);
	} else if (key == "PRONE") {
		note = u"prone " + ApplyCombat(CombatEffect::Prone, 1, TermDuration(term, 0), who, attacker);
	} else if (key == "NOATTACK") {
		note = u"can't attack " + ApplyCombat(CombatEffect::NoAttack, 1, TermDuration(term, 0), who, attacker);
	} else if (key == "INITBOTTOM") {
		note = u"acts last " + ApplyCombat(CombatEffect::InitBottom, 1, TermDuration(term, 0), who, attacker);
	} else if (key == "STUN") {
		note = u"stunned " + ApplyFor(fx_dcc_stun_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "BLIND") {
		note = u"blinded " + ApplyFor(fx_dcc_blind_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "DEAF") {
		note = u"deafened " + ApplyFor(fx_dcc_deaf_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "MUTE") {
		note = u"can't speak or cast " + ApplyFor(fx_dcc_silence_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "UNCON") {
		note = u"unconscious " + ApplyFor(fx_dcc_sleep_ref, 0, 1, TermDuration(term, 0), who, attacker);
	} else if (key == "PARALYZE" || key == "HELD") {
		note = (key == "HELD" ? u"can't move or attack " : u"paralyzed ") + ApplyFor(fx_dcc_hold_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "SPEED") {
		std::string what = arg(0);
		int percent = 100;
		if (what == "HALF") {
			percent = 50;
		} else if (what == "ZERO") {
			percent = 0;
		} else {
			percent = std::max(0, (30 + argInt(0, 0)) * 100 / 30); // feet off a 30' move
		}
		note = fmt::format(u"speed {}% ", percent) + ApplyFor(fx_dcc_movement_ref, percent, 2, TermDuration(term, 1), who, attacker);
	} else if (key == "ATK") {
		int value = argInt(0, 0);
		note = fmt::format(u"{:+} to attack ", value) + ApplyCombat(CombatEffect::AttackMod, value, TermDuration(term, 1), who, attacker);
	} else if (key == "AC") {
		int value = argInt(0, 0);
		note = fmt::format(u"{:+} AC ", value) + ApplyCombat(CombatEffect::ACMod, value, TermDuration(term, 1), who, attacker);
	} else if (key == "NEXTATK") {
		int value = argInt(0, 0);
		Duration turn;
		turn.kind = Duration::Kind::Turns;
		turn.dice.add = 1;
		ApplyCombat(CombatEffect::NextAttack, value, turn, who, attacker);
		note = fmt::format(u"{:+} on the next attack", value);
	} else if (key == "ENEMYBONUS") {
		int value = argInt(0, 0);
		Duration turn;
		turn.kind = Duration::Kind::Turns;
		turn.dice.add = 1;
		ApplyCombat(CombatEffect::NextDefense, -value, turn, who, attacker);
		note = fmt::format(u"the next attack against {} gets {:+}", Name(who), value);
	} else if (key == "DIEIN") {
		note = u"dies unless healed, " + ApplyCombat(CombatEffect::Doomed, 1, TermDuration(term, 0), who, attacker);
	} else if (key == "DIE") {
		auto fx = EffectQueue::CreateEffect(fx_dcc_death_ref, 0, 4, FX_DURATION_INSTANT_PERMANENT);
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), who, attacker);
		note = u"dies";
	} else if (key == "HALFHP") {
		int lost = Signed(who->GetBase(IE_HITPOINTS)) / 2;
		ctx.extraDamage += lost;
		note = fmt::format(u"loses half its hp ({})", lost);
	} else if (key == "ABIL") {
		int ability = AbilityIndex(arg(0));
		if (ability < 0) return;
		int points = std::max(1, rollDice(arg(1)));
		static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
		if (arg(2) == "PERM") {
			unsigned int stat = AbilityStat(static_cast<Ability>(ability));
			who->SetBase(stat, std::max(0, Signed(who->GetBase(stat)) - points));
			note = fmt::format(u"-{} {} for good", points, names[ability]);
		} else {
			auto fx = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, ability, FX_DURATION_INSTANT_PERMANENT);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), who, attacker);
			note = fmt::format(u"-{} {}", points, names[ability]);
		}
	} else if (key == "DISARM" || key == "DISARMSELF") {
		Actor* holder = key == "DISARM" ? ctx.target : attacker;
		if (!HasWeapon(holder, true)) return;
		holder->DropItem(holder->inventory.GetEquippedSlot(), 0);
		note = fmt::format(u"{} drops its weapon", Name(holder));
	} else if (key == "SUNDER" || key == "SUNDERSELF") {
		Actor* holder = key == "SUNDER" ? ctx.target : attacker;
		if (!HasWeapon(holder, true)) return;
		int slot = holder->inventory.GetEquippedSlot();
		const CREItem* item = holder->inventory.GetSlotItem(slot);
		const Item* itm = item ? gamedata->GetItem(item->ItemResRef, true) : nullptr;
		bool magic = itm && ((itm->Flags & IE_ITEM_MAGICAL) || (item->Flags & IE_INV_ITEM_MAGICAL));
		if (itm) gamedata->FreeItem(itm, item->ItemResRef);
		if (magic) {
			// magic weapons never break (footnote *): knocked away instead, or untouched by a fumble
			if (key == "SUNDER") {
				holder->DropItem(slot, 0);
				note = fmt::format(u"{}'s magic weapon is knocked away", Name(holder));
			}
		} else {
			DestroySlotItem(holder, slot);
			note = fmt::format(u"{}'s weapon breaks", Name(holder));
		}
	} else if (key == "SHIELD") {
		if (!ShieldItem(ctx.target)) return;
		DestroySlotItem(ctx.target, ctx.target->inventory.GetShieldSlot());
		note = fmt::format(u"{}'s shield is destroyed", Name(ctx.target));
	} else if (key == "WEAPONDMG") {
		const ITMExtHeader* header = ctx.target->weaponInfo[0].extHeader;
		if (!header) return;
		int times = argInt(0, 1);
		DiceExpr dice = WeaponDice(header);
		int extra = times * std::max(1, RollDice(dice, roller));
		ctx.extraDamage += extra;
		note = fmt::format(u"{}x its own weapon: +{} damage", times, extra);
	} else if (key == "ALLY") {
		Actor* ally = FindAlly(attacker, ctx.target, 100, false, attacker);
		if (!ally) return;
		int damage = std::max(1, rollDice(arg(0)));
		DirectDamage(ally, damage, attacker);
		note = fmt::format(u"{} is caught for {}", Name(ally), damage);
	} else if (key == "SELF") {
		int damage = std::max(1, rollDice(arg(0)));
		DirectDamage(who, damage, who);
		note = fmt::format(u"{} takes {}", Name(who), damage);
	} else if (key == "SELFHIT") {
		int damage = WeaponDamageRoll(attacker, wi) + argInt(0, 0);
		DirectDamage(attacker, damage, attacker);
		note = fmt::format(u"{} hits itself for {}", Name(attacker), damage);
	} else if (key == "HITALLY") {
		Actor* ally = FindAlly(attacker, attacker, std::max(attacker->GetWeaponRange(false), 10u) + 30, true, attacker);
		if (!ally) {
			note = u"no ally in reach";
		} else {
			// the same action die, against the ally's AC
			int natural = attacker->DCCDie(20);
			int modifier = Signed(attacker->GetStat(IE_DCC_ATTACK));
			int ac = Signed(ally->GetStat(IE_DCC_AC));
			bool hit = natural > 1 && natural + modifier >= ac;
			note = fmt::format(u"swings at {}: {}{} = {} vs AC {}", Name(ally), natural, Plus(modifier), natural + modifier, ac);
			if (hit) {
				int damage = WeaponDamageRoll(attacker, wi);
				DirectDamage(ally, damage, attacker);
				note += fmt::format(u", hit for {}", damage);
			} else {
				note += u", miss";
			}
		}
	} else if (key == "DELAYDMG") {
		int damage = std::max(1, rollDice(arg(0)));
		auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, damage, DAMAGE_CRUSHING << 16, FX_DURATION_DELAY_PERMANENT);
		fx->Duration = argInt(1, 1) * core->Time.round_sec;
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), who, attacker);
		note = fmt::format(u"{} more damage next round", damage);
	} else if (key == "EXTRA") {
		ctx.extraAttacks.emplace_back(key, term.args);
		return;
	} else if (key == "TODO") {
		std::string what = arg(0);
		std::replace(what.begin(), what.end(), '_', ' ');
		std::transform(what.begin(), what.end(), what.begin(), [](unsigned char c) { return std::tolower(c); });
		note = u"(not in the engine yet: " + Widen(what) + u")";
	} else {
		Log(WARNING, "DCC", "Unknown crit/fumble result '{}'", key);
		return;
	}
	if (!note.empty()) ctx.notes.push_back(std::move(note));
}

static void ApplyRow(const ResultRow& row, RowContext& ctx, const WeaponInfo& wi)
{
	ctx.save = &row.save;
	if (!row.dice.IsNone()) {
		int extra = RollDice(row.dice, [&ctx](int sides) { return ctx.attacker->DCCDie(sides); });
		ctx.extraDamage += extra;
		ctx.notes.push_back(fmt::format(u"+{} = {} damage", Widen(DiceString(row.dice)), extra));
	}
	for (const auto& alternatives : row.effects) {
		for (const auto& term : alternatives) {
			if (!TermApplies(term, ctx)) continue;
			ApplyTerm(term, ctx, wi);
			break;
		}
	}
}

static String Joined(const std::vector<String>& notes)
{
	String text;
	for (const auto& note : notes) {
		text += (text.empty() ? u"" : u"; ") + note;
	}
	return text;
}

void Actor::DCCResolveSwing(DCCSwing& swing, Actor* target, int depth)
{
	const RulesData* rules = Rules();
	AttackRecord& record = swing.record;
	record.serial = dccAttackSerial;
	record.hand = swing.hand;
	record.die = AttackDie(swing.in.actionDie, swing.in.dieSteps);
	record.deed = GetStat(IE_DCC_DEEDDIE) ? dccDeedRoll : 0;
	if (record.natural <= 0) record.natural = DCCDie(record.die);
	RollResult roll = ResolveAttack(swing.in, record.natural);
	// M2.4 seam: a Luck burn (per-character policy, or a prompt while paused) can raise roll.total
	// here and turn a miss into a hit; the deed die's Mighty Deed (signature deed) also hooks in
	// below, once the hit is known (deed die 3+ and a hit)
	record.modifier = swing.in.modifier;
	record.total = roll.total;
	record.ac = roll.target;
	record.hit = roll.success;
	record.crit = roll.critical;
	record.fumble = roll.fumble;
	record.cursed = roll.cursed;

	// "Brak attacks Goblin: d20 14 + d3 2 +2 = 18 vs AC 12, hit: 1d8+3 = 7 damage"
	String dieText = fmt::format(u"d{} {}", record.die, record.natural);
	int flat = record.modifier;
	if (record.deed && swing.deed) {
		dieText += fmt::format(u" + d{} {}", GetStat(IE_DCC_DEEDDIE), record.deed);
		flat -= record.deed;
	}
	String line = fmt::format(u"{} {}{} {}: {}{} = {} vs AC {}, ", Name(this), depth ? u"free attack on" : u"attacks", swing.hand ? u" (off hand)" : u"",
				  Name(target), dieText, Plus(flat), record.total, record.ac);
	if (record.hit) {
		line += record.crit ? u"critical hit" : u"hit";
	} else if (record.cursed) {
		line += u"miss (cursed by fortune)";
	} else {
		line += record.natural == 1 ? u"miss (natural 1)" : u"miss";
	}

	const WeaponInfo& wi = *swing.wi;
	if (record.hit) {
		const ITMExtHeader* header = wi.extHeader;
		DiceExpr dice = WeaponDice(header);
		dice.add = 0; // the header's bonus is in damageBonus
		int rolled = RollDice(dice, [this](int sides) { return DCCDie(sides); });
		swing.damage = AttackDamage(rolled, swing.damageBonus);
		line += fmt::format(u": {}{} = {} damage", Widen(DiceString(dice)), Plus(swing.damageBonus), swing.damage);
		if (!swing.extraOnHit.IsNone()) {
			int extra = RollDice(swing.extraOnHit, [this](int sides) { return DCCDie(sides); });
			swing.damage += extra;
			line += fmt::format(u" +{} = {}", Widen(DiceString(swing.extraOnHit)), extra);
		}
		if (swing.deed && dccDeedRoll >= 3) {
			// M2.4 seam: the Mighty Deed of Arms succeeds here (deed die 3+ on a hit)
			line += u" (deed die 3+)";
		}
	}
	Feedback(std::move(line), FT_TOHIT);

	RowContext ctx;
	ctx.attacker = this;
	ctx.target = target;
	ctx.level = GetDCCHitDice();

	if (record.crit) {
		CritTableID tableID = static_cast<CritTableID>(GetStat(IE_DCC_CRITTABLE));
		const ResultTable* table = rules->combat.Crit(tableID);
		int dice = GetStat(IE_DCC_CRITDICE);
		int die = GetStat(IE_DCC_CRITDIE);
		if (table && dice && die) {
			DiceExpr critDice;
			critDice.count = dice;
			critDice.sides = die;
			int rolled = RollDice(critDice, [this](int sides) { return DCCDie(sides); });
			int add = Signed(GetStat(IE_DCC_CRITADD));
			// Luck: the attacker's (doubled by the warrior's arm augur), less a PC defender's
			int luck = IsDCCCharacter() ? GetDCCAbilityMod(Ability::Luck) + GetDCCAugurBonus(AugurRoll::CritTable) : 0;
			int defender = target->IsDCCCharacter() ? target->GetDCCAbilityMod(Ability::Luck) : 0;
			record.critRoll = CritRoll(rolled, add, luck, defender);
			record.critTable = static_cast<int>(tableID);
			ctx.subject = target;
			ctx.crit = true;
			const ResultRow* row = table->Lookup(record.critRoll);
			// an entry that can't happen (no weapon, no shield) gives the next lower one
			while (row && !RowApplies(*row, ctx) && row->roll > table->MinRoll()) {
				row = table->Lookup(row->roll - 1);
			}
			String mods = Plus(add + luck - defender);
			if (row) {
				record.result = row->text;
				ApplyRow(*row, ctx, wi);
			}
			Feedback(fmt::format(u"CRITICAL ({}: {}d{}{} = {}): {}{}{}", Widen(CritTableName(tableID)), dice, die, mods, record.critRoll, Widen(record.result),
					     ctx.notes.empty() ? u"" : u"; ", Joined(ctx.notes)),
				 FT_COMBAT);
			swing.damage += ctx.extraDamage;
			swing.extraDamage = ctx.extraDamage;
		}
	} else if (record.fumble) {
		if (!IsDCCCharacter()) {
			// Ethren decision: monsters' natural 1s miss without a fumble roll (the table is about gear)
			Feedback(fmt::format(u"Fumble: {} misses", Name(this)), FT_COMBAT);
		} else {
			const CREItem* armor = ArmorItem(this);
			const CREItem* shield = ShieldItem(this);
			std::string animation;
			if (armor) {
				const Item* itm = gamedata->GetItem(armor->ItemResRef, true);
				if (itm) {
					animation = itm->AnimationType.c_str();
					gamedata->FreeItem(itm, armor->ItemResRef);
				}
			}
			int armorDie = armor ? rules->combat.armorFumbles.ArmorDie(armor->ItemResRef.c_str(), animation) : 0;
			int shieldDie = shield ? rules->combat.armorFumbles.ShieldDie(shield->ItemResRef.c_str()) : 0;
			record.fumbleDie = FumbleDie(armorDie, shieldDie);
			int rolled = DCCDie(record.fumbleDie);
			int luck = GetDCCAbilityMod(Ability::Luck) + GetDCCAugurBonus(AugurRoll::Fumble);
			record.fumbleRoll = FumbleRoll(rolled, luck);
			// M2.4 seam: warriors and dwarves may burn 1 Luck here to skip the fumble table
			ctx.subject = this;
			ctx.crit = false;
			const ResultRow* row = rules->combat.fumbles.Lookup(record.fumbleRoll);
			if (row) {
				record.result = row->text;
				ApplyRow(*row, ctx, wi);
			}
			Feedback(fmt::format(u"Fumble (d{} {}{} = {}): {}{}{}", record.fumbleDie, rolled, Plus(-luck), record.fumbleRoll, Widen(record.result),
					     ctx.notes.empty() ? u"" : u"; ", Joined(ctx.notes)),
				 FT_COMBAT);
		}
	}
	record.damage = record.hit ? swing.damage : 0;
	dccLastAttack.push_back(record);

	// free attacks a crit grants: same die and hand, at once, never more than one deep
	for (const auto& extra : ctx.extraAttacks) {
		if (depth > 0) break;
		if ((target->GetStat(IE_STATE_ID) & STATE_DEAD) || (target->GetDCCStates() & STATE_DYING)) break;
		DCCSwing free = swing;
		free.record = AttackRecord();
		free.damage = 0;
		free.extraDamage = 0;
		int bonus = 0;
		if (!extra.second.empty()) valid_signednumber(extra.second[0].c_str(), bonus);
		free.in.modifier += bonus;
		free.extraOnHit = DiceExpr();
		if (extra.second.size() > 1) ParseDice(extra.second[1], free.extraOnHit);
		DCCResolveSwing(free, target, depth + 1);
		if (free.record.hit) {
			const ITMExtHeader* header = wi.extHeader;
			auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, free.damage, ConvertDamageType(header ? header->DamageType : 0) << 16, FX_DURATION_INSTANT_LIMITED);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), target, this);
		}
	}
}

// ---------------------------------------------------------------------------
// dying

bool Actor::DCCStartBleeding()
{
	if (!Rules()) return false;
	if ((InternalFlags & IF_REALLYDIED) || (Modified[IE_STATE_ID] & STATE_DEAD)) return false;
	// already bleeding: further damage doesn't hurry it (Ethren decision)
	if (fxqueue.HasEffectWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Bleed))) return true;
	if (!IsDCCCharacter()) return false;
	int rounds = BleedOutRounds(Signed(BaseStats[IE_LEVEL]));
	if (rounds <= 0) return false;

	// the blow that drops them doesn't chunk them; they are still alive
	LastDamageType &= ~DAMAGE_CHUNKING;
	auto fx = EffectQueue::CreateEffect(fx_dcc_combat_ref, 0, static_cast<ieDword>(CombatEffect::Bleed), FX_DURATION_INSTANT_LIMITED);
	fx->Duration = rounds * core->Time.round_sec;
	fx->Target = FX_TARGET_PRESET;
	core->ApplyEffect(std::move(fx), this, this);
	Feedback(fmt::format(u"{} falls, bleeding out: {} round{} to heal", Name(this), rounds, rounds == 1 ? u"" : u"s"), FT_COMBAT);
	return true;
}

void Actor::DCCHealed(int oldHP, int newHP)
{
	if (newHP <= oldHP || newHP <= 0) return;
	if (fxqueue.HasEffectWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Bleed))) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Bleed));
		Modified[IE_DCC_BLEED] = 0;
		// survivors keep a scar and lose 1 Stamina for good (combat.md)
		SetBase(IE_CON, std::max(0, Signed(BaseStats[IE_CON]) - 1));
		SetStance(IE_ANI_GET_UP);
		Feedback(fmt::format(u"{} is saved from bleeding out (-1 Stamina for good)", Name(this)), FT_COMBAT);
	}
	if (fxqueue.HasEffectWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Doomed))) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Doomed));
		Modified[IE_DCC_DOOMED] = 0;
		Feedback(fmt::format(u"{} is healed in time", Name(this)), FT_COMBAT);
	}
}

void Actor::DCCCountdownOver(CombatEffect kind)
{
	if ((InternalFlags & IF_REALLYDIED) || (Modified[IE_STATE_ID] & STATE_DEAD)) return;
	Feedback(Name(this) + (kind == CombatEffect::Bleed ? u" bleeds out" : u" succumbs to the wound"), FT_COMBAT);
	Die(nullptr);
}

void Actor::DCCOnDeath()
{
	if (!IsDCCCharacter()) return;
	const Game* game = core->GetGame();
	BaseStats[IE_DCC_DIEDAT] = game ? std::max<ieDword>(game->GameTime, 1) : 1;
	BaseStats[IE_DCC_BODY] = 0;
}

int Actor::DCCRecoverBody(const Actor* ally, int natural)
{
	const Game* game = core->GetGame();
	if (!Rules() || !game || !(Modified[IE_STATE_ID] & STATE_DEAD) || !IsDCCCharacter()) return -1;
	if (ally && (ally->GetStat(IE_STATE_ID) & STATE_DEAD)) return -1;
	if ((LastDamageType & DAMAGE_CHUNKING) || !BaseStats[IE_DCC_DIEDAT]) return -1;
	int seconds = static_cast<int>((game->GameTime - BaseStats[IE_DCC_DIEDAT]) / core->Time.defaultTicksPerSec);
	if (!CanRecoverBody(Signed(BaseStats[IE_LEVEL]), seconds, BaseStats[IE_DCC_BODY])) return -1;

	BaseStats[IE_DCC_BODY] = 1;
	// the dead character's own Luck (combat.md)
	RollResult roll = DCCRollLuckCheck(natural > 0 ? natural : DCCDie(20));
	String line = fmt::format(u"{} rolls {} over: Luck check {} vs {}: ", Name(ally), Name(this), roll.natural, roll.target);
	if (!roll.success) {
		Feedback(line + u"dead", FT_COMBAT);
		return 0;
	}

	// only knocked out: 1 hp, -4 to all rolls for an hour and 1 point of Str, Agi or Sta for good
	Resurrect(Point());
	SetBase(IE_HITPOINTS, 1);
	static const unsigned int stats[] = { IE_STR, IE_DEX, IE_CON };
	static const char16_t* names[] = { u"Str", u"Agi", u"Sta" };
	int which = DCCDie(3) - 1;
	SetBase(stats[which], std::max(0, Signed(BaseStats[stats[which]]) - 1));
	Duration hour;
	hour.kind = Duration::Kind::Hours;
	hour.dice.add = 1;
	ApplyCombat(CombatEffect::RollMod, BODY_RECOVERY_PENALTY, hour, this, this);
	BaseStats[IE_DCC_DIEDAT] = 0;
	Feedback(line + fmt::format(u"alive at 1 hp, -1 {} for good, {} to all rolls for an hour", names[which], BODY_RECOVERY_PENALTY), FT_COMBAT);
	return 1;
}

}
