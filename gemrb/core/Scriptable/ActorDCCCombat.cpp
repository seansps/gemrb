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
// M2.4 adds dice only on paths that opt in: a thief's Luck burn rolls its luck dice right
// after the attack die it helps (before any damage), and a Mighty Deed (deed die 3+ on a hit)
// rolls its result's dice after the crit's. A halfling's burn for an ally adds nothing, nor
// does a warrior's or dwarf's burn to cancel a fumble (the fumble die isn't rolled then).
//
// Luck prompts: a key roll of a party member (a miss a burn would turn into a crit, a fumble
// a warrior or dwarf can cancel) asks its owner how much Luck to burn (PromptManager, M1.8)
// when the character pauses on key rolls. The attack action then waits in dccPendingAttack:
// the swings are rolled, nothing is resolved or applied yet, and the game is paused. The
// answer (or the policy's default on a timeout) re-finds attacker and target by global ID
// and carries on from that swing; damage goes on the projectile only once every swing is done.

#include "damages.h"
#include "ie_stats.h"

#include "DisplayMessage.h"
#include "EffectQueue.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "Item.h"
#include "Map.h"
#include "Prompts.h"
#include "RNG.h"

#include "DCC/Combat.h"
#include "DCC/CombatTables.h"
#include "DCC/Deeds.h"
#include "DCC/Luck.h"
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
static EffectRef fx_dcc_push_ref = { "WingBuffet", -1 };
static EffectRef fx_dcc_morale_ref = { "MoraleModifier", -1 };
static EffectRef fx_dcc_dmgmod_ref = { "DamageBonusModifier", -1 };

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
	Actor::DCCFeedback(std::move(text), type);
}

void Actor::DCCFeedback(String text, int type)
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
	// the Luck burn on this roll, decided by the policy or a prompt before it resolves
	bool decided = false;
	int burnPoints = 0;
	int burnBonus = 0;
	ieDword burnBy = 0; // a halfling's global ID, 0: the attacker itself
	bool fumbleCancelled = false;
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

// an attack action from its rolls to its damage; waits here while a Luck prompt is open
struct Actor::DCCAttackState {
	std::vector<DCCSwing> swings;
	ieDword targetID = 0;
	bool viaProjectile = false;
	size_t next = 0; // the swing to resolve next
	Deed deed = Deed::None; // declared for this attack action
	bool deedTried = false; // one deed per attack action
	uint32_t promptID = 0;
};

// the IE item type of a weapon (itemtype.ids), for weapon-specific deeds
static int WeaponItemType(const Actor* actor, const WeaponInfo& wi)
{
	const CREItem* item = actor->inventory.GetSlotItem(wi.slot);
	if (!item) return -1;
	const Item* itm = gamedata->GetItem(item->ItemResRef, true);
	if (!itm) return -1;
	int type = itm->ItemType;
	gamedata->FreeItem(itm, item->ItemResRef);
	return type;
}

