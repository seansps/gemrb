// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC traps and thief skills in play: the thief skill roll (d20 + the Table 1-9 bonus + the ability,
// a halfling's sneak & hide, anyone's d20 + the ability on traps, the occupation rules for the other
// skills) with its log line, Luck and the deadly disarm's prompt; Detect traps for everyone (the find
// roll, once per party slot, secret on a failure); disarming (a natural 1 or a miss by 5 or more
// springs the trap); picking locks; Stealth (hide in shadows by the light, sneak silently by the
// floor, rolled again on a new DC); pick pocket; who is aware of a thief (backstab, hiding again in a
// fight). The rules are pure functions in core/DCC/Thief.h. See docs/rules/traps.md, classes.md and
// docs/architecture/rules-dcc.md, "Traps and thief skills".

#include "defsounds.h"
#include "ie_stats.h"

#include "DisplayMessage.h"
#include "EffectQueue.h"
#include "Game.h"
#include "GameData.h"
#include "Interface.h"
#include "Item.h"
#include "Map.h"
#include "Polygon.h"
#include "Prompts.h"
#include "TileMap.h"

#include "Audio/Playback.h"
#include "DCC/Luck.h"
#include "DCC/Rules.h"
#include "DCC/Skills.h"
#include "DCC/Stats.h"
#include "DCC/Thief.h"
#include "GUI/GameControl.h"
#include "GameScript/GSUtils.h"
#include "Logging/Logging.h"
#include "Net/NetSession.h"
#include "Scriptable/Actor.h"
#include "Scriptable/Container.h"
#include "Scriptable/Door.h"
#include "Scriptable/InfoPoint.h"

#include <algorithm>
#include <cmath>

namespace GemRB {

using namespace DCC;

static ThiefCheckResult lastThiefCheck;
static EffectRef fx_dcc_invisible_ref = { "State:Invisible", -1 };
static const ResRef STEALTH_SOURCE = "DCCSTLTH";
// melee reach for breaking away (ActorDCCCombat.cpp's MELEE_REACH)
constexpr int REACH_FEET = 5;

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
	String wide;
	for (unsigned char c : text) wide += char16_t(c);
	return wide;
}

static const char16_t* AbilityShort(Ability ability)
{
	static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
	size_t i = static_cast<size_t>(ability);
	return i < static_cast<size_t>(ABILITY_COUNT) ? names[i] : u"?";
}

static ieDword Now()
{
	const Game* game = core->GetGame();
	return game ? ieDword(game->GameTime) : 0;
}

static int RoundTicks()
{
	return int(core->Time.attack_round_size);
}

static bool IsClass(const Actor* actor, ClassID id)
{
	return actor->IsDCCCharacter() && actor->GetBase(IE_CLASS) == static_cast<ieDword>(id) && Signed(actor->GetBase(IE_LEVEL)) >= 1;
}

// --- the roll -------------------------------------------------------------------------------------

bool Actor::DCCHasThievesTools() const
{
	const RulesData* rules = Rules();
	if (!rules || rules->thievesTools.empty()) return false;
	for (int slot = 0; slot < inventory.GetSlotCount(); ++slot) {
		const CREItem* item = inventory.GetSlotItem(slot);
		if (!item) continue;
		std::string name = item->ItemResRef.c_str();
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
		if (rules->thievesTools.count(name)) return true;
	}
	return false;
}

int Actor::DCCArmorCheckPenalty() const
{
	const RulesData* rules = Rules();
	if (!rules) return 0;
	int penalty = 0;
	const CREItem* armor = inventory.GetSlotItem(Inventory::GetArmorSlot());
	if (armor) {
		std::string animation;
		const Item* itm = gamedata->GetItem(armor->ItemResRef, true);
		if (itm) {
			animation = itm->AnimationType.c_str();
			gamedata->FreeItem(itm, armor->ItemResRef);
		}
		penalty += rules->combat.armorFumbles.ArmorCheck(armor->ItemResRef.c_str(), animation);
	}
	const CREItem* shield = DCCShieldInHand();
	if (shield) penalty += rules->combat.armorFumbles.ShieldCheck(shield->ItemResRef.c_str());
	return std::min(0, penalty);
}

// the occupation tags a non-thief rolls a thief skill with (funnel.md: anyone may try, untrained d10)
static const char* OccupationTags(ThiefSkill skill)
{
	switch (skill) {
		case ThiefSkill::PickLock:
			return "LOCKS";
		case ThiefSkill::PickPocket:
			return "STEALTH_STREET";
		case ThiefSkill::Hide:
		case ThiefSkill::Sneak:
			return "STEALTH_STREET|OUTDOORS";
		case ThiefSkill::Climb:
			return "CLIMB";
		default:
			return "UNTRAINABLE";
	}
}

