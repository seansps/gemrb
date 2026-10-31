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
#include "GameScript/GSUtils.h"
#include "Logging/Logging.h"
#include "Scriptable/Actor.h"

#include <cmath>
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
static EffectRef fx_dcc_slowsleep_ref = { "State:Sleep", -1 };
static EffectRef fx_dcc_miscast_ref = { "MiscastMagicModifier", -1 };

// "until healed" crit injuries end with the next night's rest (DCCRestHeal), permanent ones never
static const ResRef UntilHealedSource = "DCCHEAL";
static const ResRef PermanentSource = "DCCPERM";

static const RulesData* Rules()
{
	return core->GetDCCRules();
}

static String ApplyCombat(CombatEffect kind, int value, const Duration& duration, Actor* who, Actor* caster, Duration* rolled = nullptr);
static void DirectDamage(Actor* who, int damage, Actor* hitter);

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

void Actor::DCCRecordLine(String text)
{
	combatLog.push_back(std::move(text));
	if (combatLog.size() > 300) combatLog.pop_front();
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

// "trip", "precision" (for the log)
static String DeedName(Deed deed)
{
	std::string key = DeedKey(deed);
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
	return Widen(key);
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
	// fewer actions (Crit Table II): halved, or one action
	int limit = GetStat(IE_DCC_ACTLIMIT);
	if (!IsDCCCharacter()) return limit ? LimitedAttacks(GetStat(IE_NUMBEROFATTACKS), limit) : GetStat(IE_NUMBEROFATTACKS);
	// one attack per action die; a wizard only attacks with the first (classes.md)
	int dice = 0;
	for (unsigned int stat : { IE_DCC_ACTDIE1, IE_DCC_ACTDIE2, IE_DCC_ACTDIE3 }) {
		if (GetStat(stat)) ++dice;
	}
	if (BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Wizard)) dice = std::min(dice, 1);
	return LimitedAttacks(2 * std::max(dice, 1), limit);
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

	// injuries that hurt with exertion: grave boils (1 damage), a caved-in chest (DC 6 Fort or -1 Sta for good)
	uint32_t injuries = GetDCCInjuries();
	if (injuries & INJ_BOILS) {
		DirectDamage(this, 1, this);
		Feedback(fmt::format(u"{}'s boils burst: 1 damage", Name(this)), FT_COMBAT);
	}
	if (injuries & INJ_CHEST) {
		RollResult roll = DCCRollSave(Save::Fort, 0, 6, DCCDie(20));
		if (!roll.success) SetBase(IE_CON, std::max(0, Signed(BaseStats[IE_CON]) - 1));
		Feedback(fmt::format(u"{} strains its crushed chest: Fort save {} vs DC 6: {}", Name(this), roll.total, roll.success ? u"saved" : u"-1 Sta for good"), FT_COMBAT);
	}
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
	DiceExpr damageDice; // instead of the weapon's (shield bash, subdual, backstab); none: the weapon's
	bool subdual = false; // knocks out instead of killing at 0 hp
	bool backstab = false;
	bool bash = false; // the dwarf's shield bash
	bool intoMelee = false; // a missile into a melee: a miss may hit an ally instead
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

// sides for M2.3b's foes and allies: good, neutral, evil (the goblins in dcctest are neutral)
static int Side(const Actor* actor)
{
	ieDword ea = actor->GetStat(IE_EA);
	if (ea <= EA_GOODCUTOFF) return 0;
	return ea >= EA_EVILCUTOFF ? 2 : 1;
}

static bool OutOfFight(const Actor* actor)
{
	return (actor->GetStat(IE_STATE_ID) & STATE_DEAD) || (actor->GetInternalFlag() & IF_REALLYDIED) || (actor->GetDCCStates() & STATE_DYING) ||
		actor->GetStat(IE_DCC_SUBDUED);
}

// "adjacent", "in the melee": within this many feet, between the creatures' circles
constexpr int MELEE_REACH = 5;

// a creature of the attacker's side (not the attacker) in melee with the target
static bool InMelee(const Actor* attacker, const Actor* target)
{
	const Map* map = attacker->GetCurrentArea();
	if (!map) return false;
	for (const Actor* other : map->GetAllActorsInRadius(target->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, MELEE_REACH + 10)) {
		if (other == attacker || other == target || Side(other) != Side(attacker) || OutOfFight(other)) continue;
		if (WithinPersonalRange(other, target, MELEE_REACH)) return true;
	}
	return false;
}

// another creature (not in the melee around the target) on the line of fire: cover (Ethren decision)
static bool BehindCover(const Actor* attacker, const Actor* target)
{
	const Map* map = attacker->GetCurrentArea();
	if (!map) return false;
	unsigned int length = Distance(attacker, target);
	for (const Actor* other : map->GetAllActorsInRadius(target->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, length / 12 + 5)) {
		if (other == attacker || other == target || OutOfFight(other)) continue;
		if (WithinPersonalRange(other, target, MELEE_REACH)) continue; // that's the melee
		float_t t = 0;
		if (PersonalLineDistance(attacker->Pos, target->Pos, other, &t) == 0 && t > 0 && t < 1) return true;
	}
	return false;
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
	// M2.3b: higher ground (the area's height map), a missile into a melee, cover
	const Map* map = attacker->GetCurrentArea();
	if (!missile && map && HigherGround(map->GetHeight(attacker->Pos), map->GetHeight(target->Pos))) {
		conditions |= ATK_HIGHER;
	}
	if (missile && InMelee(attacker, target)) conditions |= ATK_INTO_MELEE;
	if (missile && BehindCover(attacker, target)) conditions |= DEF_COVER;
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

// the DCC weapon an item is (dccweap.2da), nullptr if none matches
static const WeaponRow* WeaponRowFor(const Actor* actor, const WeaponInfo& wi)
{
	const RulesData* rules = Rules();
	if (!rules) return nullptr;
	const CREItem* item = actor->inventory.GetSlotItem(wi.slot);
	std::string resref = item ? item->ItemResRef.c_str() : "";
	return rules->combat.weapons.Lookup(resref, static_cast<int>(wi.itemtype), wi.itemflags & IE_INV_ITEM_TWOHANDED);
}

bool Actor::DCCTrainedWith(const WeaponInfo& wi) const
{
	// level 0 is trained in its occupation's weapon and the last one it fought with (funnel.md)
	if (!IsDCCCharacter() || BaseStats[IE_LEVEL] == 0) return true;
	WeaponClass column = WeaponClassFor(static_cast<ClassID>(BaseStats[IE_CLASS]), AlignmentFromIE(GetStat(IE_ALIGNMENT)));
	const WeaponRow* row = WeaponRowFor(this, wi);
	// weapons the table doesn't know count as trained (Ethren decision)
	return !row || column == WeaponClass::count || row->Trained(column);
}

bool Actor::DCCBuildSwing(DCCSwing& swing, int hand, const Actor* target, int index, bool bash)
{
	bool character = IsDCCCharacter();
	swing.hand = hand;
	swing.wi = &weaponInfo[bash ? 0 : hand];
	const ITMExtHeader* header = swing.wi->extHeader;
	if (!header) return false;
	swing.bash = bash;
	swing.missile = !bash && (swing.wi->wflags & WEAPON_RANGED);
	// level 0: trained in the weapon it last fought with (funnel.md), kept in the occupation record
	if (character && !bash && BaseStats[IE_LEVEL] == 0 && !dccExt.occupation.Empty()) {
		int slot = 0;
		const CREItem* used = inventory.GetUsedWeapon(hand == 1, slot);
		std::string weapon = used ? std::string(used->ItemResRef.c_str()) : std::string();
		std::transform(weapon.begin(), weapon.end(), weapon.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (!weapon.empty() && weapon != dccExt.occupation.lastWeapon) {
			dccExt.occupation.lastWeapon = weapon;
			++dccExtRevision;
		}
	}
	bool fists = !bash && inventory.FistsEquipped();

	uint32_t augurFlags = swing.missile ? AF_MISSILE : AF_MELEE;
	if (fists) augurFlags |= AF_UNARMED;
	swing.abilityMod = character ? GetDCCAbilityMod(swing.missile ? Ability::Agi : Ability::Str) : 0;
	int deedDie = GetStat(IE_DCC_DEEDDIE);
	swing.deed = deedDie ? dccDeedRoll + Signed(GetStat(IE_DCC_DEEDADD)) : 0;
	int augur = character ? GetDCCAugurBonus(AugurRoll::Attack, augurFlags) : 0;

	// stock to-hit effects and magic weapons count as flat bonuses
	int effects = ToHit.GetFxBonus() + Signed(GetStat(IE_HITBONUS));
	if (!bash) effects += header->THAC0Bonus + swing.wi->launcherTHAC0Bonus;
	effects += Signed(GetStat(swing.missile ? IE_MISSILEHITBONUS : (fists ? IE_FISTHIT : IE_MELEETOHIT)));
	effects += Signed(GetStat(IE_DCC_ATKMOD)) + Signed(GetStat(IE_DCC_ROLLMOD));

	uint32_t conditions = AttackConditionsFor(this, target, swing.missile, bash ? 0 : GetWeaponRange(hand));
	// M2.3b: an untrained weapon is a die lower (the shield bash is the dwarf's own)
	const WeaponRow* row = bash ? nullptr : WeaponRowFor(this, *swing.wi);
	bool trained = bash || DCCTrainedWith(*swing.wi);
	if (UntrainedSteps(character, Signed(BaseStats[IE_LEVEL]), trained)) conditions |= ATK_UNTRAINED;
	swing.intoMelee = conditions & ATK_INTO_MELEE;
	Situational situational = SituationalModifiers(conditions, swing.missile);
	// lasting injuries: fighting with the other hand, one eye
	Situational injured = InjuryModifiers(GetDCCInjuries(), swing.missile, swing.wi->itemflags & IE_INV_ITEM_TWOHANDED);

	swing.in.actionDie = bash ? SHIELD_BASH_DIE : ActionDie(this, index);
	swing.in.dieSteps = situational.dieSteps + injured.dieSteps;
	swing.in.modifier = Signed(GetStat(IE_DCC_ATTACK)) + swing.abilityMod + swing.deed + augur + situational.flat + injured.flat + effects;
	swing.in.targetAC = Signed(target->GetStat(IE_DCC_AC));
	swing.in.threat = Signed(GetStat(IE_DCC_THREAT));
	swing.in.cursed = GetDCCStates() & STATE_CURSED;

	// damage: the weapon's dice + Str (melee and thrown) + deed + augur + magic + effects
	bool strength = !swing.missile || (swing.wi->wflags & (WEAPON_USESTRENGTH | WEAPON_USESTRENGTH_DMG));
	int damageAugur = character ? GetDCCAugurBonus(AugurRoll::Damage, augurFlags) : 0;
	int strMod = character && strength ? GetDCCAbilityMod(Ability::Str) : 0;
	swing.damageBonus = strMod + swing.deed + damageAugur + Signed(GetStat(IE_DAMAGEBONUS));
	if (bash) {
		// 1d3 (spiked or magic shields may do more: TODO)
		swing.damageDice.count = 1;
		swing.damageDice.sides = SHIELD_BASH_DAMAGE;
		return true;
	}
	swing.damageBonus += header->DamageBonus + swing.wi->launcherDmgBonus;
	// subdual blows: fists and the blackjack always, the stance with a sword, axe, club, spear or staff;
	// a monster's natural weapons (its "fists") are lethal (Ethren decision)
	Subdual subdual = row ? row->subdual : Subdual::Never;
	if (!character && row && row->key == "UNARMED") subdual = Subdual::Never;
	bool chosen = GetDCCCombatOptions() & OPT_SUBDUAL;
	if (!swing.missile && SubdualBlow(subdual, chosen, trained)) {
		swing.subdual = true;
		if (subdual == Subdual::Possible) {
			swing.damageDice = WeaponDice(header);
			swing.damageDice.add = 0;
			swing.damageDice.sides = SubdualDie(swing.damageDice.sides, subdual);
		}
	}
	return true;
}

// the backstab bonus of a thief (Table 1-9, by path and level)
static int BackstabBonus(const Actor* thief)
{
	const RulesData* rules = Rules();
	if (!rules) return 0;
	return rules->thiefSkills.Bonus(AlignmentFromIE(thief->GetStat(IE_ALIGNMENT)), Signed(thief->GetBase(IE_LEVEL)), ThiefSkill::Backstab);
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

	// immobile, witless, unconscious, dying, prone, a crit's "can't attack", no arms left
	uint32_t injuries = GetDCCInjuries();
	if (IsDCCHeld() || Immobile() || GetStat(IE_DCC_NOATTACK) || GetStat(IE_DCC_PRONE) || (injuries & INJ_BOTH_ARMS)) {
		Feedback(fmt::format(u"{} can't attack", Name(this)), FT_COMBAT);
		return;
	}
	// a fouled weapon (fumble 4) is useless until it's seen to; another one works
	ieDword fouled = GetStat(IE_DCC_FOULED);
	if (fouled && fouled == ieDword(weaponInfo[0].slot + 1)) {
		Feedback(fmt::format(u"{} can't attack: its weapon is fouled", Name(this)), FT_COMBAT);
		return;
	}
	bool melee = !(weaponInfo[0].wflags & WEAPON_RANGED);
	// a grapple declared for the next melee attack takes its place
	uint32_t options = GetDCCCombatOptions();
	if ((options & OPT_GRAPPLE) && melee) {
		SetBase(IE_DCC_COMBATOPT, options & ~OPT_GRAPPLE);
		DCCGrapple(target);
		return;
	}
	// the deed die: scripted attacks roll their own; a real round rolled it in InitRoundDCC, unless
	// the game was loaded in the middle of the round (it isn't saved), then the swing rolls it
	int roundDeedDie = GetStat(IE_DCC_DEEDDIE);
	if (!viaProjectile || (roundDeedDie && !dccDeedRoll)) {
		dccDeedRoll = roundDeedDie ? DCCDie(roundDeedDie) : 0;
	}

	bool character = IsDCCCharacter();
	bool twoWeapons = IsDualWielding() && weaponInfo[1].extHeader && !InjuryBlocksTwoWeapons(injuries);
	bool halfling = character && BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Halfling) && BaseStats[IE_LEVEL] > 0;
	int index = viaProjectile ? std::max(0, int(attacksperround) - int(attackcount) - 1) : 0;
	TwoWeaponDice twoDice;
	if (twoWeapons) twoDice = rules->combat.twoWeapons.Lookup(Signed(GetStat(IE_DEX)), halfling);

	// M2.3b: is the target in a fight already, does it notice us (surprise, the thief's backstab)
	const Game* game = core->GetGame();
	ieDword now = game ? ieDword(game->GameTime) : 0;
	ieDword window = 2 * core->Time.attack_round_size;
	bool targetInFight = target->dccCombatTime && now <= target->dccCombatTime + window;
	bool aware = CanSee(target, this, true, GA_NO_DEAD | GA_NO_HIDDEN);

	auto state = std::make_shared<DCCAttackState>();
	state->targetID = target->GetGlobalID();
	state->viaProjectile = viaProjectile;
	std::vector<DCCSwing>& swings = state->swings;
	swings.resize(twoWeapons ? 2 : 1);
	for (int hand = 0; hand < int(swings.size()); ++hand) {
		DCCSwing& swing = swings[hand];
		if (!DCCBuildSwing(swing, hand, target, index, false)) return;
		if (twoWeapons) {
			swing.in.dieSteps += hand ? twoDice.offSteps : twoDice.primarySteps;
			swing.in.critRule = hand ? twoDice.offCrit : twoDice.primaryCrit;
			swing.in.threat = 20; // the expanded threat range is lost with two weapons
			// halflings only fumble when both dice show 1 (classes.md); decided after both rolls
			swing.in.fumbles = !halfling;
		}
	}

	// the thief's backstab: from behind a foe busy with another, or against an unaware one; the
	// backstab bonus, and a hit is a crit (with a melee weapon: no fists, no missiles)
	DCCSwing& main = swings[0];
	bool thief = character && BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Thief);
	// from behind: the target is fighting another foe in melee right now
	bool busyElsewhere = false;
	for (const auto& foe : target->dccEngaged) {
		if (foe.first != GetGlobalID() && now <= foe.second + window) busyElsewhere = true;
	}
	bool fromBehind = targetInFight && IsBehind(target) && busyElsewhere;
	bool unaware = Surprised(targetInFight, aware);
	if (melee && !inventory.FistsEquipped() && BackstabApplies(thief, Signed(BaseStats[IE_LEVEL]), fromBehind, unaware, target->GetStat(IE_DISABLEBACKSTAB))) {
		main.backstab = true;
		main.in.autoCrit = true;
		main.in.modifier += BackstabBonus(this);
		const WeaponRow* row = WeaponRowFor(this, *main.wi);
		if (row && !row->backstab.IsNone() && !main.subdual) main.damageDice = row->backstab;
		Feedback(fmt::format(u"{} backstabs {} ({}): {:+} to hit, a hit is a crit", Name(this), Name(target), fromBehind ? u"from behind" : u"unaware",
				     BackstabBonus(this)),
			 FT_COMBAT);
	}

	// a charge (declared): +2 to hit and -2 AC for a round, with a run-up of half the speed
	if ((options & OPT_CHARGE) && melee) {
		SetBase(IE_DCC_COMBATOPT, options & ~OPT_CHARGE);
		float_t angle = AngleFromPoints(dccChargeFrom, Pos);
		int moved = static_cast<int>(Distance(dccChargeFrom, this) / std::max<float_t>(1, Feet2Pixels(1, angle)));
		int speed = Signed(GetStat(IE_DCC_SPEED));
		if (ChargeRunUp(moved, speed)) {
			for (auto& swing : swings) {
				swing.in.modifier += CHARGE_ATTACK;
			}
			Duration round;
			round.dice.add = 1;
			ApplyCombat(CombatEffect::ACMod, CHARGE_AC, round, this, this);
			Feedback(fmt::format(u"{} charges {} ({}' run-up): {:+} to hit, {:+} AC for a round", Name(this), Name(target), moved, CHARGE_ATTACK, CHARGE_AC), FT_COMBAT);
		} else {
			Feedback(fmt::format(u"{} has no room to charge ({}' of the {}' run-up)", Name(this), moved, (speed + 1) / 2), FT_COMBAT);
		}
	}

	// the dwarf's shield bash: one a round on top of the action dice, d14 + deed die, 1d3
	bool dwarf = character && BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Dwarf) && BaseStats[IE_LEVEL] > 0;
	if (dwarf && melee && ShieldItem(this) && !InjuryBlocksShield(injuries) && (!viaProjectile || index == 0)) {
		DCCSwing bash;
		if (DCCBuildSwing(bash, 2, target, 0, true)) swings.push_back(bash);
	}

	// surprise: a creature not yet in a fight that doesn't notice its attacker loses its first round
	if (unaware && !OutOfFight(target)) {
		Duration round;
		round.dice.add = 1;
		ApplyCombat(CombatEffect::NoAttack, 1, round, target, this);
		Feedback(fmt::format(u"{} is caught by surprise: no action this round", Name(target)), FT_COMBAT);
	}
	// both are in a fight now; a melee swing in reach engages them (withdrawal)
	dccCombatTime = target->dccCombatTime = now;
	if (melee && WithinPersonalRange(this, target, std::max<int>(GetWeaponRange(false), MELEE_REACH))) {
		dccEngaged[target->GetGlobalID()] = now;
		target->dccEngaged[GetGlobalID()] = now;
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

	// roll the dice together, so the halfling double-1 rule can see them
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
		if (!swing.record.hit) continue;
		// subdual blows and the shield bash land at once (no projectile)
		if (swing.subdual || swing.bash) {
			DCCApplySwingDamage(target, swing, swing.damage);
			continue;
		}
		totalDamage += swing.damage;
		if (hitHand < 0) hitHand = swing.hand;
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
		DCCApplySwingDamage(target, state->swings[hitHand], totalDamage);
	}
	return true;
}

