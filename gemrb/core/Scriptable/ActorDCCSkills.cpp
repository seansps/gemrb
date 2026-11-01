// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC occupation skill checks (M3.4) for actors: the tags a character has, how it rolls a check
// (trained d20, untrained d10, d10 + 2 with some familiarity, a thief's Table 1-9 bonus), the roll on
// the actor's dice, Luck by the burn policy or a prompt for a deadly check, the combat log line and a
// floating line over the roller, and the party member best placed for a check. The rules are pure
// functions in core/DCC/Skills.h; the script triggers and actions are in GameScript/DCCScript.cpp.
// See docs/architecture/rules-dcc.md, "Occupation skills (M3.4)".

#include "ie_stats.h"

#include "DisplayMessage.h"
#include "Game.h"
#include "Interface.h"
#include "Map.h"
#include "Prompts.h"

#include "DCC/Luck.h"
#include "DCC/Rules.h"
#include "DCC/Skills.h"
#include "DCC/Stats.h"
#include "GUI/GameControl.h"
#include "Logging/Logging.h"
#include "Net/NetSession.h"
#include "Scriptable/Actor.h"

#include <algorithm>

namespace GemRB {

using namespace DCC;

static SkillCheckResult lastSkillCheck;

static const RulesData* Rules()
{
	return core->GetDCCRules();
}

static int Signed(ieDword value)
{
	return static_cast<int>(static_cast<ieDwordSigned>(value));
}

static const SkillTable& Skills()
{
	static const SkillTable none;
	const RulesData* rules = Rules();
	return rules ? rules->skills : none;
}

static Ability CheckAbility(const SkillTags& tags, int code)
{
	if (code >= SKILL_ABILITY_STR && code <= SKILL_ABILITY_LUCK) return static_cast<Ability>(code - SKILL_ABILITY_STR);
	return Skills().DefaultAbility(tags.bits);
}

static const char16_t* AbilityShort(Ability ability)
{
	static const char16_t* names[] = { u"Str", u"Agi", u"Sta", u"Per", u"Int", u"Luck" };
	size_t i = static_cast<size_t>(ability);
	return i < static_cast<size_t>(ABILITY_COUNT) ? names[i] : u"?";
}

static String Widen(const std::string& text)
{
	String wide;
	for (unsigned char c : text) wide += char16_t(c);
	return wide;
}

// "Smithing", "Smithing/Mining", or the special tags'
static String CheckLabel(const std::string& key)
{
	SkillTags tags = ParseSkillTags(key);
	std::string label = Skills().Labels(tags.bits);
	std::replace(label.begin(), label.end(), ',', '/');
	label.erase(std::remove(label.begin(), label.end(), ' '), label.end());
	if (label.empty()) label = tags.untrainable ? "Untrainable" : "Everyday";
	return Widen(label);
}

uint32_t Actor::GetDCCSkills() const
{
	const Occupation& occupation = dccExt.occupation;
	if (occupation.skills) return occupation.skills;
	const RulesData* rules = Rules();
	const OccupationInfo* info = rules && occupation.id ? rules->occupations.Find(occupation.id) : nullptr;
	return info ? info->skills : 0;
}

bool Actor::DCCAddSkill(uint32_t bit)
{
	if (!IsDCCCharacter() || !bit || (bit & (bit - 1)) || !Skills().Find(bit)) return false;
	uint32_t have = GetDCCSkills();
	if (have & bit) return true;
	const RulesData* rules = Rules();
	const OccupationInfo* info = rules ? rules->occupations.Find(dccExt.occupation.id) : nullptr;
	uint32_t own = info ? info->skills : 0;
	if (SkillTagCount(have & ~own) >= ACQUIRED_SKILL_LIMIT) return false;
	dccExt.occupation.skills = have | bit;
	DCCExtChanged();
	DCCFeedback(fmt::format(u"{} learns {} (an acquired skill)", GetName(), Widen(Skills().Labels(bit))), FT_MISC);
	return true;
}

bool Actor::DCCCanTrySkill() const
{
	if (!IsDCCCharacter()) return false;
	if (GetStat(IE_STATE_ID) & STATE_CANTMOVE) return false;
	return !IsDCCHeld();
}

SkillPlan Actor::DCCSkillPlan(const SkillTags& tags, int ability, int dc, int situational) const
{
	const RulesData* rules = Rules();
	const SkillTable& table = Skills();
	SkillPlanInput in;
	in.known = GetDCCSkills();
	in.wanted = tags;
	in.dc = dc;
	in.situational = situational;
	in.abilityMod = GetDCCAbilityMod(CheckAbility(tags, ability));
	// born under the loom: the frozen Luck modifier on skill checks
	in.augur = GetDCCAugurBonus(AugurRoll::SkillCheck);
	in.thiefSkill = SkillThiefOverlap(table, tags.bits);
	int level = Signed(BaseStats[IE_LEVEL]);
	if (rules && in.thiefSkill >= 0 && level >= 1 && BaseStats[IE_CLASS] == static_cast<ieDword>(ClassID::Thief)) {
		in.thief = true;
		in.thiefBonus = rules->thiefSkills.Bonus(AlignmentFromIE(GetStat(IE_ALIGNMENT)), level, static_cast<ThiefSkill>(in.thiefSkill));
	}
	return PlanSkillCheck(table, in);
}

// "blacksmith", "ex-blacksmith" from level 1
static String OccupationWords(const Actor* actor)
{
	const RulesData* rules = Rules();
	const OccupationInfo* info = rules ? rules->occupations.Find(actor->dccExt.occupation.id) : nullptr;
	if (!info || info->name.empty()) return String();
	std::string name = info->name;
	name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
	return Widen(actor->GetBase(IE_LEVEL) >= 1 ? "ex-" + name : name);
}

static String TrainingWords(const SkillPlan& plan)
{
	switch (plan.training) {
		case SkillTraining::Trained:
			return u"trained d20";
		case SkillTraining::Anyone:
			return u"anyone's d20";
		case SkillTraining::Thief:
			return u"thief d20";
		case SkillTraining::Familiar:
			return plan.capped ? u"past its training, d10+2" : u"familiar d10+2";
		default:
			return u"untrained d10";
	}
}

static void Part(String& line, int value, const char16_t* what)
{
	if (value) line += fmt::format(u" {:+d} {}", value, what);
}

// "Brin (blacksmith, trained d20) Smithing check: 11 +1 Str = 12 vs DC 12: success"
static String SkillLine(const Actor* actor, const SkillCheckResult& r)
{
	String who = actor->GetName();
	String occupation = OccupationWords(actor);
	String training = TrainingWords(r.plan);
	String head = occupation.empty() ? fmt::format(u"{} ({})", who, training) : fmt::format(u"{} ({}, {})", who, occupation, training);
	String line = fmt::format(u"{} {} check{}: {}", head, CheckLabel(r.tags), r.party ? u" for the party" : u"", r.natural);
	Part(line, r.plan.abilityMod, AbilityShort(r.ability));
	Part(line, r.plan.familiarity, u"familiar");
	Part(line, r.plan.thiefBonus, u"thief");
	Part(line, r.plan.augur, u"augur");
	Part(line, r.plan.situational, u"situation");
	Part(line, r.rollMod, u"groggy");
	Part(line, r.luckBonus, u"Luck");
	line += fmt::format(u" = {} vs DC {}: {}", r.total, r.dc, r.success ? u"success" : u"failure");
	if (r.cursed) line += u" (cursed by fortune)";
	return line;
}

// the log (shown in a conversation too, unlike combat lines: the check belongs to it) and a line over the roller
static void ReportSkillCheck(Actor* actor, const SkillCheckResult& result)
{
	lastSkillCheck = result;
	String line = SkillLine(actor, result);
	Log(COMBAT, "DCC", "{}", fmt::WideToChar { line });
	Actor::DCCRecordLine(line);
	// co-op: a dialog's check is rolled at a reply's pick, outside the host's simulation window; what it
	// shows still goes to everyone
	NetSession* net = core->GetNetSession();
	bool captured = net ? net->BeginCapture() : false;
	if (core->HasFeedback(FT_MISC)) displaymsg->DisplayString(line);
	if (actor->GetCurrentArea()) {
		static const Color passed(0xa0, 0xff, 0xa0, 0xff);
		static const Color failed(0xff, 0xa0, 0x90, 0xff);
		actor->overHead.SetText(fmt::format(u"{} {}: {}", CheckLabel(result.tags), result.dc, result.success ? u"success" : u"failure"), true, true,
					result.success ? passed : failed);
	}
	if (net) net->EndCapture(captured);
}

// a deadly check that failed: a party member who pauses on key rolls is asked how much Luck to burn
static bool AskSkillBurn(Actor* actor, const SkillCheckResult& result, const Actor::DCCSkillDone& done)
{
	if (!actor->InParty || !actor->DCCPausesOnKeyRolls()) return false;
	const NetSession* net = core->GetNetSession();
	if ((net && !net->IsHost()) || !core->GetGameControl()) return false;
	int luck = actor->GetDCCLuck();
	int gap = result.dc - result.total;
	BurnValue value = actor->GetDCCBurnValue();
	if (luck <= 0 || gap <= 0 || MaxBurnBonus(value, luck) < gap) return false;

	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "luckburn";
	q.title = "Burn Luck?";
	std::string name = fmt::format("{}", fmt::WideToChar { actor->GetShortName() });
	std::string label = fmt::format("{}", fmt::WideToChar { CheckLabel(result.tags) });
	std::string how = value.IsDie() ? fmt::format("a d{} each", value.luckDie) : fmt::format("+{} each", value.perPoint);
	q.text = fmt::format("{} fails a deadly {} check by {} ({} vs DC {}). Burn Luck ({}, {} left)?", name, label, gap, result.total, result.dc, how, luck);
	q.fields = { { "Luck", 0, luck, PolicyBurn(actor->GetDCCLuckPolicy(), BurnReason::Death, gap, luck, value) } };
	q.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, LUCK_PROMPT_TIMEOUT));

	ieDword selfID = actor->GetGlobalID();
	auto pending = std::make_shared<SkillCheckResult>(result);
	uint32_t id = core->GetPrompts().Ask(actor, std::move(q), [selfID, pending, done](const PromptResult& r) {
		Game* game = core->GetGame();
		Actor* self = game ? game->GetActorByGlobalID(selfID) : nullptr;
		SkillCheckResult final = *pending;
		final.waiting = false;
		if (self) {
			int points = Clamp<int>(r.Value(), 0, self->GetDCCLuck());
			if (points > 0) {
				BurnValue burnValue = self->GetDCCBurnValue();
				int bonus = self->DCCBurnLuck(points, burnValue);
				final.luckBurned = points;
				final.luckBonus = bonus;
				final.total += bonus;
				final.success = final.total >= final.dc;
				String burned = burnValue.IsDie() ? fmt::format(u"{}d{} = +{}", points, burnValue.luckDie, bonus) : fmt::format(u"+{}", bonus);
				Actor::DCCFeedback(fmt::format(u"{} burns {} Luck on a {} check ({}): {} -> {} vs DC {}, {} (Luck {} left)", self->GetName(), points,
							       CheckLabel(final.tags), burned, pending->total, final.total, final.dc,
							       final.success ? u"success" : u"still a failure", self->GetDCCLuck()),
						   FT_MISC);
			}
			ReportSkillCheck(self, final);
		} else {
			lastSkillCheck = final;
		}
		if (done) done(final);
	});
	if (!id) return false;
	Actor::DCCFeedback(fmt::format(u"{} fails a deadly {} check: {} vs DC {}; burn Luck?", actor->GetName(), CheckLabel(result.tags), result.total, result.dc),
			   FT_MISC);
	return true;
}