void Actor::DCCAttack(Actor* target, bool viaProjectile)
{
	const RulesData* rules = Rules();
	if (!rules || !target) return;
	// the last attack action still waits for a Luck prompt (the game is paused meanwhile)
	if (dccPendingAttack) {
		Log(COMBAT, "DCC", "{}'s attack waits for a Luck decision", fmt::WideToChar { GetName() });
		return;
	}
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

	auto state = std::make_shared<DCCAttackState>();
	state->targetID = target->GetGlobalID();
	state->viaProjectile = viaProjectile;
	std::vector<DCCSwing>& swings = state->swings;
	swings.resize(twoWeapons ? 2 : 1);
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

	// the Mighty Deed is declared before the roll: a queued one-off (used up now, whatever
	// happens), else the signature deed, else the weapon's own
	if (GetStat(IE_DCC_DEEDDIE)) {
		Deed queued = GetDCCQueuedDeed();
		if (queued != Deed::None) SetBase(IE_DCC_NEXTDEED, 0);
		state->deed = ChooseDeed(queued, GetDCCSignatureDeed(), WeaponDeed(WeaponItemType(this, weaponInfo[0])));
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

	DCCContinueAttack(state);
}

bool Actor::DCCContinueAttack(const std::shared_ptr<DCCAttackState>& state)
{
	Game* game = core->GetGame();
	Actor* target = game ? game->GetActorByGlobalID(state->targetID) : nullptr;
	if (!target || target->GetCurrentArea() != GetCurrentArea()) {
		// the target is gone (or moved on while we waited): the attack comes to nothing
		dccPendingAttack.reset();
		return true;
	}
	for (; state->next < state->swings.size(); ++state->next) {
		DCCSwing& swing = state->swings[state->next];
		if (!swing.decided && DCCAskAttackBurn(state, swing, target)) {
			dccPendingAttack = state;
			return false;
		}
		DCCResolveSwing(swing, target, 0, state.get());
	}
	dccPendingAttack.reset();

	int totalDamage = 0;
	int hitHand = -1;
	for (const auto& swing : state->swings) {
		if (swing.record.hit) {
			totalDamage += swing.damage;
			if (hitHand < 0) hitHand = swing.hand;
		}
	}

	// the damage: on the weapon's projectile like a stock attack, or at once
	if (state->viaProjectile) {
		const WeaponInfo& wi = weaponInfo[hitHand < 0 ? 0 : hitHand];
		int header = wi.wflags & WEAPON_RANGED ? -2 : -1;
		if (hitHand < 0) {
			if (wi.wflags & WEAPON_RANGED) UseItem(wi.slot, -2, target, UI_MISS | UI_NOAURA);
		} else {
			bool crit = state->swings[hitHand].record.crit;
			UseItem(wi.slot, header, target, (crit ? UI_CRITICAL : 0) | UI_NOAURA, totalDamage);
		}
	} else if (hitHand >= 0) {
		const ITMExtHeader* header = weaponInfo[hitHand].extHeader;
		auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, totalDamage, ConvertDamageType(header ? header->DamageType : 0) << 16, FX_DURATION_INSTANT_LIMITED);
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), target, this);
	}
	return true;
}

// what a burn would do for a swing: why, by how much it falls short, whether it would crit
struct BurnNeed {
	BurnReason reason = BurnReason::count; // count: nothing to burn for
	int gap = 0;
	bool wouldCrit = false;
};

static BurnNeed SwingBurnNeed(const Actor* attacker, const AttackInput& in, const RollResult& roll)
{
	BurnNeed need;
	if (roll.cursed && !roll.fumble) return need;
	if (roll.fumble) {
		if (CanCancelFumble(attacker->GetBase(IE_CLASS), Signed(attacker->GetBase(IE_LEVEL)))) need.reason = BurnReason::Fumble;
		return need;
	}
	// a natural 1 always misses; nothing to burn for a hit
	if (roll.success || roll.natural <= 1) return need;
	need.reason = BurnReason::Miss;
	need.gap = roll.target - roll.total;
	AttackInput burned = in;
	burned.modifier += need.gap;
	need.wouldCrit = ResolveAttack(burned, roll.natural).critical;
	return need;
}

static String BurnValueText(const BurnValue& value)
{
	if (value.IsDie()) return fmt::format(u"a d{} each", value.luckDie);
	return fmt::format(u"+{} each", value.perPoint);
}