void Actor::DCCApplySwingDamage(Actor* target, const DCCSwing& swing, int damage)
{
	if (damage <= 0 || !target) return;
	const ITMExtHeader* header = swing.wi ? swing.wi->extHeader : nullptr;
	ieDword type = swing.bash ? DAMAGE_CRUSHING : ConvertDamageType(header ? header->DamageType : 0);
	auto fx = EffectQueue::CreateEffect(fx_dcc_damage_ref, damage, type << 16, FX_DURATION_INSTANT_LIMITED);
	fx->Target = FX_TARGET_PRESET;
	// a subdual blow that takes the target to 0 hp knocks it out (DCCStartBleeding)
	target->dccSubdualHit = swing.subdual;
	core->ApplyEffect(std::move(fx), target, this);
	target->dccSubdualHit = false;
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
	if (!InParty || !IsDCCCharacter() || !DCCPausesOnKeyRolls()) return false;
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
	// M2.3b
	int normalDamage = 0; // this hit's own damage (HALFDMG, DOUBLEDMG)
	int rowDice = 0; // what the row's DICE rolled (DRAIN)
	bool cleave = false; // CLEAVE: a free attack on another foe if this hit kills
	std::vector<int> rampage; // RAMPAGE: feet, most attacks, from the target (1) or the attacker (0)
	int again = 0; // AGAIN: more rolls on the same table
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
		// DCC:Combat keeps which in the effect too: an old-format CRE doesn't save the source
		if (ref.Name == fx_dcc_combat_ref.Name) p2 |= duration.kind == Duration::Kind::Permanent ? COMBAT_FOR_GOOD : COMBAT_UNTIL_HEALED;
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

static String ApplyCombat(CombatEffect kind, int value, const Duration& duration, Actor* who, Actor* caster, Duration* rolled)
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

// d20 + the ability modifier (monsters: none) against a DC, with the subject's dice
static RollResult AbilityCheck(Actor* who, Ability ability, int dc)
{
	int mod = who->IsDCCCharacter() ? who->GetDCCAbilityMod(ability) : 0;
	return who->DCCRoll(RollKind::Check, mod, dc, who->DCCDie(20));
}

// feet from a table: dice, optionally times a number ("3d10x10")
static int RollFeet(const std::string& text, const Roller& roller)
{
	std::string dice = text;
	int times = 1;
	auto x = text.find('x');
	if (x != std::string::npos) {
		dice = text.substr(0, x);
		valid_signednumber(text.substr(x + 1).c_str(), times);
	}
	DiceExpr expr;
	if (!ParseDice(dice, expr)) return 0;
	return std::max(0, RollDice(expr, roller)) * std::max(1, times);
}

// a passable point up to feet from the origin along the angle (shorter if something is in the way);
// blocked tells whether it had to stop short
static Point PointAway(const Map* map, const Point& origin, float_t angle, int feet, bool* blocked = nullptr)
{
	if (blocked) *blocked = false;
	Point best = origin;
	int pixels = static_cast<int>(Feet2Pixels(feet, angle));
	for (int step = 8; step <= pixels; step += 8) {
		Point p(origin.x + static_cast<int>(std::cos(angle) * step), origin.y - static_cast<int>(std::sin(angle) * step));
		if (!map || !bool(map->GetBlocked(p) & PathMapFlags::PASSABLE)) {
			if (blocked) *blocked = true;
			break;
		}
		best = p;
	}
	return best;
}

// the direction from a to b (b - a), in the angle convention of Feet2Pixels and PointAway
static float_t AngleAway(const Point& a, const Point& b)
{
	if (a == b) return 0;
	return std::atan2(static_cast<float_t>(a.y - b.y), static_cast<float_t>(b.x - a.x));
}

// the weapon in a creature's hand drops feet away from it (away from the attacker), or at its feet
static void DropAway(Actor* holder, const Actor* attacker, int feet)
{
	int slot = holder->inventory.GetEquippedSlot();
	Map* map = holder->GetCurrentArea();
	Point where = holder->Pos;
	if (feet > 0 && map) where = PointAway(map, holder->Pos, AngleAway(attacker->Pos, holder->Pos), feet);
	if (holder->inventory.DropItemAtLocation(slot, 0, map, where)) holder->ReinitQuickSlots();
}

// the living foes of a creature (another side) near a point, nearest first
static std::vector<Actor*> FoesNear(const Actor* of, const Point& where, int feet, const Actor* except)
{
	std::vector<Actor*> foes;
	const Map* map = of->GetCurrentArea();
	if (!map) return foes;
	for (Actor* other : map->GetAllActorsInRadius(where, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, feet + 5)) {
		if (other == of || other == except || Side(other) == Side(of) || OutOfFight(other)) continue;
		if (!WithinPersonalRange(other, where, feet)) continue;
		foes.push_back(other);
	}
	std::sort(foes.begin(), foes.end(), [&where](const Actor* a, const Actor* b) {
		return SquaredDistance(a->Pos, where) < SquaredDistance(b->Pos, where);
	});
	return foes;
}

static bool FailedSave(RowContext& ctx)
{
	if (!ctx.save || ctx.save->IsNone()) return true;
	if (ctx.saved < 0) {
		int dc = ctx.save->vsAttack ? ctx.attackTotal : ctx.save->dc + (ctx.save->plusLevel ? ctx.level : 0);
		if (ctx.save->ability >= 0) {
			// an ability check (Crit Table DR): d20 + the ability modifier
			RollResult roll = AbilityCheck(ctx.subject, static_cast<Ability>(ctx.save->ability), dc);
			ctx.saved = roll.success ? 1 : 0;
			static const char16_t* abilities[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
			ctx.notes.push_back(fmt::format(u"{} {} check {}{} = {} vs DC {}: {}", Name(ctx.subject), abilities[ctx.save->ability],
							roll.natural, Plus(roll.total - roll.natural), roll.total, dc, roll.success ? u"passed" : u"failed"));
			return ctx.saved == 0;
		}
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
		note = fmt::format(u"speed {}% ", percent) + ApplyFor(fx_dcc_movement_ref, percent, 2, TermDuration(term, 1, &ctx.lastDuration), who, attacker, 0, &ctx.lastDuration);
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
		std::vector<Actor*> whom = NearAllies(attacker, key == "GUARD" ? 15 : 60, count);
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
		who->DCCDisengage();
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
		// GONE: banished, no body left to roll over (the stock disintegration; Ethren decision)
		bool gone = arg(0) == "GONE";
		auto fx = EffectQueue::CreateEffect(fx_dcc_death_ref, 0, gone ? 512 : 4, FX_DURATION_INSTANT_PERMANENT);
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), who, attacker);
		note = gone ? u"is gone" : u"dies";
	} else if (key == "HALFHP") {
		int lost = Signed(who->GetBase(IE_HITPOINTS)) / 2;
		ctx.extraDamage += lost;
		note = fmt::format(u"loses half its hp ({})", lost);
	} else if (key == "ABIL") {
		int ability = AbilityIndex(arg(0));
		// RND5: a random one of Str, Agi, Sta, Int, Per (Crit Table DN)
		if (arg(0) == "RND5") {
			static const Ability random5[] = { Ability::Str, Ability::Agi, Ability::Sta, Ability::Int, Ability::Per };
			ability = static_cast<int>(random5[attacker->DCCDie(5) - 1]);
		}
		if (ability < 0) return;
		int points = std::max(1, rollDice(arg(1)));
		static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
		if (arg(2) == "PERM") {
			unsigned int stat = AbilityStat(static_cast<Ability>(ability));
			who->SetBase(stat, std::max(0, Signed(who->GetBase(stat)) - points));
			note = fmt::format(u"-{} {} for good", points, names[ability]);
		} else if (!arg(2).empty()) {
			// for a while (a week): the loss lasts as long as the effect
			Duration duration = TermDuration(term, 2);
			note = fmt::format(u"-{} {} ", points, names[ability]) + ApplyFor(fx_dcc_loss_ref, points, ability, duration, who, attacker);
		} else {
			auto fx = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, ability, FX_DURATION_INSTANT_PERMANENT);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), who, attacker);
			note = fmt::format(u"-{} {}", points, names[ability]);
		}
	} else if (key == "DISARM" || key == "DISARMSELF") {
		Actor* holder = key == "DISARM" ? ctx.target : attacker;
		if (!HasWeapon(holder, true)) return;
		// "lands N' away": dropped that far from the holder, away from the attacker
		int feet = key == "DISARM" && !arg(0).empty() ? RollFeet(arg(0), roller) : 0;
		DropAway(holder, attacker, feet);
		note = feet ? fmt::format(u"{}'s weapon lands {}' away", Name(holder), feet) : fmt::format(u"{} drops its weapon", Name(holder));
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
				int feet = RollFeet("1d10+5", roller);
				DropAway(holder, attacker, feet);
				note = fmt::format(u"{}'s magic weapon is knocked {}' away", Name(holder), feet);
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
	} else if (key == "INJURY") {
		Injury injury = ParseInjury(arg(0));
		if (injury == INJ_NONE) {
			Log(WARNING, "DCC", "Unknown injury '{}'", arg(0));
			return;
		}
		Duration duration = TermDuration(term, 1, &ctx.lastDuration);
		note = Widen(InjuryName(injury)) + u" " + ApplyCombat(CombatEffect::Injury, static_cast<int>(injury), duration, who, attacker, &ctx.lastDuration);
		if (injury == INJ_AMNESIA) {
			// it forgets its spells: arcane and divine casting fail while it lasts
			ApplyFor(fx_dcc_miscast_ref, 100, 0, duration, who, attacker);
			ApplyFor(fx_dcc_miscast_ref, 100, 1, duration, who, attacker);
		}
	} else if (key == "RAGE") {
		// battle rage (footnote **): the attacker spends Per or Int, +1d12 a point (its setting)
		int wanted = attacker->IsDCCCharacter() ? Signed(attacker->GetBase(IE_DCC_RAGE)) : 0;
		RageSpend spend = BattleRage(wanted, Signed(attacker->GetStat(IE_CHR)), Signed(attacker->GetStat(IE_INT)));
		if (!spend.Total()) {
			note = u"battle rage (nothing spent)";
		} else {
			for (const auto& part : { std::make_pair(Ability::Per, spend.personality), std::make_pair(Ability::Int, spend.intelligence) }) {
				if (!part.second) continue;
				auto fx = EffectQueue::CreateEffect(fx_dcc_loss_ref, part.second, static_cast<ieDword>(part.first), FX_DURATION_INSTANT_PERMANENT);
				fx->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(fx), attacker, attacker);
			}
			DiceExpr dice;
			dice.count = spend.Total();
			dice.sides = RAGE_DIE;
			int extra = RollDice(dice, roller);
			ctx.extraDamage += extra;
			note = fmt::format(u"battle rage: {} spends {} Per and {} Int, +{}d{} = {} damage", Name(attacker), spend.personality, spend.intelligence, dice.count,
					   RAGE_DIE, extra);
		}
	} else if (key == "HALFACT" || key == "ONEACTION") {
		bool half = key == "HALFACT";
		Duration duration = TermDuration(term, 0, &ctx.lastDuration);
		note = (half ? u"half its actions " : u"one action ") + ApplyCombat(CombatEffect::ActionLimit, half ? ACTIONS_HALVED : ACTIONS_ONE, duration, who, attacker);
	} else if (key == "FOUL") {
		if (!HasWeapon(who, false)) return;
		int slot = who->weaponInfo[0].slot;
		note = u"its weapon is fouled " + ApplyCombat(CombatEffect::Fouled, slot + 1, TermDuration(term, 0), who, attacker);
	} else if (key == "DOWNED" || key == "STUCK") {
		// down (prone) or stuck (no actions) until an ability check: DOWNED:16, STUCK:AGI:16, STUCK:STR:10:L
		bool prone = key == "DOWNED";
		int ability = prone ? static_cast<int>(Ability::Agi) : AbilityIndex(arg(0));
		int dc = argInt(prone ? 0 : 1, FUMBLE_STAND_DC);
		if (!prone && arg(2) == "L") dc += ctx.level;
		if (ability < 0) return;
		Duration turn;
		turn.kind = Duration::Kind::Turns;
		turn.dice.add = 1;
		static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
		ApplyCombat(CombatEffect::StandCheck, dc | (ability << 8) | (prone ? STAND_PRONE : 0), turn, who, attacker);
		note = fmt::format(u"{} until a DC {} {} check", prone ? u"down" : u"stuck", dc, names[ability]);
	} else if (key == "WOUND") {
		DiceExpr dice;
		if (!ParseDice(arg(0), dice)) return;
		int ability = arg(1).empty() ? -1 : AbilityIndex(arg(1));
		Duration healed;
		healed.kind = Duration::Kind::UntilHealed;
		ApplyCombat(CombatEffect::Wound, dice.count | (dice.sides << 8) | ((ability + 1) << 16), healed, who, attacker);
		note = fmt::format(u"{} a round until healed", Widen(arg(0))) + (ability >= 0 ? u" (and a point of the ability)" : u"");
	} else if (key == "CHOKE") {
		// a Fort save now and every round after a failure: -1d4 Sta each time, lethal at 0
		int dc = argInt(0, 20);
		RollResult roll = who->DCCRollSave(Save::Fort, 0, dc, who->DCCDie(20));
		if (roll.success) {
			note = fmt::format(u"Fort save {} vs DC {}: shrugs it off", roll.total, dc);
		} else {
			int points = who->DCCDie(4);
			auto fx = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, static_cast<ieDword>(Ability::Sta) | 0x100, FX_DURATION_INSTANT_PERMANENT);
			fx->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(fx), who, attacker);
			Duration turn;
			turn.kind = Duration::Kind::Turns;
			turn.dice.add = 1;
			ApplyCombat(CombatEffect::Choke, dc, turn, who, attacker);
			note = fmt::format(u"Fort save {} vs DC {}: -{} Sta, choking", roll.total, dc, points);
		}
	} else if (key == "NUMB") {
		Duration turn;
		turn.kind = Duration::Kind::Turns;
		turn.dice.add = 1;
		ApplyCombat(CombatEffect::Numb, 2, turn, who, attacker);
		note = u"numbness spreads (Fort saves from DC 2 up)";
	} else if (key == "HALFDMG") {
		int half = ctx.normalDamage - std::max(1, ctx.normalDamage / 2);
		ctx.extraDamage -= half;
		note = fmt::format(u"half damage (-{})", half);
	} else if (key == "DOUBLEDMG") {
		ctx.extraDamage += ctx.normalDamage;
		note = fmt::format(u"double damage (+{})", ctx.normalDamage);
	} else if (key == "SLEEP") {
		note = u"asleep " + ApplyFor(fx_dcc_slowsleep_ref, 0, 0, TermDuration(term, 0), who, attacker);
	} else if (key == "ROLLMOD") {
		int value = argInt(0, 0);
		note = fmt::format(u"{:+} to all rolls ", value) + ApplyCombat(CombatEffect::RollMod, value, TermDuration(term, 1), who, attacker);
	} else if (key == "MAXHP") {
		int points = argInt(0, 1);
		int maxHP = std::max(1, Signed(who->GetBase(IE_MAXHITPOINTS)) - points);
		who->SetBase(IE_MAXHITPOINTS, maxHP);
		if (Signed(who->GetBase(IE_HITPOINTS)) > maxHP) who->SetBase(IE_HITPOINTS, maxHP);
		note = fmt::format(u"-{} max hp for good", points);
	} else if (key == "XP") {
		int points = argInt(0, 1);
		who->SetBase(IE_XP, std::max(0, Signed(who->GetBase(IE_XP)) - points));
		note = fmt::format(u"-{} XP", points);
	} else if (key == "REROLL") {
		int ability = AbilityIndex(arg(0));
		if (ability < 0) return;
		DiceExpr dice;
		dice.count = 3;
		dice.sides = 6;
		int score = RollDice(dice, roller);
		who->SetBase(AbilityStat(static_cast<Ability>(ability)), score);
		static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
		note = fmt::format(u"{} is now {}", names[ability], score);
	} else if (key == "ABILTO") {
		// down to a score (temporary loss)
		int ability = AbilityIndex(arg(0));
		if (ability < 0) return;
		int now = Signed(who->GetStat(AbilityStat(static_cast<Ability>(ability))));
		int points = now - argInt(1, 1);
		if (points <= 0) return;
		auto fx = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, ability, FX_DURATION_INSTANT_PERMANENT);
		fx->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(fx), who, attacker);
		static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
		note = fmt::format(u"{} drops to {}", names[ability], argInt(1, 1));
	} else if (key == "LIMBS") {
		// limbs torn off: +1d14 each; the weapon arm, the other arm, then the legs
		int limbs = std::max(1, rollDice(arg(0)));
		int extra = 0;
		for (int i = 0; i < limbs; ++i) {
			extra += attacker->DCCDie(14);
		}
		ctx.extraDamage += extra;
		Duration forGood;
		forGood.kind = Duration::Kind::Permanent;
		if (HasWeapon(who, true)) DropAway(who, attacker, 0);
		ApplyCombat(CombatEffect::Injury, INJ_WEAPON_ARM, forGood, who, attacker);
		if (limbs >= 2) ApplyCombat(CombatEffect::Injury, INJ_BOTH_ARMS, forGood, who, attacker);
		if (limbs >= 3) {
			ApplyCombat(CombatEffect::Injury, INJ_LEG, forGood, who, attacker);
			ApplyFor(fx_dcc_movement_ref, 0, 2, forGood, who, attacker);
		}
		note = fmt::format(u"{} limbs lost: +{}d14 = {} damage", limbs, limbs, extra);
	} else if (key == "SMASHGEAR") {
		// each piece of gear: 50% destroyed (magic 10%)
		String broken;
		for (int slot : { who->inventory.GetEquippedSlot(), who->inventory.GetShieldSlot(), Inventory::GetArmorSlot(), Inventory::GetHeadSlot() }) {
			if (slot < 0 || (slot == who->inventory.GetEquippedSlot() && who->inventory.FistsEquipped())) continue;
			const CREItem* item = who->inventory.GetSlotItem(slot);
			if (!item || (item->Flags & IE_INV_ITEM_UNDROPPABLE)) continue;
			const Item* itm = gamedata->GetItem(item->ItemResRef, true);
			bool magic = itm && ((itm->Flags & IE_ITEM_MAGICAL) || (item->Flags & IE_INV_ITEM_MAGICAL));
			if (itm) gamedata->FreeItem(itm, item->ItemResRef);
			int roll = attacker->DCCDie(100);
			if (roll > (magic ? 10 : 50)) continue;
			broken += (broken.empty() ? u"" : u", ") + Widen(std::string(item->ItemResRef.c_str()));
			DestroySlotItem(who, slot);
		}
		note = broken.empty() ? String(u"its gear holds") : u"smashed: " + broken;
	} else if (key == "FLING") {
		// flung feet away (from the attacker, or RANDOM); FALL: +1d4 per 30' and +1d6 against an
		// obstacle; STRIKE: the creatures next to where it lands strike it (Crit Table V, 2);
		// RICOCHET: into a random other creature, both take 1d10 (Crit Table G, 6)
		int feet = RollFeet(arg(0), roller);
		auto has = [&term](const char* option) { return std::find(term.args.begin() + 1, term.args.end(), option) != term.args.end(); };
		Map* map = who->GetCurrentArea();
		if (!map || feet <= 0) return;
		float_t angle = AngleAway(attacker->Pos, who->Pos);
		if (has("RANDOM")) angle = static_cast<float_t>(attacker->DCCDie(360)) * static_cast<float_t>(M_PI) / 180;
		Actor* victim = nullptr;
		if (has("RICOCHET")) {
			std::vector<Actor*> others;
			for (Actor* other : map->GetAllActorsInRadius(who->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, feet)) {
				if (other != who && other != attacker && !OutOfFight(other)) others.push_back(other);
			}
			if (!others.empty()) {
				victim = others[attacker->DCCDie(int(others.size())) - 1];
				angle = AngleAway(who->Pos, victim->Pos);
				feet = std::max(1, static_cast<int>(Distance(who, victim) / std::max<float_t>(1, Feet2Pixels(1, angle))) - MELEE_REACH);
			}
		}
		bool blocked = false;
		// it doesn't block its own way
		map->ClearSearchMapFor(who);
		Point landing = PointAway(map, who->Pos, angle, feet, &blocked);
		who->SetPosition(landing, true);
		// a creature thrown out of a melee didn't withdraw, nor did the ones it was fighting
		who->DCCDisengage();
		note = fmt::format(u"flung {}'", feet);
		if (has("FALL")) {
			DiceExpr fall;
			fall.count = (feet + 29) / 30;
			fall.sides = 4;
			int extra = RollDice(fall, roller);
			if (blocked) extra += attacker->DCCDie(6);
			ctx.extraDamage += extra;
			note += fmt::format(u"{}, +{} damage", blocked ? u" into an obstacle" : u"", extra);
		}
		if (victim) {
			int hurt = attacker->DCCDie(10);
			int hurtVictim = attacker->DCCDie(10);
			ctx.extraDamage += hurt;
			DirectDamage(victim, hurtVictim, attacker);
			note += fmt::format(u" into {}: {} and {} damage", Name(victim), hurt, hurtVictim);
		}
		if (has("STRIKE")) {
			// those next to it where it lands strike it, by accident
			for (Actor* other : map->GetAllActorsInRadius(who->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, MELEE_REACH + 10)) {
				if (other == who || other == attacker || OutOfFight(other) || !WithinPersonalRange(other, who, MELEE_REACH)) continue;
				if (other->IsDCCHeld() || other->Immobile()) continue;
				ctx.notes.push_back(std::move(note));
				note.clear();
				other->DCCFreeAttack(who, fmt::format(u"{} is hurled into it", Name(who)));
			}
		}
	} else if (key == "CORRUPT") {
		// M2.5: corruption rolls (Crit Table DN 5, 10): 1d10 + the subject's Luck mod on the minor
		// table for an attacker of up to 5 HD, the major one up to 10 HD, the greater one above
		int count = std::max(1, argInt(0, 1));
		int hd = attacker->GetDCCHitDice();
		CorruptionTable table = hd <= 5 ? CorruptionTable::Minor : (hd <= 10 ? CorruptionTable::Major : CorruptionTable::Greater);
		int luck = who->IsDCCCharacter() ? who->GetDCCAbilityMod(Ability::Luck) : 0;
		std::vector<int> rows;
		for (int i = 0; i < count; ++i) {
			int roll = Clamp(attacker->DCCDie(10) + luck, 1, 10);
			rows.push_back(who->DCCCorrupt(table, 0, "", roll));
		}
		note = fmt::format(u"{} corruption", count == 1 ? u"a" : fmt::format(u"{}x", count));
	} else if (key == "CLEAVE") {
		ctx.cleave = true;
		return;
	} else if (key == "RAMPAGE") {
		ctx.rampage = { argInt(0, 10), argInt(1, 10), arg(2) == "TARGET" ? 1 : 0 };
		return;
	} else if (key == "AGAIN") {
		ctx.again = std::max(ctx.again, argInt(0, 1));
		return;
	} else if (key == "MORALECHECK") {
		// the subject's side near it checks morale against the DC (PCS: the party too, stunned on a failure)
		int dc = argInt(0, MORALE_DC);
		bool pcs = arg(1) == "PCS";
		const Map* map = who->GetCurrentArea();
		int checked = 0;
		if (map) {
			for (Actor* other : map->GetAllActorsInRadius(who->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED, 60)) {
				if (other == who || Side(other) != Side(who) || OutOfFight(other)) continue;
				if (other->InParty && !pcs) continue;
				other->DCCCheckMorale(attacker, dc, fmt::format(u"{}'s fate", Name(who)), pcs);
				++checked;
			}
		}
		note = checked ? fmt::format(u"{} near it check morale vs {}", checked, dc) : String(u"nobody near to lose heart");
	} else if (key == "DRAIN") {
		// the attacker heals what the row's dice rolled
		int max = Signed(attacker->GetStat(IE_MAXHITPOINTS));
		int hp = Signed(attacker->GetBase(IE_HITPOINTS));
		int healed = std::max(0, std::min(ctx.rowDice, max - hp));
		if (healed) attacker->SetBase(IE_HITPOINTS, hp + healed);
		note = fmt::format(u"{} heals {}", Name(attacker), healed);
	} else if (key == "ATTACKER") {
		int damage = std::max(1, rollDice(arg(0)));
		DirectDamage(attacker, damage, attacker);
		note = fmt::format(u"{} loses {} hp", Name(attacker), damage);
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
		ctx.rowDice = extra;
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

// M2.5: the crit table grammar for magic results (corruption, misfire, disapproval, mercurial
// magic): this actor causes them (its dice), subject suffers them; damage lands at once
String Actor::DCCApplyTableEffects(const std::string& effects, Actor* subject, const std::string& save, int level)
{
	if (!subject || effects.empty() || effects == "*") return u"";
	std::vector<TableAlternatives> parsed;
	if (!ParseEffects(effects, parsed)) {
		Log(WARNING, "DCC", "Bad magic result effects '{}'", effects);
		return u"";
	}
	SaveSpec spec;
	if (!save.empty() && !ParseSaveSpec(save, spec)) spec = SaveSpec();
	RowContext ctx;
	ctx.attacker = this;
	ctx.target = subject;
	ctx.subject = subject;
	ctx.crit = true;
	ctx.level = level;
	ctx.save = &spec;
	WeaponInfo wi;
	for (const auto& alternatives : parsed) {
		for (const auto& term : alternatives) {
			if (!TermApplies(term, ctx)) continue;
			ApplyTerm(term, ctx, wi);
			break;
		}
	}
	if (ctx.extraDamage > 0) DirectDamage(subject, ctx.extraDamage, this);
	return Joined(ctx.notes);
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
	record.target = target->GetGlobalID();
	record.backstab = swing.backstab;
	record.subdual = swing.subdual;
	record.bash = swing.bash;
	record.free = depth > 0;

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
	const char16_t* handText = swing.bash ? u" (shield bash)" : (swing.hand ? u" (off hand)" : u"");
	String line = fmt::format(u"{} {}{} {}: {}{}{} = {} vs AC {}, ", Name(this), depth ? u"free attack on" : u"attacks", handText,
				  Name(target), dieText, Plus(flat), luckText, record.total, record.ac);
	if (record.hit) {
		line += record.crit ? (swing.backstab ? u"backstab, critical hit" : u"critical hit") : u"hit";
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
		// a shield bash, a subdual blow (a die lower) or a backstab's own damage
		if (!swing.damageDice.IsNone()) dice = swing.damageDice;
		int rolled = RollDice(dice, [this](int sides) { return DCCDie(sides); });
		swing.damage = AttackDamage(rolled, swing.damageBonus);
		line += fmt::format(u": {}{} = {} damage", Widen(DiceString(dice)), Plus(swing.damageBonus), swing.damage);
		if (!swing.extraOnHit.IsNone()) {
			int extra = RollDice(swing.extraOnHit, [this](int sides) { return DCCDie(sides); });
			swing.damage += extra;
			line += fmt::format(u" +{} = {}", Widen(DiceString(swing.extraOnHit)), extra);
		}
		// a deed die below 3 on a hit: the declared deed doesn't happen (DCCMightyDeed logs a success)
		if (state && depth == 0 && swing.deed && state->deed != Deed::None && !state->deedTried && !DeedSucceeds(dccDeedRoll, true)) {
			line += fmt::format(u" (deed {}: no {})", dccDeedRoll, DeedName(state->deed));
		}
		if (swing.subdual) line += u" (subdual)";
	}
	Feedback(std::move(line), FT_TOHIT);

	RowContext ctx;
	ctx.attacker = this;
	ctx.target = target;
	ctx.level = GetDCCHitDice();
	ctx.normalDamage = record.hit ? swing.damage : 0;

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
			// "roll again" (Crit Table G): more rolls on the same table; another "again" there is nothing
			int again = ctx.again;
			ctx.again = 0;
			for (int i = 0; i < again; ++i) {
				int more = CritRoll(RollDice(critDice, [this](int sides) { return DCCDie(sides); }), add, luck, defender);
				const ResultRow* extra = table->Lookup(more);
				if (!extra) continue;
				ctx.notes.push_back(fmt::format(u"again {}: {}", more, Widen(extra->text)));
				ApplyRow(*extra, ctx, wi);
				ctx.again = 0;
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

	// a miss into a melee (or at a pinned foe) may hit an ally instead: 1 on a d2, rolled again
	// against that ally (combat.md "Firing into melee", "Grappling")
	Actor* holder = nullptr;
	if (!swing.missile && target->GetStat(IE_DCC_PINNED)) {
		const Game* game = core->GetGame();
		holder = game ? game->GetActorByGlobalID(target->GetStat(IE_DCC_PINNED)) : nullptr;
		if (holder == this) holder = nullptr;
	}
	if (depth == 0 && !record.hit && !record.fumble && ((swing.missile && swing.intoMelee) || holder)) {
		Actor* ally = holder;
		if (!ally) {
			std::vector<Actor*> allies;
			const Map* map = target->GetCurrentArea();
			if (map) {
				for (Actor* other : map->GetAllActorsInRadius(target->Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, MELEE_REACH + 10)) {
					if (other == this || other == target || Side(other) != Side(this) || OutOfFight(other)) continue;
					if (WithinPersonalRange(other, target, MELEE_REACH)) allies.push_back(other);
				}
			}
			if (!allies.empty() && DCCDie(STRAY_DIE) == 1) ally = allies[allies.size() > 1 ? DCCDie(int(allies.size())) - 1 : 0];
		} else if (DCCDie(STRAY_DIE) != 1) {
			ally = nullptr;
		}
		if (ally) {
			DCCSwing stray = swing;
			stray.record = AttackRecord();
			stray.damage = stray.extraDamage = 0;
			stray.in.targetAC = Signed(ally->GetStat(IE_DCC_AC));
			stray.decided = true; // no Luck for a stray shot
			stray.in.autoCrit = false;
			stray.backstab = false;
			stray.burnPoints = stray.burnBonus = 0;
			Feedback(fmt::format(u"{}'s miss goes astray towards {}", Name(this), Name(ally)), FT_COMBAT);
			DCCResolveSwing(stray, ally, depth + 1, state);
			if (stray.record.hit) DCCApplySwingDamage(ally, stray, stray.damage);
		}
	}

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
		free.in.autoCrit = false;
		free.backstab = false;
		free.burnPoints = free.burnBonus = 0;
		free.burnBy = 0;
		free.fumbleCancelled = false;
		DCCResolveSwing(free, target, depth + 1, state);
		if (free.record.hit) DCCApplySwingDamage(target, free, free.damage);
	}
	if (depth > 0) return;

	// cleave (Crit Table V, 21): if this blow kills, a free attack on another foe within 10'
	int hp = Signed(target->GetStat(IE_HITPOINTS));
	bool kills = OutOfFight(target) || (record.hit && swing.damage >= hp);
	if (ctx.cleave && kills) {
		std::vector<Actor*> foes = FoesNear(this, Pos, 10, target);
		if (foes.empty()) {
			Feedback(fmt::format(u"{} cleaves through, but no other foe is near", Name(this)), FT_COMBAT);
		} else {
			DCCFreeAttack(foes[0], fmt::format(u"{} cleaves on into {}", Name(this), Name(foes[0])));
		}
	}
	// keep attacking until a miss (V-28, M-30, DR-29/30, G-9): foes near the attacker (or the target)
	if (ctx.rampage.size() == 3) {
		int feet = ctx.rampage[0];
		int most = ctx.rampage[1];
		Point from = ctx.rampage[2] ? target->Pos : Pos;
		std::set<ieDword> struck { target->GetGlobalID() };
		for (int i = 0; i < most; ++i) {
			Actor* next = nullptr;
			for (Actor* foe : FoesNear(this, from, feet, target)) {
				if (!struck.count(foe->GetGlobalID())) {
					next = foe;
					break;
				}
			}
			if (!next) break;
			struck.insert(next->GetGlobalID());
			AttackRecord free = DCCFreeAttack(next, fmt::format(u"{} keeps attacking", Name(this)));
			if (!free.hit) break;
		}
	}
}

// "Brak's deed d3 = 3: trip! tripped; Goblin 1 Ref save ...; prone for 1 round"
void Actor::DCCMightyDeed(DCCSwing& swing, Actor* target, DCCAttackState* state)
{
	if (state->deedTried || state->deed == Deed::None) return;
	state->deedTried = true;
	AttackRecord& record = swing.record;
	int die = GetStat(IE_DCC_DEEDDIE);
	if (!DeedSucceeds(dccDeedRoll, record.hit)) return; // the attack line says so
	String deedName = DeedName(state->deed);
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
	// a subdual blow knocks out instead of killing (one hp pool: the blow that drops it decides)
	if (dccSubdualHit) {
		if (!fxqueue.HasEffectWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Subdued))) {
			Duration turn;
			turn.kind = Duration::Kind::Turns;
			turn.dice.add = 1;
			ApplyCombat(CombatEffect::Subdued, 1, turn, this, this);
			Feedback(fmt::format(u"{} is knocked out", Name(this)), FT_COMBAT);
		}
		return true;
	}
	// a lethal blow on one knocked out: no more mercy
	fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Subdued));
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
	// a heal closes a wound that bleeds every round, and wakes the knocked out
	ieDword wound = static_cast<ieDword>(CombatEffect::Wound);
	if (fxqueue.HasEffectWithParam(fx_dcc_combat_ref, wound) || fxqueue.HasEffectWithParam(fx_dcc_combat_ref, wound | COMBAT_UNTIL_HEALED)) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, wound);
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, wound | COMBAT_UNTIL_HEALED);
		Feedback(fmt::format(u"{}'s wound is closed", Name(this)), FT_COMBAT);
	}
	if (fxqueue.HasEffectWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Subdued))) {
		fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Subdued));
		Modified[IE_DCC_SUBDUED] = 0;
		SetStance(IE_ANI_GET_UP);
		Feedback(fmt::format(u"{} comes to", Name(this)), FT_COMBAT);
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
	const Game* game = core->GetGame();
	dccDiedTime = game ? std::max<ieDword>(game->GameTime, 1) : 1;
	// its group may lose heart (combat.md "Morale")
	DCCGroupMorale();
	if (!IsDCCCharacter()) return;
	BaseStats[IE_DCC_DIEDAT] = game ? std::max<ieDword>(game->GameTime, 1) : 1;
	BaseStats[IE_DCC_BODY] = 0;
	// the funnel (Phase 3): a peasant in it is now one of its dead, with the time and place
	if (dccExt.funnel.status == DCC::FunnelStatus::Alive) {
		dccExt.funnel.status = DCC::FunnelStatus::Dead;
		dccExt.funnel.diedAt = BaseStats[IE_DCC_DIEDAT];
		const Map* map = GetCurrentArea();
		std::string area = map ? std::string(map->GetScriptRef().c_str()) : std::string();
		std::transform(area.begin(), area.end(), area.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		dccExt.funnel.diedIn = area;
	}
}