ThiefPlan Actor::DCCThiefPlan(ThiefSkill skill, int dc, Ability ability, int situational, int dwarf) const
{
	const RulesData* rules = Rules();
	ThiefCheckInput in;
	in.skill = skill;
	in.ability = ability;
	in.thief = IsClass(this, ClassID::Thief);
	in.halfling = IsClass(this, ClassID::Halfling);
	in.tools = DCCHasThievesTools();
	int level = Signed(BaseStats[IE_LEVEL]);
	if (rules && in.thief) {
		in.tableBonus = rules->thiefSkills.Bonus(AlignmentFromIE(GetStat(IE_ALIGNMENT)), level, skill);
	} else if (rules && in.halfling) {
		const LevelRow* row = rules->classes.Row(BaseStats[IE_CLASS], level);
		in.tableBonus = row ? row->sneakHide : 0;
	}
	in.abilityMod = GetDCCAbilityMod(ability);
	AugurRoll augur = AugurRoll::SkillCheck;
	if (skill == ThiefSkill::FindTrap) augur = AugurRoll::FindTrap;
	if (skill == ThiefSkill::DisableTrap) augur = AugurRoll::DisableTrap;
	in.augur = GetDCCAugurBonus(augur);
	in.armor = DCCArmorCheckPenalty();
	in.dwarf = dwarf;
	in.situational = situational;
	bool trap = skill == ThiefSkill::FindTrap || skill == ThiefSkill::DisableTrap;
	if (HasThiefBonus(in) || trap) return PlanThiefCheck(in);

	// anything else by the occupation rules: trained d20, familiar d10 + 2, untrained d10
	SkillTags tags = ParseSkillTags(OccupationTags(skill));
	int code = static_cast<int>(ability) + SKILL_ABILITY_STR;
	SkillPlan occupation = DCCSkillPlan(tags, code, dc, situational);
	ThiefPlan plan;
	plan.ability = ability;
	plan.die = occupation.die;
	plan.roller = occupation.die == SKILL_TRAINED_DIE ? ThiefRoller::Trained : (occupation.familiarity ? ThiefRoller::Familiar : ThiefRoller::Untrained);
	plan.familiarity = occupation.familiarity;
	plan.abilityMod = occupation.abilityMod;
	plan.augur = occupation.augur;
	plan.armor = occupation.armor;
	plan.situational = occupation.situational;
	return plan;
}

static void Part(String& line, int value, const char16_t* what)
{
	if (!value) return;
	if (what && *what) {
		line += fmt::format(u" {:+d} {}", value, what);
	} else {
		line += fmt::format(u" {:+d}", value);
	}
}

static String Verb(const ThiefCheckResult& r, const String& what)
{
	switch (r.skill) {
		case ThiefSkill::FindTrap:
			return u"finds a trap";
		case ThiefSkill::DisableTrap:
			return what.empty() ? String(u"tries to disarm a trap") : fmt::format(u"tries to disarm {}", what);
		case ThiefSkill::PickLock:
			return what.empty() ? String(u"tries to pick a lock") : fmt::format(u"tries to pick the lock of {}", what);
		case ThiefSkill::PickPocket:
			return what.empty() ? String(u"tries to pick a pocket") : fmt::format(u"tries to pick {}'s pocket", what);
		case ThiefSkill::Hide:
			return u"hides in shadows";
		case ThiefSkill::Sneak:
			return u"sneaks silently";
		case ThiefSkill::Climb:
			return u"climbs";
		default:
			return fmt::format(u"rolls {}", Widen(ThiefSkillKey(r.skill)));
	}
}

static String OutcomeWords(const ThiefCheckResult& r)
{
	switch (r.skill) {
		case ThiefSkill::FindTrap:
			return String();
		case ThiefSkill::DisableTrap:
			if (r.outcome == CheckOutcome::Success) return u"disarmed";
			return r.outcome == CheckOutcome::Botch ? u"it springs!" : u"missed, still armed";
		case ThiefSkill::PickLock:
			return r.success ? u"open" : u"still locked";
		case ThiefSkill::PickPocket:
			if (r.outcome == CheckOutcome::Success) return u"takes it";
			return r.outcome == CheckOutcome::Botch ? u"caught!" : u"nothing taken, unnoticed";
		case ThiefSkill::Hide:
			return r.success ? u"unseen" : u"seen";
		case ThiefSkill::Sneak:
			return r.success ? u"unheard" : u"heard";
		default:
			return r.success ? u"success" : u"failure";
	}
}

// "Vex hides in shadows: 9 +3 +1 Agi = 13 vs DC 10 (dim): unseen"
static String ThiefLine(const Actor* actor, const ThiefCheckResult& r, const String& what)
{
	String line = fmt::format(u"{} {}", actor->GetName(), Verb(r, what));
	ThiefRoller roller = r.plan.roller;
	if (roller == ThiefRoller::Trained || roller == ThiefRoller::Familiar || roller == ThiefRoller::Untrained) {
		line += fmt::format(u" ({})", Widen(ThiefRollerName(roller)));
	}
	line += fmt::format(u": {}", r.natural);
	Part(line, r.plan.skill, u"");
	Part(line, r.plan.familiarity, u"familiar");
	Part(line, r.plan.abilityMod, AbilityShort(r.plan.ability));
	Part(line, r.plan.augur, u"augur");
	Part(line, r.plan.armor, u"armour");
	Part(line, r.plan.dwarf, u"dwarf");
	Part(line, r.plan.situational, u"situation");
	Part(line, r.rollMod, u"groggy");
	Part(line, r.luckBonus, u"Luck");
	line += fmt::format(u" = {} vs DC {}", r.total, r.dc);
	if (!r.dcLabel.empty()) line += fmt::format(u" ({})", Widen(r.dcLabel));
	String outcome = OutcomeWords(r);
	if (!outcome.empty()) line += fmt::format(u": {}", outcome);
	if (r.cursed) line += u" (cursed by fortune)";
	return line;
}