SkillCheckResult Actor::DCCSkillCheck(const SkillTags& tags, int ability, int dc, uint32_t flags, int situational, int natural, bool ask,
				      const DCCSkillDone& done)
{
	SkillCheckResult result;
	result.roller = GetGlobalID();
	result.tags = SkillTagsKey(tags);
	result.ability = CheckAbility(tags, ability);
	result.dc = dc;
	result.party = flags & SKILL_PARTY;
	result.plan = DCCSkillPlan(tags, ability, dc, situational);
	// on the actor's dice: the engine's RNG (seeded in the autotests), or forced ones (DCCForceDice)
	result.natural = natural > 0 ? std::min(natural, result.plan.die) : DCCDie(result.plan.die);
	RollResult roll = DCCRoll(RollKind::Check, result.plan.Modifier(), dc, result.natural, result.plan.die);
	result.rollMod = Signed(GetStat(IE_DCC_ROLLMOD));
	result.cursed = roll.cursed;
	result.total = roll.total;
	result.success = roll.success;

	bool deadly = flags & SKILL_DEADLY;
	if (!roll.success && !roll.cursed && !(flags & SKILL_NO_LUCK) && IsDCCCharacter()) {
		if (ask && deadly && AskSkillBurn(this, result, done)) {
			result.waiting = true;
			lastSkillCheck = result;
			return result;
		}
		// the policy (or the party's halfling) by itself
		String what = fmt::format(u"a {} check", CheckLabel(result.tags));
		int before = roll.total;
		result.luckBurned = DCCPolicyRollBurn(roll, deadly ? BurnReason::Death : BurnReason::Miss, what.c_str());
		result.luckBonus = roll.total - before;
		result.total = roll.total;
		result.success = roll.success;
	}
	ReportSkillCheck(this, result);
	if (done) done(result);
	return result;
}

// a candidate's plan as it really rolls: groggy counts, and a cursed one never succeeds
static SkillPlan Effective(const Actor* actor, SkillPlan plan)
{
	plan.situational += Signed(actor->GetStat(IE_DCC_ROLLMOD));
	if (actor->GetDCCStates() & STATE_CURSED) plan.situational -= 1000;
	return plan;
}

Actor* Actor::DCCBestSkillRoller(const Map* area, const SkillTags& tags, int ability, int dc, int situational)
{
	const Game* game = core->GetGame();
	if (!game) return nullptr;
	Actor* best = nullptr;
	SkillPlan bestPlan;
	for (int i = 0; i < game->GetPartySize(false); ++i) {
		Actor* pc = game->GetPC(i, false);
		if (!pc || !pc->DCCCanTrySkill()) continue;
		if (area && pc->GetCurrentArea() != area) continue;
		SkillPlan plan = Effective(pc, pc->DCCSkillPlan(tags, ability, dc, situational));
		if (!best || BetterSkillPlan(plan, bestPlan, dc)) {
			best = pc;
			bestPlan = plan;
		}
	}
	return best;
}

const SkillCheckResult& Actor::DCCLastSkillCheck()
{
	return lastSkillCheck;
}

}