bool Actor::DCCBodyRecoverable() const
{
	const Game* game = core->GetGame();
	if (!Rules() || !game || !(Modified[IE_STATE_ID] & STATE_DEAD) || !IsDCCCharacter()) return false;
	if ((LastDamageType & (DAMAGE_CHUNKING | DAMAGE_DISINTEGRATE)) || !BaseStats[IE_DCC_DIEDAT]) return false;
	int seconds = static_cast<int>((game->GameTime - BaseStats[IE_DCC_DIEDAT]) / core->Time.defaultTicksPerSec);
	return CanRecoverBody(Signed(BaseStats[IE_LEVEL]), seconds, BaseStats[IE_DCC_BODY]);
}

int Actor::DCCRecoverBody(const Actor* ally, int natural)
{
	// a living ally in the same area, able to act
	if (ally && ((ally->GetStat(IE_STATE_ID) & STATE_DEAD) || ally->IsDCCHeld() || ally->GetCurrentArea() != GetCurrentArea())) return -1;
	if (!DCCBodyRecoverable()) return -1;

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
	if (dccExt.funnel.status == DCC::FunnelStatus::Dead) {
		dccExt.funnel.status = DCC::FunnelStatus::Alive;
		dccExt.funnel.flags |= DCC::FUNNEL_RECOVERED;
		dccExt.funnel.diedAt = 0;
		dccExt.funnel.diedIn.clear();
	}
	Feedback(line + fmt::format(u"alive at 1 hp, -1 {} for good, {} to all rolls for an hour", names[which], BODY_RECOVERY_PENALTY), FT_COMBAT);
	return 1;
}