static String FloatWords(const ThiefCheckResult& r)
{
	switch (r.skill) {
		case ThiefSkill::FindTrap:
			return u"A trap!";
		case ThiefSkill::DisableTrap:
			if (r.outcome == CheckOutcome::Success) return u"Disarmed";
			return r.outcome == CheckOutcome::Botch ? u"Sprung!" : u"Disarm failed";
		case ThiefSkill::PickLock:
			return r.success ? u"Lock picked" : u"Lock holds";
		case ThiefSkill::PickPocket:
			if (r.outcome == CheckOutcome::Success) return u"Pocket picked";
			return r.outcome == CheckOutcome::Botch ? u"Caught!" : u"Nothing";
		case ThiefSkill::Hide:
			return r.success ? u"Hidden" : u"Seen";
		case ThiefSkill::Sneak:
			return r.success ? u"Silent" : u"Heard";
		default:
			return r.success ? u"Success" : u"Failure";
	}
}

static void ReportThiefCheck(Actor* actor, const ThiefCheckResult& result, const String& what, uint32_t flags)
{
	lastThiefCheck = result;
	if ((flags & THIEF_SECRET) && !result.success) {
		// the find roll is secret: a failure says nothing (traps.md, "Finding")
		Log(DEBUG, "DCC", "{} misses a hidden trap (secret roll {} vs {})", fmt::WideToChar { actor->GetName() }, result.total, result.dc);
		return;
	}
	String line = ThiefLine(actor, result, what);
	NetSession* net = core->GetNetSession();
	bool captured = net ? net->BeginCapture() : false;
	Actor::DCCFeedback(line, FT_MISC);
	if (!(flags & THIEF_NO_FLOAT) && actor->GetCurrentArea()) {
		static const Color passed(0xa0, 0xff, 0xa0, 0xff);
		static const Color failed(0xff, 0xa0, 0x90, 0xff);
		actor->overHead.SetText(FloatWords(result), true, true, result.success ? passed : failed);
	}
	if (net) net->EndCapture(captured);
}

static void Resolve(ThiefCheckResult& r)
{
	r.success = r.total >= r.dc && !r.cursed;
	r.outcome = BotchOutcome(r.natural, r.cursed ? std::min(r.total, r.dc - 1) : r.total, r.dc);
	// a natural 1 on a disarm springs the trap whatever Luck does; elsewhere only the margin counts
	if (r.skill != ThiefSkill::DisableTrap && r.skill != ThiefSkill::PickPocket) r.outcome = r.success ? CheckOutcome::Success : CheckOutcome::Miss;
}

// a deadly disarm that failed: a party member who pauses on key rolls is asked how much Luck to burn
static bool AskThiefBurn(Actor* actor, const ThiefCheckResult& result, const String& what, uint32_t flags, const Actor::DCCThiefDone& done)
{
	if (!actor->InParty || !actor->DCCPausesOnKeyRolls()) return false;
	const NetSession* net = core->GetNetSession();
	if ((net && !net->IsHost()) || !core->GetGameControl()) return false;
	int luck = actor->GetDCCLuck();
	int gap = result.dc - result.total;
	BurnValue value = actor->GetDCCBurnValue();
	if (luck <= 0 || gap <= 0 || result.natural == 1) return false;

	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "luckburn";
	q.title = "Burn Luck?";
	std::string name = fmt::format("{}", fmt::WideToChar { actor->GetShortName() });
	std::string how = value.IsDie() ? fmt::format("a d{} each", value.luckDie) : fmt::format("+{} each", value.perPoint);
	std::string springs = gap >= BOTCH_MARGIN ? "; it springs unless the miss comes under 5" : "";
	q.text = fmt::format("{} misses a deadly disarm by {} ({} vs DC {}{}). Burn Luck ({}, {} left)?", name, gap, result.total, result.dc, springs, how, luck);
	q.fields = { { "Luck", 0, luck, PolicyBurn(actor->GetDCCLuckPolicy(), BurnReason::Death, gap, luck, value) } };
	q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));

	ieDword selfID = actor->GetGlobalID();
	auto pending = std::make_shared<ThiefCheckResult>(result);
	uint32_t id = core->GetPrompts().Ask(actor, std::move(q), [selfID, pending, what, flags, done](const PromptResult& r) {
		Game* game = core->GetGame();
		Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
		ThiefCheckResult final = *pending;
		final.waiting = false;
		if (self) {
			int points = Clamp<int>(r.Value(), 0, self->GetDCCLuck());
			if (points > 0) {
				BurnValue burnValue = self->GetDCCBurnValue();
				int bonus = self->DCCBurnLuck(points, burnValue);
				final.luckBurned = points;
				final.luckBonus = bonus;
				final.total += bonus;
				Resolve(final);
				String burned = burnValue.IsDie() ? fmt::format(u"{}d{} = +{}", points, burnValue.luckDie, bonus) : fmt::format(u"+{}", bonus);
				Actor::DCCFeedback(fmt::format(u"{} burns {} Luck on a disarm ({}): {} -> {} vs DC {} (Luck {} left)", self->GetName(), points, burned,
							       pending->total, final.total, final.dc, self->GetDCCLuck()),
						   FT_MISC);
			}
			ReportThiefCheck(self, final, what, flags);
		} else {
			lastThiefCheck = final;
		}
		if (done) done(final);
	});
	if (!id) return false;
	Actor::DCCFeedback(fmt::format(u"{} misses a deadly disarm: {} vs DC {}; burn Luck?", actor->GetName(), result.total, result.dc), FT_MISC);
	return true;
}