bool Actor::DCCAskAttackBurn(const std::shared_ptr<DCCAttackState>& state, DCCSwing& swing, const Actor* target)
{
	// only party members are asked, if they pause on key rolls and can burn at all
	LuckPolicy policy = GetDCCLuckPolicy();
	if (!InParty || !IsDCCCharacter() || !(policy.flags & LUCK_PAUSE)) return false;
	int luck = GetDCCLuck();
	if (luck <= 0) return false;

	RollResult roll = ResolveAttack(swing.in, swing.record.natural);
	BurnNeed need = SwingBurnNeed(this, swing.in, roll);
	if (need.reason == BurnReason::count || !IsKeyRoll(need.reason, need.wouldCrit)) return false;
	BurnValue value = GetDCCBurnValue();
	// nothing Luck can do: even all of it doesn't reach
	if (need.reason == BurnReason::Miss && MaxBurnBonus(value, luck) < need.gap) return false;

	int suggested = PolicyBurn(policy, need.reason, need.gap, luck, value);
	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "luckburn";
	q.title = "Burn Luck?";
	std::string name = fmt::format("{}", fmt::WideToChar { GetShortName() });
	std::string other = fmt::format("{}", fmt::WideToChar { target->GetShortName() });
	if (need.reason == BurnReason::Fumble) {
		q.text = fmt::format("{} fumbles against {}. Burn 1 Luck (of {}) to shrug it off?", name, other, luck);
		q.fields = { { "Luck", 0, FUMBLE_CANCEL_COST, suggested } };
	} else {
		q.text = fmt::format("{} misses {} by {} with a natural {}, which would crit. Burn Luck ({}, {} left)?", name, other, need.gap,
				     swing.record.natural, fmt::WideToChar { BurnValueText(value) }, luck);
		q.fields = { { "Luck", 0, luck, suggested } };
	}
	// a short wait: the timeout falls back to the policy (the default)
	q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));

	ieDword selfID = GetGlobalID();
	size_t index = state->next;
	std::weak_ptr<DCCAttackState> weak = state;
	uint32_t id = core->GetPrompts().Ask(this, std::move(q), [selfID, index, weak, reason = need.reason](const PromptResult& r) {
		auto pending = weak.lock();
		Game* game = core->GetGame();
		Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
		// the attacker may be gone, or the attack called off
		if (!pending || !self || self->dccPendingAttack != pending || pending->next != index) return;
		DCCSwing& swing = pending->swings[index];
		swing.decided = true;
		int points = Clamp<int>(r.Value(), 0, self->GetDCCLuck());
		if (reason == BurnReason::Fumble) points = std::min(points, FUMBLE_CANCEL_COST);
		if (points > 0) {
			swing.burnPoints = points;
			if (reason == BurnReason::Fumble) {
				self->DCCBurnLuck(points, BurnValue());
				swing.fumbleCancelled = true;
			} else {
				swing.burnBonus = self->DCCBurnLuck(points, self->GetDCCBurnValue());
			}
		}
		Log(COMBAT, "DCC", "Luck prompt #{}: {} burns {} ({})", r.id, fmt::WideToChar { self->GetName() }, points, PromptOutcomeName(r.outcome));
		self->DCCContinueAttack(pending);
	});
	if (!id) return false;
	state->promptID = id;
	return true;
}