// ---------------------------------------------------------------------------
// combat completion (M2.3b): free attacks, grappling, withdrawal, morale, the per-round
// effects, injuries and the combat options

AttackRecord Actor::DCCFreeAttack(Actor* target, const String& why)
{
	AttackRecord none;
	if (!Rules() || !target || OutOfFight(target) || target == this) return none;
	if (IsDCCHeld() || Immobile() || GetStat(IE_DCC_NOATTACK) || GetStat(IE_DCC_PRONE) || (GetDCCInjuries() & INJ_BOTH_ARMS)) return none;
	DCCSwing swing;
	if (!DCCBuildSwing(swing, 0, target, 0, false)) return none;
	Feedback(fmt::format(u"{} gets a free attack: {}", Name(this), why), FT_COMBAT);
	size_t before = dccLastAttack.size();
	DCCResolveSwing(swing, target, 1, nullptr);
	if (swing.record.hit) DCCApplySwingDamage(target, swing, swing.damage);
	return dccLastAttack.size() > before ? dccLastAttack[before] : swing.record;
}

// grappling: the action die + attack bonus + the better of Str and Agi (monsters: HD), the larger
// side + its size bonus
static int GrappleRoll(Actor* who, int sizeBonus, int& natural)
{
	bool character = who->IsDCCCharacter();
	int str = character ? who->GetDCCAbilityMod(Ability::Str) : 0;
	int agi = character ? who->GetDCCAbilityMod(Ability::Agi) : 0;
	int die = who->GetStat(IE_DCC_ACTDIE1) ? who->GetStat(IE_DCC_ACTDIE1) : 20;
	natural = who->DCCDie(die);
	return natural + Signed(who->GetStat(IE_DCC_ATTACK)) + GrappleModifier(character, str, agi, who->GetDCCHitDice()) + sizeBonus + Signed(who->GetStat(IE_DCC_ROLLMOD));
}