ThiefCheckResult Actor::DCCThiefCheck(ThiefSkill skill, int dc, const std::string& dcLabel, Ability ability, uint32_t flags, const String& what, int situational,
				      int dwarf, bool ask, const DCCThiefDone& done)
{
	ThiefCheckResult result;
	result.roller = GetGlobalID();
	result.skill = skill;
	result.dc = dc;
	result.dcLabel = dcLabel;
	result.plan = DCCThiefPlan(skill, dc, ability, situational, dwarf);
	// on the actor's dice: the engine's RNG (seeded in the autotests), or forced ones (DCCForceDice)
	result.natural = DCCDie(result.plan.die);
	RollResult roll = DCCRoll(RollKind::Check, result.plan.Modifier(), dc, result.natural, result.plan.die);
	result.rollMod = Signed(GetStat(IE_DCC_ROLLMOD));
	result.cursed = roll.cursed;
	result.total = roll.total;
	Resolve(result);

	bool deadly = flags & SKILL_DEADLY;
	bool natOneSprings = skill == ThiefSkill::DisableTrap && result.natural == 1;
	if (!result.success && !roll.cursed && !natOneSprings && !(flags & SKILL_NO_LUCK) && IsDCCCharacter()) {
		if (ask && deadly && AskThiefBurn(this, result, what, flags, done)) {
			result.waiting = true;
			lastThiefCheck = result;
			return result;
		}
		String label = fmt::format(u"{} ({})", Verb(result, String()), Widen(ThiefSkillKey(skill)));
		int before = roll.total;
		result.luckBurned = DCCPolicyRollBurn(roll, deadly ? BurnReason::Death : BurnReason::Miss, label.c_str());
		result.luckBonus = roll.total - before;
		result.total = roll.total;
		Resolve(result);
	}
	ReportThiefCheck(this, result, what, flags);
	if (done) done(result);
	return result;
}

const ThiefCheckResult& Actor::DCCLastThiefCheck()
{
	return lastThiefCheck;
}

// --- traps and locks ------------------------------------------------------------------------------

static const TrapInfo& TrapData(const Highlightable* trap)
{
	static const TrapInfo defaults;
	const RulesData* rules = Rules();
	const Map* area = trap->GetCurrentArea();
	if (!rules || !area) return defaults;
	const TrapInfo* info = rules->traps.Find(area->GetScriptName().c_str(), trap->GetScriptName().c_str());
	return info ? *info : defaults;
}

static String TrapName(const Highlightable* trap)
{
	return Widen(trap->GetScriptName().c_str());
}

Point Highlightable::DCCNearestPoint(const Point& p) const
{
	if (!outline || outline->Count() < 3) return Pos;
	if (outline->PointIn(p)) return p;
	Point best = outline->vertices[0];
	double bestDist = -1;
	const auto& v = outline->vertices;
	for (size_t i = 0; i < v.size(); ++i) {
		const Point& a = v[i];
		const Point& b = v[(i + 1) % v.size()];
		double dx = b.x - a.x;
		double dy = b.y - a.y;
		double len = dx * dx + dy * dy;
		double t = len > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len : 0;
		t = std::max(0.0, std::min(1.0, t));
		Point q(int(std::lround(a.x + t * dx)), int(std::lround(a.y + t * dy)));
		double d = double(q.x - p.x) * (q.x - p.x) + double(q.y - p.y) * (q.y - p.y);
		if (bestDist < 0 || d < bestDist) {
			bestDist = d;
			best = q;
		}
	}
	return best;
}

void Highlightable::DCCRevealTrap(ieDword actorID)
{
	if (!Trapped || TrapDetected) return;
	SetTrapDetected(1);
	AddTrigger(TriggerEntry(trigger_detected, actorID));
}

bool Highlightable::DCCDetectTrap(Actor* searcher)
{
	if (!searcher || !Trapped || TrapDetected || !CanDetectTrap() || !Scripts[0]) return false;
	uint16_t bit = TrapSlotBit(searcher->InParty);
	if (!bit || (dccRolls.found & bit)) return false;
	dccRolls.found |= bit;
	DCCRollsChanged();
	int dc = TrapDetectionDiff;
	if (dc >= DC_UNROLLABLE) return false; // no roll finds it
	const TrapInfo& info = TrapData(this);
	// dwarves of level 1+ know stone and earth underground (traps.md)
	int dwarf = 0;
	const Map* area = GetCurrentArea();
	if (IsClass(searcher, ClassID::Dwarf) && DwarfKnowsTrap(info.kind) && area && !(area->AreaType & AT_OUTDOOR)) {
		dwarf = Signed(searcher->GetBase(IE_LEVEL));
	}
	ThiefCheckResult r = searcher->DCCThiefCheck(ThiefSkill::FindTrap, dc, std::string(), Ability::Int, SKILL_NO_LUCK | THIEF_SECRET, TrapName(this), 0, dwarf);
	if (!r.success) return false;
	SetTrapDetected(1);
	AddTrigger(TriggerEntry(trigger_detected, searcher->GetGlobalID()));
	return true;
}

static void SpringOrNot(Highlightable* trap, Actor* actor, const ThiefCheckResult& r)
{
	ieDword id = actor->GetGlobalID();
	if (r.outcome == CheckOutcome::Success) {
		trap->AddTrigger(TriggerEntry(trigger_disarmed, id));
		trap->Trapped = 0;
		// the trap's XP goes to a thief (traps.md, "XP"); nobody else gets XP for a trap itself
		const TrapInfo& info = TrapData(trap);
		if (IsClass(actor, ClassID::Thief) && info.xp > 0) actor->AddExperience(info.xp, 0);
		core->GetAudioPlayback().PlayDefaultSound(DS_DISARMED, SFXChannel::Hits);
		GameControl* gc = core->GetGameControl();
		if (gc) gc->ResetTargetMode();
	} else {
		trap->AddTrigger(TriggerEntry(trigger_disarmfailed, id));
		if (r.outcome == CheckOutcome::Botch) trap->TriggerTrap(0, id);
	}
	trap->ImmediateEvent();
}