void Actor::DCCPolicyAttackBurn(DCCSwing& swing, const RollResult& roll)
{
	swing.decided = true;
	if (!IsDCCCharacter()) return; // monsters don't burn Luck
	BurnNeed need = SwingBurnNeed(this, swing.in, roll);
	if (need.reason == BurnReason::count) return;
	LuckPolicy policy = GetDCCLuckPolicy();
	BurnValue value = GetDCCBurnValue();
	int points = PolicyBurn(policy, need.reason, need.gap, GetDCCLuck(), value);
	if (need.reason == BurnReason::Fumble) {
		if (points <= 0) return;
		DCCBurnLuck(points, BurnValue());
		swing.burnPoints = points;
		swing.fumbleCancelled = true;
		return;
	}
	if (points > 0) {
		swing.burnPoints = points;
		swing.burnBonus = DCCBurnLuck(points, value);
		return;
	}
	// the party's good luck charm may burn for us, by its own policy, at +2 a point
	Actor* charm = DCCLuckCharm();
	if (!charm) return;
	LuckPolicy charmPolicy = charm->GetDCCLuckPolicy();
	if (!(charmPolicy.flags & LUCK_ALLIES)) return;
	BurnValue charmValue = charm->GetDCCBurnValue();
	points = PolicyBurn(charmPolicy, need.reason, need.gap, charm->GetDCCLuck(), charmValue);
	if (points <= 0) return;
	swing.burnPoints = points;
	swing.burnBonus = charm->DCCBurnLuck(points, charmValue);
	swing.burnBy = charm->GetGlobalID();
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
	int attackTotal = 0; // the attack roll's total, for saves against it (deeds)
	Duration lastDuration; // the duration a term rolled last, for a later term's SAME
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

static Duration TermDuration(const TableTerm& term, size_t arg, const Duration* same = nullptr)
{
	Duration duration;
	// SAME: what the term before rolled (a deed's blindness and its attack penalty last as long)
	if (same && arg < term.args.size() && term.args[arg] == "SAME") return *same;
	if (!ParseDuration(arg < term.args.size() ? term.args[arg] : std::string(), duration)) {
		duration = Duration();
		duration.dice.add = 1;
	}
	return duration;
}

// applies a stock or DCC:Combat effect for a table duration; returns the rolled amount for the log
// (rolled: the duration as rolled, for a later term's SAME)
static String ApplyFor(EffectRef& ref, ieDword p1, ieDword p2, const Duration& duration, Actor* who, Actor* caster, ieDword p3 = 0, Duration* rolled = nullptr)
{
	std::unique_ptr<Effect> fx;
	String what;
	if (rolled) *rolled = duration;
	if (duration.kind == Duration::Kind::Permanent || duration.kind == Duration::Kind::UntilHealed) {
		fx = EffectQueue::CreateEffect(ref, p1, p2, FX_DURATION_INSTANT_PERMANENT_AFTER_BONUSES);
		fx->SourceRef = duration.kind == Duration::Kind::Permanent ? PermanentSource : UntilHealedSource;
		what = duration.kind == Duration::Kind::Permanent ? u"for good" : u"until healed";
	} else {
		int amount = RollDice(duration.dice, [caster](int sides) { return caster->DCCDie(sides); });
		amount = std::max(1, amount);
		if (rolled) {
			rolled->dice = DiceExpr();
			rolled->dice.add = amount;
		}
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

static String ApplyCombat(CombatEffect kind, int value, const Duration& duration, Actor* who, Actor* caster, Duration* rolled = nullptr)
{
	return ApplyFor(fx_dcc_combat_ref, static_cast<ieDword>(value), static_cast<ieDword>(kind), duration, who, caster, 0, rolled);
}

// up to count creatures on the attacker's side near it, nearest first (deeds that help allies)
static std::vector<Actor*> NearAllies(const Actor* attacker, unsigned int radius, int count)
{
	std::vector<Actor*> allies;
	const Map* map = attacker->GetCurrentArea();
	if (!map || count <= 0) return allies;
	bool good = attacker->GetStat(IE_EA) <= EA_GOODCUTOFF;
	for (Actor* other : map->GetAllActorsInRadius(attacker->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED, radius)) {
		if (other == attacker || (other->GetStat(IE_EA) <= EA_GOODCUTOFF) != good) continue;
		if (other->GetDCCStates() & STATE_DYING) continue;
		allies.push_back(other);
	}
	std::sort(allies.begin(), allies.end(), [attacker](const Actor* a, const Actor* b) {
		return SquaredDistance(a->Pos, attacker->Pos) < SquaredDistance(b->Pos, attacker->Pos);
	});
	if (int(allies.size()) > count) allies.resize(count);
	return allies;
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
		int dc = ctx.save->vsAttack ? ctx.attackTotal : ctx.save->dc + (ctx.save->plusLevel ? ctx.level : 0);
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
		note = u"blinded " + ApplyFor(fx_dcc_blind_ref, 0, 0, TermDuration(term, 0, &ctx.lastDuration), who, attacker, 0, &ctx.lastDuration);
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
		note = fmt::format(u"{:+} to attack ", value) + ApplyCombat(CombatEffect::AttackMod, value, TermDuration(term, 1, &ctx.lastDuration), who, attacker, &ctx.lastDuration);
	} else if (key == "DMGMOD") {
		int value = argInt(0, 0);
		note = fmt::format(u"{:+} damage ", value) + ApplyFor(fx_dcc_dmgmod_ref, static_cast<ieDword>(value), 0, TermDuration(term, 1), who, attacker);
	} else if (key == "GUARD" || key == "ALLYATK" || key == "MORALE") {
		// the attacker and up to N allies near it (GUARD), or N allies (ALLYATK, MORALE)
		int value = argInt(0, 1);
		int count = argInt(1, 0);
		Duration duration = TermDuration(term, 2);
		std::vector<Actor*> whom = NearAllies(attacker, key == "GUARD" ? 100 : 300, count);
		if (key == "GUARD") whom.insert(whom.begin(), attacker);
		String names;
		String how;
		for (Actor* ally : whom) {
			if (key == "MORALE") {
				how = ApplyFor(fx_dcc_morale_ref, static_cast<ieDword>(value), 0, duration, ally, attacker);
			} else {
				how = ApplyCombat(key == "GUARD" ? CombatEffect::ACMod : CombatEffect::AttackMod, value, duration, ally, attacker);
			}
			names += (names.empty() ? u"" : u", ") + Name(ally);
		}
		const char16_t* what = key == "GUARD" ? u"AC" : (key == "ALLYATK" ? u"to attack" : u"morale");
		note = whom.empty() ? String(u"no allies near") : fmt::format(u"{} {:+} {} {}", names, value, what, how);
	} else if (key == "PUSH") {
		// knocked back, away from the attacker, for a second (the stock wing buffet)
		int strength = std::max(1, argInt(0, 2));
		auto fx = EffectQueue::CreateEffect(fx_dcc_push_ref, static_cast<ieDword>(strength), 2, FX_DURATION_INSTANT_LIMITED);
		fx->Duration = 1;
		fx->Source = attacker->Pos;
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), who, attacker);
		note = u"knocked back";
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

void Actor::DCCResolveSwing(DCCSwing& swing, Actor* target, int depth, DCCAttackState* state)
{
	const RulesData* rules = Rules();
	AttackRecord& record = swing.record;
	record.serial = dccAttackSerial;
	record.hand = swing.hand;
	record.die = AttackDie(swing.in.actionDie, swing.in.dieSteps);
	record.deed = GetStat(IE_DCC_DEEDDIE) ? dccDeedRoll : 0;
	if (record.natural <= 0) record.natural = DCCDie(record.die);
	RollResult roll = ResolveAttack(swing.in, record.natural);
	int before = roll.total;
	bool wasHit = roll.success;

	// Luck: the owner's answer to a prompt, or the policy (a thief rolls its luck dice here).
	// The one-shot modifiers are in swing.in already, so the burn re-resolves the same roll
	if (!swing.decided) DCCPolicyAttackBurn(swing, roll);
	if (swing.burnBonus) {
		AttackInput burned = swing.in;
		burned.modifier += swing.burnBonus;
		roll = ResolveAttack(burned, record.natural);
	}
	record.luckBurned = swing.burnPoints;
	record.luckBonus = swing.burnBonus;
	record.luckBy = swing.burnBy;
	record.fumbleCancelled = swing.fumbleCancelled;
	record.modifier = swing.in.modifier;
	record.total = roll.total;
	record.ac = roll.target;
	record.hit = roll.success;
	record.crit = roll.critical;
	record.fumble = roll.fumble;
	record.cursed = roll.cursed;
	if (state && depth == 0) record.deedKind = static_cast<int>(state->deed);

	// "Ilsa burns 3 Luck: 9 -> 12, hit" (before the attack line, which shows the burned total)
	if (swing.burnPoints) {
		const Game* game = core->GetGame();
		const Actor* by = swing.burnBy && game ? game->GetActorByGlobalID(swing.burnBy) : this;
		int left = by ? by->GetDCCLuck() : 0;
		if (swing.fumbleCancelled) {
			Feedback(fmt::format(u"{} burns {} Luck to shrug off the fumble (Luck {} left)", Name(this), swing.burnPoints, left), FT_COMBAT);
		} else {
			BurnValue value = by ? by->GetDCCBurnValue() : BurnValue();
			String how = value.IsDie() ? fmt::format(u"{}d{} = +{}", swing.burnPoints, value.luckDie, swing.burnBonus) : fmt::format(u"+{}", swing.burnBonus);
			String forWhom = by == this ? String() : fmt::format(u" for {}", Name(this));
			String result = record.hit ? (record.crit && !wasHit ? u"hit and crit" : u"hit") : u"still a miss";
			Feedback(fmt::format(u"{} burns {} Luck{} ({}): {} -> {}, {} (Luck {} left)", Name(by), swing.burnPoints, forWhom, how, before, record.total, result, left),
				 FT_COMBAT);
		}
	}

	// "Brak attacks Goblin: d20 14 + d3 2 +2 = 18 vs AC 12, hit: 1d8+3 = 7 damage"
	String dieText = fmt::format(u"d{} {}", record.die, record.natural);
	int flat = record.modifier;
	if (record.deed && swing.deed) {
		dieText += fmt::format(u" + d{} {}", GetStat(IE_DCC_DEEDDIE), record.deed);
		flat -= record.deed;
	}
	String luckText = swing.burnBonus ? fmt::format(u" +{} Luck", swing.burnBonus) : String();
	String line = fmt::format(u"{} {}{} {}: {}{}{} = {} vs AC {}, ", Name(this), depth ? u"free attack on" : u"attacks", swing.hand ? u" (off hand)" : u"",
				  Name(target), dieText, Plus(flat), luckText, record.total, record.ac);
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
	} else if (record.fumble && swing.fumbleCancelled) {
		// a warrior or dwarf burned 1 Luck: the natural 1 still misses, but no fumble roll (combat.md)
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
	// the Mighty Deed (deed die 3+ on a hit), once per attack action, after any crit (both apply)
	if (state && depth == 0 && record.hit && swing.deed) DCCMightyDeed(swing, target, state);

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
		free.decided = false;
		free.burnPoints = free.burnBonus = 0;
		free.burnBy = 0;
		free.fumbleCancelled = false;
		DCCResolveSwing(free, target, depth + 1, state);
		if (free.record.hit) {
			const ITMExtHeader* header = wi.extHeader;
			auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, free.damage, ConvertDamageType(header ? header->DamageType : 0) << 16, FX_DURATION_INSTANT_LIMITED);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), target, this);
		}
	}
}