static int CreatureSize(const Actor* actor)
{
	return std::max(1, actor->CircleSize2Radius());
}

int Actor::DCCGrapple(Actor* target)
{
	if (!Rules() || !target || target == this || OutOfFight(target)) return -1;
	if (IsDCCHeld() || Immobile() || GetStat(IE_DCC_NOATTACK) || (GetDCCInjuries() & INJ_BOTH_ARMS)) return -1;
	int mine = CreatureSize(this);
	int theirs = CreatureSize(target);
	int natural = 0;
	int otherNatural = 0;
	int total = GrappleRoll(this, mine > theirs ? GrappleSizeBonus(mine, theirs) : 0, natural);
	int defence = GrappleRoll(target, theirs > mine ? GrappleSizeBonus(theirs, mine) : 0, otherNatural);
	const Game* game = core->GetGame();
	dccCombatTime = target->dccCombatTime = game ? ieDword(game->GameTime) : 0;
	bool wins = GrappleWins(total, defence);
	String line = fmt::format(u"{} grapples {}: {} vs {}, ", Name(this), Name(target), total, defence);
	if (!wins) {
		Feedback(line + u"fails", FT_COMBAT);
		return 0;
	}
	// pinned until it wins a grapple of its own (or the holder lets go)
	Duration turn;
	turn.kind = Duration::Kind::Turns;
	turn.dice.add = 1;
	target->fxqueue.RemoveAllEffectsWithParam(fx_dcc_combat_ref, static_cast<ieDword>(CombatEffect::Pinned));
	ApplyCombat(CombatEffect::Pinned, static_cast<int>(GetGlobalID()), turn, target, this);
	Feedback(line + u"pinned", FT_COMBAT);
	return 1;
}