void Highlightable::DCCTryDisarm(Actor* actor)
{
	if (!actor) return;
	uint16_t bit = TrapSlotBit(actor->InParty);
	if (bit && (dccRolls.disarm & bit)) {
		Actor::DCCFeedback(fmt::format(u"{} has tried to disarm this trap already", actor->GetName()), FT_MISC);
		return;
	}
	const TrapInfo& info = TrapData(this);
	if (!info.disarmable || TrapRemovalDiff >= DC_UNROLLABLE) {
		Actor::DCCFeedback(fmt::format(u"{} can't disarm this by hand: it takes a word, a token or a spell", actor->GetName()), FT_MISC);
		return;
	}
	dccRolls.disarm |= bit;
	DCCRollsChanged();
	uint32_t flags = info.deadly ? SKILL_DEADLY : 0;
	ieDword selfID = GetGlobalID();
	ieDword actorID = actor->GetGlobalID();
	ResRef areaRef;
	if (GetCurrentArea()) areaRef = GetCurrentArea()->GetScriptName();
	actor->DCCThiefCheck(ThiefSkill::DisableTrap, TrapRemovalDiff, std::string(), info.disarm, flags, String(), 0, 0, info.deadly,
			     [selfID, actorID, areaRef](const ThiefCheckResult& r) {
				     if (r.waiting) return;
				     Game* game = core->GetGame();
				     int index = game ? game->FindMap(areaRef) : -1;
				     Map* map = index >= 0 ? game->GetMap(index) : nullptr;
				     Scriptable* scr = map ? map->GetScriptableByGlobalID(selfID) : nullptr;
				     Actor* who = game ? game->GetActorByGlobalID(actorID) : nullptr;
				     auto* trap = dynamic_cast<Highlightable*>(scr);
				     if (trap && who) SpringOrNot(trap, who, r);
			     });
}

bool Highlightable::DCCTryPickLock(Actor* actor, ieWord lockDifficulty, ieStrRef customFailStr, HCStrings failStr)
{
	if (lockDifficulty >= DC_UNROLLABLE) {
		if (int(customFailStr) && customFailStr != ieStrRef::INVALID) {
			displaymsg->DisplayStringName(customFailStr, GUIColors::XPCHANGE, actor, STRING_FLAGS::SOUND | STRING_FLAGS::SPEECH);
		} else {
			displaymsg->DisplayMsgAtLocation(failStr, FT_ANY, actor, actor, GUIColors::XPCHANGE);
		}
		return false;
	}
	uint16_t bit = TrapSlotBit(actor->InParty);
	if (bit && (dccRolls.lock & bit)) {
		Actor::DCCFeedback(fmt::format(u"{} has tried this lock already", actor->GetName()), FT_MISC);
		return false;
	}
	dccRolls.lock |= bit;
	DCCRollsChanged();
	ThiefCheckResult r = actor->DCCThiefCheck(ThiefSkill::PickLock, lockDifficulty, std::string(), Ability::Agi, 0, TrapName(this));
	if (!r.success) {
		AddTrigger(TriggerEntry(trigger_picklockfailed, actor->GetGlobalID()));
		core->GetAudioPlayback().PlayDefaultSound(DS_PICKFAIL, SFXChannel::Hits);
		return false;
	}
	GameControl* gc = core->GetGameControl();
	if (gc) gc->ResetTargetMode();
	core->GetAudioPlayback().PlayDefaultSound(DS_PICKLOCK, SFXChannel::Hits);
	AddTrigger(TriggerEntry(trigger_unlocked, actor->GetGlobalID()));
	ImmediateEvent();
	return true;
}

static ieVariable DoorVariable(const Door* door, char kind)
{
	return ieVariable(fmt::format("DCCD{}_{}", kind, door->ID));
}

void Door::DCCRollsChanged()
{
	Map* map = GetCurrentArea();
	if (!map) return;
	map->locals[DoorVariable(this, 'R')] = ieDword(dccRolls.found) | ieDword(dccRolls.disarm) << 16;
	map->locals[DoorVariable(this, 'L')] = ieDword(dccRolls.lock) | ieDword(dccRolls.flags) << 16;
}

void Door::DCCLoadRolls()
{
	const Map* map = GetCurrentArea();
	if (!map) return;
	auto rolls = map->locals.find(DoorVariable(this, 'R'));
	auto lock = map->locals.find(DoorVariable(this, 'L'));
	if (rolls != map->locals.end()) {
		dccRolls.found = uint16_t(rolls->second & 0xffff);
		dccRolls.disarm = uint16_t(rolls->second >> 16);
	}
	if (lock != map->locals.end()) {
		dccRolls.lock = uint16_t(lock->second & 0xffff);
		dccRolls.flags = uint16_t(lock->second >> 16);
	}
}

// --- Detect traps ---------------------------------------------------------------------------------

// a trap the searcher may roll for: hidden, within 20' of its outline, in sight
static bool InSearch(const Actor* searcher, const Highlightable* trap)
{
	if (!trap->Trapped || trap->TrapDetected || !trap->CanDetectTrap() || !trap->Scripts[0]) return false;
	const Map* area = searcher->GetCurrentArea();
	if (!area) return false;
	Point nearest = trap->DCCNearestPoint(searcher->Pos);
	if (!WithinRange(searcher, nearest, TRAP_SEARCH_FEET)) return false;
	return nearest == searcher->Pos || area->IsVisibleLOS(searcher->Pos, nearest);
}