// "Brak's deed d3 = 3: trip! Goblin 1 Ref save ...; prone for 1 round"
void Actor::DCCMightyDeed(DCCSwing& swing, Actor* target, DCCAttackState* state)
{
	if (state->deedTried || state->deed == Deed::None) return;
	state->deedTried = true;
	AttackRecord& record = swing.record;
	int die = GetStat(IE_DCC_DEEDDIE);
	std::string key = DeedKey(state->deed);
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
	String deedName = Widen(key);
	if (!DeedSucceeds(dccDeedRoll, record.hit)) {
		Feedback(fmt::format(u"{}'s deed d{} = {}: no {}", Name(this), die, dccDeedRoll, deedName), FT_COMBAT);
		return;
	}
	const ResultRow* row = Rules()->deeds.Lookup(state->deed, dccDeedRoll);
	if (!row) return;
	RowContext ctx;
	ctx.attacker = this;
	ctx.target = target;
	ctx.subject = target;
	ctx.level = GetDCCHitDice();
	ctx.attackTotal = record.total;
	ApplyRow(*row, ctx, *swing.wi);
	// a precision shot's extra damage rides on this hit
	swing.damage += ctx.extraDamage;
	record.deedDone = true;
	record.deedResult = row->text;
	Feedback(fmt::format(u"{}'s deed d{} = {}: {}! {}{}{}", Name(this), die, dccDeedRoll, deedName, Widen(row->text), ctx.notes.empty() ? u"" : u"; ", Joined(ctx.notes)),
		 FT_COMBAT);
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