// a forced move (a fling, a push) breaks a creature's melee engagements without a withdrawal
void Actor::DCCDisengage()
{
	const Map* map = GetCurrentArea();
	for (const auto& entry : dccEngaged) {
		Actor* foe = map ? map->GetActorByGlobalID(entry.first) : nullptr;
		if (foe) foe->dccEngaged.erase(GetGlobalID());
	}
	dccEngaged.clear();
}

// whether mover standing at a point would be within reach feet of foe (WithinPersonalRange from there)
static bool InReachFrom(const Actor* foe, const Actor* mover, const Point& at, int reach)
{
	float_t angle = AngleFromPoints(foe->Pos, at);
	long dx = at.x - foe->Pos.x;
	long dy = at.y - foe->Pos.y;
	// PersonalDistance's circle sizes (its DistanceFactor of 4)
	double distance = std::hypot(dx, dy) - (foe->CircleSize2Radius() + mover->CircleSize2Radius()) * 4.0;
	return std::max(0.0, distance) <= Feet2Pixels(reach, angle);
}

// a voluntary step (Actor::DoStep): before is where the step started
void Actor::DCCCheckWithdrawal(const Point& before)
{
	if (dccEngaged.empty() || !Rules()) return;
	const Game* game = core->GetGame();
	const Map* map = GetCurrentArea();
	if (!game || !map) return;
	ieDword now = game->GameTime;
	ieDword window = 2 * core->Time.attack_round_size;
	for (auto it = dccEngaged.begin(); it != dccEngaged.end();) {
		Actor* foe = map->GetActorByGlobalID(it->first);
		bool recent = now <= it->second + window;
		if (!foe || !recent || OutOfFight(foe)) {
			it = dccEngaged.erase(it);
			continue;
		}
		int reach = std::max<int>(foe->GetWeaponRange(false), MELEE_REACH);
		bool inReach = WithinPersonalRange(foe, this, reach);
		// displaced out of reach (a push, a fling, a teleport) before this step: not a withdrawal
		bool wasInReach = inReach || InReachFrom(foe, this, before, reach);
		bool canAttack = !(foe->weaponInfo[0].wflags & WEAPON_RANGED) && !foe->IsDCCHeld() && !foe->Immobile() && !foe->GetStat(IE_DCC_NOATTACK) &&
				 !foe->GetStat(IE_DCC_PRONE) && !(foe->GetStat(IE_STATE_ID) & STATE_PANIC);
		// still in reach, or walking towards the foe (around it, after it): no withdrawal yet
		if (inReach || SquaredDistance(foe->Pos, Destination) < SquaredDistance(foe->Pos, Pos)) {
			if (!inReach) it = dccEngaged.erase(it);
			else ++it;
			continue;
		}
		// it left the foe's reach on its own feet: one free attack (combat.md "Withdrawal")
		it = dccEngaged.erase(it);
		foe->dccEngaged.erase(GetGlobalID());
		if (WithdrawalProvokes(true, wasInReach, false, canAttack)) {
			foe->DCCFreeAttack(this, fmt::format(u"{} withdraws", Name(this)));
		}
	}
}