void Actor::DCCSearchTraps()
{
	Map* map = GetCurrentArea();
	if (!map || !InParty) return;
	const TileMap* tm = map->TMap;
	// the outline of a door, container or region: every searcher in range rolls once (traps.md)
	for (const auto& door : tm->GetDoors()) {
		if (door->Visible() && InSearch(this, door.get())) door->DCCDetectTrap(this);
	}
	for (const auto& container : tm->GetContainers()) {
		if (InSearch(this, container.get())) container->DCCDetectTrap(this);
	}
	for (const auto& ip : tm->GetInfoPoints()) {
		if (InSearch(this, ip.get())) ip->DCCDetectTrap(this);
	}
}

// the tell: a trap's notice line floats once over the first party member to come within 15' of it
void Actor::DCCNoticeTraps(ieDword gameTime)
{
	const RulesData* rules = Rules();
	if (!InParty || !area || !rules || rules->traps.Empty() || gameTime % core->Time.defaultTicksPerSec) return;
	std::string areaName = area->GetScriptName().c_str();
	auto notice = [&](Highlightable* trap) {
		if (!trap->Trapped || (trap->dccRolls.flags & Highlightable::DCC_NOTICE_SAID)) return;
		const TrapInfo* info = rules->traps.Find(areaName, trap->GetScriptName().c_str());
		if (!info || info->notice.empty()) return;
		Point nearest = trap->DCCNearestPoint(Pos);
		if (!WithinRange(this, nearest, TRAP_NOTICE_FEET) || (nearest != Pos && !area->IsVisibleLOS(Pos, nearest))) return;
		trap->dccRolls.flags |= Highlightable::DCC_NOTICE_SAID;
		trap->DCCRollsChanged();
		String line = Widen(info->notice);
		NetSession* net = core->GetNetSession();
		bool captured = net ? net->BeginCapture() : false;
		DCCFeedback(fmt::format(u"{}: {}", GetName(), line), FT_MISC);
		overHead.SetText(line, true, true, Color(0xff, 0xff, 0xc0, 0xff));
		if (net) net->EndCapture(captured);
	};
	for (const auto& door : area->TMap->GetDoors()) notice(door.get());
	for (const auto& container : area->TMap->GetContainers()) notice(container.get());
	for (const auto& ip : area->TMap->GetInfoPoints()) notice(ip.get());
}

Actor* Actor::DCCLowestLuck(const Map* area, const Highlightable* region)
{
	const Game* game = core->GetGame();
	if (!game) return nullptr;
	Actor* lowest = nullptr;
	int lowestLuck = 0;
	for (int i = 0; i < game->GetPartySize(false); ++i) {
		Actor* pc = game->GetPC(i, false);
		if (!pc || pc->GetCurrentArea() != area || (pc->GetStat(IE_STATE_ID) & STATE_DEAD)) continue;
		if (region && region->outline && !region->IsOver(pc->Pos)) continue;
		int luck = Signed(pc->GetStat(IE_DCC_LUCK));
		if (!lowest || luck < lowestLuck) {
			lowest = pc;
			lowestLuck = luck;
		}
	}
	return lowest;
}

// --- Stealth --------------------------------------------------------------------------------------

int Actor::DCCHideDC(std::string* label) const
{
	const RulesData* rules = Rules();
	const Map* map = GetCurrentArea();
	static const StealthTable defaults;
	const StealthTable& table = rules ? rules->stealth : defaults;
	int light = map ? int(map->GetLightLevel(Pos)) : 100;
	const LightBand& band = table.Band(light);
	if (label) *label = band.label;
	return band.dc;
}

int Actor::DCCFloorDC() const
{
	const RulesData* rules = Rules();
	const Map* map = GetCurrentArea();
	if (!rules || !map) return FLOOR_DC_DEFAULT;
	std::string area = map->GetScriptName().c_str();
	const std::map<std::string, int>* regions = rules->floors.Regions(area);
	if (regions) {
		for (const auto& ip : map->TMap->GetInfoPoints()) {
			std::string name = ip->GetScriptName().c_str();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			auto it = regions->find(name);
			if (it != regions->end() && ip->IsOver(Pos)) return it->second;
		}
	}
	return rules->floors.AreaDC(area);
}

bool Actor::DCCHidden() const
{
	return Modal.State == Modal::Stealth && dccStealth.hidden;
}

void Actor::DCCUpdateStealth(bool force)
{
	std::string hideLabel;
	int hideDC = DCCHideDC(&hideLabel);
	int floorDC = DCCFloorDC();
	// rolled on entering, and again on a new light or floor DC (classes.md); never opposed
	if (force || hideDC != dccStealth.hideDC) {
		dccStealth.hideDC = hideDC;
		ThiefCheckResult r = DCCThiefCheck(ThiefSkill::Hide, hideDC, hideLabel, Ability::Agi, SKILL_NO_LUCK);
		dccStealth.hidden = r.success;
	}
	if (force || floorDC != dccStealth.floorDC) {
		dccStealth.floorDC = floorDC;
		ThiefCheckResult r = DCCThiefCheck(ThiefSkill::Sneak, floorDC, FloorLabel(floorDC), Ability::Agi, SKILL_NO_LUCK);
		dccStealth.silent = r.success;
	}
	// something else made it visible (a spell cast at someone, a ForceVisible): Stealth ends
	bool applied = fxqueue.HasEffectWithSource(fx_dcc_invisible_ref, STEALTH_SOURCE);
	if (dccStealth.invisible && !applied) {
		DCCEndStealth(u"seen");
		return;
	}
	// unseen while it stands, or while it moves unheard
	bool want = dccStealth.hidden && (dccStealth.silent || !InMove());
	if (want && !applied) {
		auto fx = EffectQueue::CreateEffect(fx_dcc_invisible_ref, 0, 0, FX_DURATION_INSTANT_PERMANENT);
		fx->SourceRef = STEALTH_SOURCE;
		core->ApplyEffect(std::move(fx), this, this);
	} else if (!want && applied) {
		fxqueue.RemoveAllEffects(STEALTH_SOURCE);
	}
	dccStealth.invisible = want;
}

void Actor::DCCEndStealth(const char16_t* why)
{
	if (Modal.State != Modal::Stealth) return;
	DCCFeedback(fmt::format(u"{} is out of Stealth ({})", GetName(), why), FT_MISC);
	if (fxqueue.HasEffectWithSource(fx_dcc_invisible_ref, STEALTH_SOURCE)) fxqueue.RemoveAllEffects(STEALTH_SOURCE);
	dccStealth = {}; // no second line from SetModal
	SetModal(Modal::None, true);
}

// --- the modes, each tick -------------------------------------------------------------------------

bool Actor::DCCUpdateModal(ieDword gameTime, int roundFraction)
{
	if (Modal.State != Modal::DetectTraps && Modal.State != Modal::Stealth) return false;
	ieDword state = Modified[IE_STATE_ID];
	bool canAct = !Immobile() && !(state & (STATE_CONFUSED | STATE_DEAD | STATE_HELPLESS | STATE_PANIC | STATE_BERSERK | STATE_SLEEP)) && !IsDCCHeld();
	bool first = Modal.FirstApply;
	Modal.FirstApply = false;
	if (Modal.State == Modal::DetectTraps) {
		// a searcher walks at half speed and can't run (traps.md)
		InternalFlags &= ~IF_RUNNING;
		if (first) DCCFeedback(fmt::format(u"{} searches for traps (half speed)", GetName()), FT_MISC);
		// once a round, from the moment the search starts
		if ((first || roundFraction == 0) && canAct) {
			if (first) Modal.LastApplyTime = gameTime;
			DCCSearchTraps();
		}
		return true;
	}
	if (!canAct) {
		if (dccStealth.invisible) DCCEndStealth(u"can't keep hidden");
		return true;
	}
	if (first) {
		DCCFeedback(fmt::format(u"{} tries to stay hidden", GetName()), FT_MISC);
		dccStealth = {};
	}
	DCCUpdateStealth(first);
	return true;
}

// --- backstab and awareness -----------------------------------------------------------------------

bool Actor::DCCAwareOf(const Actor* target) const
{
	auto it = dccAwareness.find(target->GetGlobalID());
	return it != dccAwareness.end() && it->second.aware;
}

void Actor::DCCMarkAware(const Actor* target, const char* why)
{
	DCCAwareness& entry = dccAwareness[target->GetGlobalID()];
	if (!entry.aware) Log(DEBUG, "DCC", "{} is aware of {} ({})", fmt::WideToChar { target->GetName() }, fmt::WideToChar { GetName() }, why);
	entry.aware = true;
	entry.lostSince = 0;
}

bool Actor::DCCInFight(ieDword now) const
{
	ieDword window = 2 * core->Time.attack_round_size;
	return dccCombatTime && now <= dccCombatTime + window;
}

static bool InFight(const Actor* actor, ieDword now)
{
	return actor->DCCInFight(now);
}

Unaware Actor::DCCBackstabUnaware(const Actor* target, bool targetFighting) const
{
	bool hidden = DCCHidden() || (GetStat(IE_STATE_ID) & STATE_INVISIBLE);
	bool inSight = CanSee(target, this, true, GA_NO_DEAD);
	bool silent = Modal.State == Modal::Stealth && dccStealth.silent;
	return TargetUnaware(DCCAwareOf(target), hidden, inSight, silent, targetFighting, IsBehind(target));
}

// a creature that could notice the thief and fight it: not its own side (the party, or the same band of
// EA: good, neutral, evil)
static int Band(ieDword ea)
{
	if (ea <= EA_GOODCUTOFF) return 0;
	return ea >= EA_EVILCUTOFF ? 2 : 1;
}

static bool OtherSide(const Actor* thief, const Actor* other)
{
	if (thief->InParty && other->InParty) return false;
	return Band(thief->GetStat(IE_EA)) != Band(other->GetStat(IE_EA));
}

static bool Hostile(const Actor* thief, const Actor* other)
{
	ieDword a = thief->GetStat(IE_EA);
	ieDword b = other->GetStat(IE_EA);
	return (a <= EA_GOODCUTOFF && b >= EA_EVILCUTOFF) || (a >= EA_EVILCUTOFF && b <= EA_GOODCUTOFF);
}