bool Actor::DCCCheckMorale(const Scriptable* from, int dc, const String& why, bool pcs)
{
	if (!Rules() || OutOfFight(this) || IsDCCHeld()) return true;
	if (InParty && !pcs) return true; // the party never checks (combat.md)
	// mindless creatures: the stock "never panics" (morale break 0)
	if (!InParty && !GetStat(IE_MORALEBREAK)) return true;
	int will = Signed(GetStat(IE_DCC_WILL));
	// the CRE's morale as the situational modifier, plus morale effects (a rallying deed)
	int mod = InParty ? 0 : MoraleModifier(Signed(BaseStats[IE_MORALE])) + Signed(Modified[IE_MORALE]) - Signed(BaseStats[IE_MORALE]);
	int natural = DCCDie(20);
	bool holds = MoraleHolds(natural, will, mod, dc);
	int total = natural + will + mod;
	String outcome = holds ? String(u"stands") : (InParty ? String(u"stunned for a round") : String(u"flees"));
	Feedback(fmt::format(u"{} checks morale ({}): d20 {}{} = {} vs {}: {}", Name(this), why, natural, Plus(will + mod), total, dc, outcome), FT_COMBAT);
	if (holds) return true;
	if (InParty) {
		Duration round;
		round.dice.add = 1;
		ApplyFor(fx_dcc_stun_ref, 0, 0, round, this, this);
		return false;
	}
	// flees: the stock panic, running from the one it fears, for a turn
	Duration turn;
	turn.kind = Duration::Kind::Turns;
	turn.dice.add = 1;
	ApplyCombat(CombatEffect::Fleeing, 1, turn, this, this);
	Panic(from, PanicMode::RunAway);
	return false;
}

// whether a creature counts as down for its group's morale
static bool Down(const Actor* actor)
{
	return OutOfFight(actor) || actor->IsDCCHeld() || (actor->GetStat(IE_STATE_ID) & (STATE_HELPLESS | STATE_SLEEP));
}

void Actor::DCCGroupMorale()
{
	const Map* map = GetCurrentArea();
	const Game* game = core->GetGame();
	if (!map || !game || !Rules()) return;
	ieDword now = game->GameTime;
	ieDword fight = 10 * core->Time.round_size; // deaths within the last turn count for this fight
	std::vector<Actor*> group;
	MoraleGroup state;
	int highest = GetDCCHitDice();
	bool leader = true;
	for (Actor* other : map->GetAllActorsInRadius(Pos, GA_NO_LOS | GA_NO_UNSCHEDULED, 40)) {
		if (other == this || Side(other) != Side(this)) continue;
		bool dead = (other->GetStat(IE_STATE_ID) & STATE_DEAD) || (other->GetInternalFlag() & IF_REALLYDIED);
		if (dead) {
			if (other->dccDiedTime && now <= other->dccDiedTime + fight) {
				++state.down;
				++state.deadRecently;
			}
			continue;
		}
		if (Down(other)) {
			++state.down;
		} else {
			++state.standing;
			group.push_back(other);
		}
		if (other->GetDCCHitDice() >= highest) leader = false;
	}
	// this one is dying now
	++state.down;
	++state.deadRecently;
	state.leaderDied = leader && !group.empty();
	const Actor* killer = game->GetActorByGlobalID(objects.LastHitter);
	for (Actor* member : group) {
		if (member->InParty) continue;
		uint32_t triggers = MoraleTriggers(state, member->dccMoraleDone);
		if (!triggers) continue;
		member->dccMoraleDone |= triggers;
		const char16_t* why = (triggers & MORALE_LEADER) ? u"its leader fell" : ((triggers & MORALE_HALF_DOWN) ? u"half its side is down" : u"the first of its side fell");
		member->DCCCheckMorale(killer, MORALE_DC, why);
	}
}

void Actor::DCCHurt(int oldHP, int newHP)
{
	if (InParty || newHP >= oldHP || !Rules() || (dccMoraleDone & MORALE_ALONE_HURT)) return;
	int maxHP = Signed(GetStat(IE_MAXHITPOINTS));
	if (maxHP <= 0 || newHP * 2 > maxHP || newHP <= 0) return;
	// alone: nobody of its side standing near it
	const Map* map = GetCurrentArea();
	if (!map) return;
	for (const Actor* other : map->GetAllActorsInRadius(Pos, GA_NO_LOS | GA_NO_DEAD | GA_NO_UNSCHEDULED, 40)) {
		if (other != this && Side(other) == Side(this) && !Down(other)) return;
	}
	MoraleGroup state;
	state.standing = 1;
	state.alone = true;
	state.halfHurt = true;
	if (!MoraleTriggers(state, dccMoraleDone)) return;
	dccMoraleDone |= MORALE_ALONE_HURT;
	const Game* game = core->GetGame();
	DCCCheckMorale(game ? game->GetActorByGlobalID(objects.LastHitter) : nullptr, MORALE_DC, u"alone and hurt");
}

bool Actor::DCCRoundEffect(Effect& fx)
{
	static const char16_t* abilities[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
	int value = Signed(fx.Parameter1);
	switch (static_cast<CombatEffect>(fx.Parameter2 & COMBAT_KIND_MASK)) {
		case CombatEffect::StandCheck: {
			// once a round: an ability check to get up or free
			int dc = value & 0xff;
			int ability = std::min((value >> 8) & 0xff, ABILITY_COUNT - 1);
			RollResult roll = AbilityCheck(this, static_cast<Ability>(ability), dc);
			bool prone = value & STAND_PRONE;
			Feedback(fmt::format(u"{} tries to {}: {} check {}{} = {} vs DC {}: {}", Name(this), prone ? u"get up" : u"get free", abilities[ability], roll.natural,
					     Plus(roll.total - roll.natural), roll.total, dc, roll.success ? (prone ? u"up" : u"free") : u"not yet"),
				 FT_COMBAT);
			if (roll.success && prone) SetStance(IE_ANI_GET_UP);
			return !roll.success;
		}
		case CombatEffect::Wound: {
			DiceExpr dice;
			dice.count = value & 0xff;
			dice.sides = (value >> 8) & 0xff;
			int ability = ((value >> 16) & 0xff) - 1;
			int damage = std::max(1, RollDice(dice, [this](int sides) { return DCCDie(sides); }));
			Feedback(fmt::format(u"{}'s wound: {} damage{}", Name(this), damage, ability >= 0 ? fmt::format(u", -1 {}", abilities[ability]) : String()), FT_COMBAT);
			if (ability >= 0) {
				auto loss = EffectQueue::CreateEffect(fx_dcc_loss_ref, 1, ability | 0x100, FX_DURATION_INSTANT_PERMANENT);
				loss->Target = FX_TARGET_PRESET;
				core->ApplyEffect(std::move(loss), this, this);
			}
			DirectDamage(this, damage, this);
			return true;
		}
		case CombatEffect::Choke: {
			RollResult roll = DCCRollSave(Save::Fort, 0, value, DCCDie(20));
			if (roll.success) {
				Feedback(fmt::format(u"{} catches its breath: Fort save {} vs DC {}", Name(this), roll.total, value), FT_COMBAT);
				return false;
			}
			int points = DCCDie(4);
			Feedback(fmt::format(u"{} chokes: Fort save {} vs DC {}, -{} Sta", Name(this), roll.total, value, points), FT_COMBAT);
			auto loss = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, static_cast<ieDword>(Ability::Sta) | 0x100, FX_DURATION_INSTANT_PERMANENT);
			loss->Target = FX_TARGET_PRESET;
			core->ApplyEffect(std::move(loss), this, this);
			return true;
		}
		case CombatEffect::Numb: {
			// DC 2, 3, 4, ...: one failure paralyzes until healed; DC 20 passed shakes it off
			RollResult roll = DCCRollSave(Save::Fort, 0, value, DCCDie(20));
			Feedback(fmt::format(u"{} fights the numbness: Fort save {} vs DC {}: {}", Name(this), roll.total, value,
					     roll.success ? (value >= 20 ? u"shaken off" : u"holds") : u"paralyzed"),
				 FT_COMBAT);
			if (!roll.success) {
				Duration healed;
				healed.kind = Duration::Kind::UntilHealed;
				ApplyFor(fx_dcc_hold_ref, 0, 0, healed, this, this);
				return false;
			}
			if (value >= 20) return false;
			++fx.Parameter1;
			return true;
		}
		case CombatEffect::Pinned: {
			// the holder must stay on it; each round the pinned one tries a grapple of its own
			const Game* game = core->GetGame();
			Actor* holder = game ? game->GetActorByGlobalID(fx.Parameter1) : nullptr;
			if (!holder || holder->GetCurrentArea() != GetCurrentArea() || OutOfFight(holder) || holder->IsDCCHeld() ||
			    !WithinPersonalRange(holder, this, MELEE_REACH + 5)) {
				Feedback(fmt::format(u"{} is no longer pinned", Name(this)), FT_COMBAT);
				return false;
			}
			int mine = CreatureSize(this);
			int theirs = CreatureSize(holder);
			int natural = 0;
			int holderNatural = 0;
			int total = GrappleRoll(this, mine > theirs ? GrappleSizeBonus(mine, theirs) : 0, natural);
			int defence = GrappleRoll(holder, theirs > mine ? GrappleSizeBonus(theirs, mine) : 0, holderNatural);
			bool free = GrappleWins(total, defence);
			Feedback(fmt::format(u"{} struggles against {}'s hold: {} vs {}: {}", Name(this), Name(holder), total, defence, free ? u"free" : u"still pinned"), FT_COMBAT);
			return !free;
		}
		default:
			return true;
	}
}

std::vector<String> Actor::DCCInjuryList() const
{
	std::vector<String> lines;
	const Game* game = core->GetGame();
	ieDword now = game ? ieDword(game->GameTime) : 0;
	int combatOpcode = EffectQueue::ResolveEffect(fx_dcc_combat_ref);
	auto how = [now, combatOpcode](const Effect& fx) {
		bool combat = static_cast<int>(fx.Opcode) == combatOpcode;
		if (fx.SourceRef == PermanentSource || (combat && (fx.Parameter2 & COMBAT_FOR_GOOD))) return String(u" (for good)");
		if (fx.SourceRef == UntilHealedSource || (combat && (fx.Parameter2 & COMBAT_UNTIL_HEALED))) return String(u" (until healed)");
		if (fx.TimingMode == FX_DURATION_INSTANT_PERMANENT_AFTER_BONUSES || fx.TimingMode == FX_DURATION_INSTANT_PERMANENT) return String(u" (for good)");
		if (fx.Duration > now) {
			ieDword rounds = (fx.Duration - now + core->Time.round_size - 1) / core->Time.round_size;
			return fmt::format(u" ({} round{})", rounds, rounds == 1 ? u"" : u"s");
		}
		return String();
	};
	static EffectRef refs[] = { fx_dcc_blind_ref, fx_dcc_deaf_ref, fx_dcc_silence_ref, fx_dcc_hold_ref, fx_dcc_movement_ref, fx_dcc_sleep_ref };
	static const char16_t* names[] = { u"blind", u"deaf", u"can't speak", u"paralyzed", u"slowed", u"unconscious" };
	auto it = fxqueue.GetFirstEffect();
	while (const Effect* next = fxqueue.GetNextEffect(it)) {
		const Effect& fx = *next;
		bool lasting = fx.SourceRef == PermanentSource || fx.SourceRef == UntilHealedSource;
		if (static_cast<int>(fx.Opcode) == combatOpcode) {
			lasting = lasting || (fx.Parameter2 & (COMBAT_UNTIL_HEALED | COMBAT_FOR_GOOD));
			switch (static_cast<CombatEffect>(fx.Parameter2 & COMBAT_KIND_MASK)) {
				case CombatEffect::Injury:
					lines.push_back(Widen(InjuryName(static_cast<Injury>(fx.Parameter1))) + how(fx));
					break;
				case CombatEffect::AttackMod:
					if (lasting) lines.push_back(fmt::format(u"{:+} to attack", Signed(fx.Parameter1)) + how(fx));
					break;
				case CombatEffect::ACMod:
					if (lasting) lines.push_back(fmt::format(u"{:+} AC", Signed(fx.Parameter1)) + how(fx));
					break;
				case CombatEffect::NoAttack:
					if (lasting) lines.push_back(u"can't attack" + how(fx));
					break;
				case CombatEffect::RollMod:
					if (lasting) lines.push_back(fmt::format(u"{:+} to all rolls", Signed(fx.Parameter1)) + how(fx));
					break;
				case CombatEffect::Wound:
					lines.push_back(u"bleeding wound" + how(fx));
					break;
				case CombatEffect::Doomed:
					lines.push_back(u"mortally wounded" + how(fx));
					break;
				default:
					break;
			}
			continue;
		}
		if (!lasting) continue;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
			if (static_cast<int>(fx.Opcode) != EffectQueue::ResolveEffect(refs[i])) continue;
			String what = names[i];
			if (i == 4) what = fx.Parameter1 ? fmt::format(u"speed {}%", fx.Parameter1) : String(u"can't walk");
			lines.push_back(what + how(fx));
		}
	}
	return lines;
}

uint32_t Actor::GetDCCInjuries() const
{
	return GetStat(IE_DCC_INJURY);
}

uint32_t Actor::GetDCCCombatOptions() const
{
	return BaseStats[IE_DCC_COMBATOPT];
}

void Actor::SetDCCCombatOptions(uint32_t options, int rage)
{
	uint32_t old = BaseStats[IE_DCC_COMBATOPT];
	SetBase(IE_DCC_COMBATOPT, options & (OPT_CHARGE | OPT_GRAPPLE | OPT_SUBDUAL));
	// the run-up of a charge starts where it's declared
	if ((options & OPT_CHARGE) && !(old & OPT_CHARGE)) dccChargeFrom = Pos;
	if (rage >= 0) SetBase(IE_DCC_RAGE, std::min(rage, RAGE_MAX_SETTING));
}

void Actor::DCCNightlyInjuries()
{
	uint32_t injuries = GetDCCInjuries();
	// an infection and grave rot: a Fort save each night or -1d4 Sta; rot: -1 Per (lethal at 0)
	for (const auto& rot : { std::make_pair(INJ_INFECTED, 10), std::make_pair(INJ_GRAVE_ROT, 16) }) {
		if (!(injuries & rot.first)) continue;
		RollResult roll = DCCRollSave(Save::Fort, 0, rot.second, DCCDie(20));
		int points = roll.success ? 0 : DCCDie(4);
		Feedback(fmt::format(u"{}'s {} overnight: Fort save {} vs DC {}: {}", Name(this), Widen(InjuryName(rot.first)), roll.total, rot.second,
				     roll.success ? String(u"holds") : fmt::format(u"-{} Sta", points)),
			 FT_MISC);
		if (!points) continue;
		auto loss = EffectQueue::CreateEffect(fx_dcc_loss_ref, points, static_cast<ieDword>(Ability::Sta) | 0x100, FX_DURATION_INSTANT_PERMANENT);
		loss->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(loss), this, this);
	}
	if (injuries & INJ_ROT) {
		Feedback(fmt::format(u"{}'s flesh rots overnight: -1 Per", Name(this)), FT_MISC);
		auto loss = EffectQueue::CreateEffect(fx_dcc_loss_ref, 1, static_cast<ieDword>(Ability::Per) | 0x100, FX_DURATION_INSTANT_PERMANENT);
		loss->Target = FX_TARGET_PRESET;
		core->ApplyEffect(std::move(loss), this, this);
	}
}

}