void Actor::DCCUpdateAwareness(ieDword gameTime)
{
	if (!IsClass(this, ClassID::Thief) || !area) return;
	if (gameTime % core->Time.defaultTicksPerSec) return;
	int round = RoundTicks();
	bool threat = dccAttackedTime && gameTime < dccAttackedTime + ieDword(round);
	std::vector<Actor*> near = area->GetAllActorsInRadius(Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_SELF, GetVisualRange(), this);
	for (const Actor* other : near) {
		if (!OtherSide(this, other)) continue;
		bool fighting = InFight(other, gameTime);
		// it sees the thief: in sight (Stealth's hiding counts), unless calm with its back to it. A bystander
		// who isn't hostile and isn't fighting doesn't take note of a thief it sees; the attack decides then
		bool hostile = Hostile(this, other);
		bool sees = CanSee(other, this, true, GA_NO_DEAD | GA_NO_HIDDEN) && (fighting || !IsBehind(other));
		if (sees && (fighting || hostile)) DCCMarkAware(other, "saw it");
		bool foe = hostile || DCCAwareOf(other);
		if (!foe) continue;
		if (WithinPersonalRange(other, this, REACH_FEET)) threat = true;
		if (other->objects.LastTarget == GetGlobalID() && sees) threat = true;
	}
	// breaking away: nobody in reach or after it for a full round
	dccBreakAwaySince = threat ? 0 : (dccBreakAwaySince ? dccBreakAwaySince : gameTime);
	int brokenAway = dccBreakAwaySince ? int(gameTime - dccBreakAwaySince) : 0;
	for (auto it = dccAwareness.begin(); it != dccAwareness.end();) {
		Actor* target = area->GetActorByGlobalID(it->first);
		if (!target || (target->GetStat(IE_STATE_ID) & STATE_DEAD)) {
			it = dccAwareness.erase(it);
			continue;
		}
		DCCAwareness& entry = it->second;
		++it;
		if (!entry.aware) continue;
		// lost: it fights someone else, or can't see the thief
		bool fightsOther = InFight(target, gameTime) && target->objects.LastTarget && target->objects.LastTarget != GetGlobalID();
		bool lost = fightsOther || !CanSee(target, this, true, GA_NO_DEAD | GA_NO_HIDDEN);
		entry.lostSince = lost ? (entry.lostSince ? entry.lostSince : gameTime) : 0;
		int lostTicks = entry.lostSince ? int(gameTime - entry.lostSince) : 0;
		if (LosesTrack(brokenAway, DCCHidden(), lostTicks, round)) {
			entry.aware = false;
			entry.lostSince = 0;
			DCCFeedback(fmt::format(u"{} loses track of {}", target->GetName(), GetName()), FT_MISC);
		}
	}
}

// --- pick pocket ----------------------------------------------------------------------------------

int Actor::DCCPickPocketDC(const Actor* target) const
{
	bool hidden = DCCHidden() || (GetStat(IE_STATE_ID) & STATE_INVISIBLE);
	bool unaware = hidden || !CanSee(target, this, true, GA_NO_DEAD) || (target->GetStat(IE_STATE_ID) & (STATE_SLEEP | STATE_HELPLESS));
	return PickPocketDC(Signed(target->GetBase(IE_PICKPOCKET)), unaware, InFight(target, Now()));
}


bool Actor::DCCPickPocket(Actor* target, bool turnHostile)
{
	int dc = DCCPickPocketDC(target);
	if (dc < 0) {
		DCCFeedback(fmt::format(u"{} has nothing {} could lift", target->GetName(), GetName()), FT_MISC);
		return false;
	}
	std::string label = dc == PICKPOCKET_DC_UNAWARE ? "unaware" : (dc == PICKPOCKET_DC_WATCHFUL ? "watching" : "");
	ThiefCheckResult r = DCCThiefCheck(ThiefSkill::PickPocket, dc, label, Ability::Agi, 0, target->GetName());
	if (r.outcome == CheckOutcome::Success) return true;
	if (r.outcome == CheckOutcome::Miss) return false; // nothing taken, and nobody noticed
	// caught: out of hiding, the target turns on the thief (or its script calls the guard)
	DCCEndStealth(u"caught");
	CureInvisibility();
	target->AddTrigger(TriggerEntry(trigger_pickpocketfailed, GetGlobalID()));
	if (turnHostile) {
		if (target->GetStat(IE_EA) > EA_GOODCUTOFF && target->GetStat(IE_EA) < EA_EVILCUTOFF) target->SetBase(IE_EA, EA_ENEMY);
		target->AttackedBy(this);
	}
	DCCMarkAware(target, "caught it");
	return false;
}

void Actor::DCCLeaveModal(enum Modal oldState)
{
	if (Modal.State == oldState) return;
	if (oldState == Modal::Stealth) {
		if (fxqueue.HasEffectWithSource(fx_dcc_invisible_ref, STEALTH_SOURCE)) fxqueue.RemoveAllEffects(STEALTH_SOURCE);
		// DCCEndStealth said why already
		if (dccStealth.hideDC >= 0) DCCFeedback(fmt::format(u"{} leaves Stealth", GetName()), FT_MISC);
		dccStealth = {};
	} else if (oldState == Modal::DetectTraps) {
		DCCFeedback(fmt::format(u"{} stops searching", GetName()), FT_MISC);
	}
}


void Map::DCCForgetSlot(uint16_t bit)
{
	if (!bit) return;
	auto forget = [bit](Highlightable* h) {
		Highlightable::DCCRolls& r = h->dccRolls;
		if (!((r.found | r.disarm | r.lock) & bit)) return;
		r.found &= ~bit;
		r.disarm &= ~bit;
		r.lock &= ~bit;
		h->DCCRollsChanged();
	};
	for (const auto& door : TMap->GetDoors()) forget(door.get());
	for (const auto& container : TMap->GetContainers()) forget(container.get());
	for (const auto& ip : TMap->GetInfoPoints()) forget(ip.get());
}

}
