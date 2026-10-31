// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC Luck burn and Mighty Deeds settings (DCCRules = 1) for actors: the burn policy, what a
// point of Luck is worth, burning it, the halfling's good luck charm, burns on saves and
// checks, and the thief's and halfling's nightly recovery. The attack path (prompts that
// suspend an attack, deeds) is in ActorDCCCombat.cpp; the rules are pure functions in
// core/DCC/Luck.h and Deeds.h. See docs/architecture/rules-dcc.md, "Luck burn and Mighty
// Deeds (M2.4)".

#include "ie_stats.h"

#include "Game.h"
#include "Interface.h"
#include "Map.h"

#include "DCC/Classes.h"
#include "DCC/Luck.h"
#include "DCC/Rules.h"
#include "DCC/Stats.h"
#include "Scriptable/Actor.h"

namespace GemRB {

using namespace DCC;

static int Signed(ieDword value)
{
	return static_cast<int>(static_cast<ieDwordSigned>(value));
}

LuckPolicy Actor::GetDCCLuckPolicy() const
{
	LuckPolicy policy;
	policy.policy = static_cast<BurnPolicy>(std::min<ieDword>(BaseStats[IE_DCC_BURNPOLICY], BURN_POLICY_COUNT - 1));
	policy.threshold = Signed(BaseStats[IE_DCC_BURNMIN]);
	policy.flags = BaseStats[IE_DCC_LUCKFLAGS];
	return policy;
}

bool Actor::DCCPausesOnKeyRolls() const
{
	if (!(BaseStats[IE_DCC_LUCKFLAGS] & LUCK_PAUSE)) return false;
	if (BaseStats[IE_LEVEL] > 0 && BaseStats[IE_CLASS] != static_cast<ieDword>(ClassID::Level0)) return true;
	// the funnel's peasants follow their policy without asking, unless the party settings say so
	const Game* game = core->GetGame();
	return game && game->GetGlobal("DCC_PROMPT_LOUD0", 0);
}

BurnValue Actor::GetDCCBurnValue() const
{
	return BurnValueFor(Signed(BaseStats[IE_CLASS]), Signed(BaseStats[IE_LEVEL]), Signed(GetStat(IE_DCC_LUCKDIE)));
}

int Actor::GetDCCLuck() const
{
	return std::max(0, Signed(GetStat(IE_DCC_LUCK)));
}

int Actor::DCCBurnLuck(int points, const BurnValue& value)
{
	points = std::min(points, GetDCCLuck());
	if (points <= 0) return 0;
	// burned Luck is Luck loss: rest never heals it (thieves and halflings recover it, DCCRecoverLuck);
	// the current score drops at once, the effects refresh re-derives it the same way
	SetBase(IE_DCC_LOSS_LCK, BaseStats[IE_DCC_LOSS_LCK] + points);
	Modified[IE_DCC_LUCK] = std::max(0, Signed(Modified[IE_DCC_LUCK]) - points);
	return BurnBonus(value, points, [this](int sides) { return DCCDie(sides); });
}

static bool CanCharm(const Actor* actor)
{
	if (!actor->IsDCCCharacter() || actor->GetBase(IE_LEVEL) < 1) return false;
	if (actor->GetBase(IE_CLASS) != static_cast<ieDword>(ClassID::Halfling)) return false;
	if (actor->GetStat(IE_STATE_ID) & STATE_DEAD) return false;
	return !actor->IsDCCHeld() && actor->GetDCCLuck() > 1;
}

Actor* Actor::DCCLuckCharm()
{
	const Game* game = core->GetGame();
	const Map* map = GetCurrentArea();
	if (!game || !map) return nullptr;

	// only one halfling per party is the good luck charm (luck.md): the first in party order;
	// creatures outside the party use the nearest one on their side that isn't in the party
	Actor* charm = nullptr;
	if (InParty) {
		for (int i = 0; i < game->GetPartySize(false); ++i) {
			Actor* pc = game->GetPC(i, false);
			if (pc && pc->IsDCCCharacter() && pc->GetBase(IE_CLASS) == static_cast<ieDword>(ClassID::Halfling) && pc->GetBase(IE_LEVEL) >= 1) {
				charm = pc;
				break;
			}
		}
	} else {
		bool good = GetStat(IE_EA) <= EA_GOODCUTOFF;
		for (Actor* other : map->GetAllActorsInRadius(Pos, GA_NO_DEAD | GA_NO_UNSCHEDULED | GA_NO_LOS, GetVisualRange())) {
			if (other == this || other->InParty || (other->GetStat(IE_EA) <= EA_GOODCUTOFF) != good) continue;
			if (!CanCharm(other)) continue;
			if (!charm || SquaredDistance(other->Pos, Pos) < SquaredDistance(charm->Pos, Pos)) charm = other;
		}
	}
	// a halfling's own rolls already get its double value
	if (!charm || charm == this || !CanCharm(charm)) return nullptr;
	// nearby and visible to the halfling
	if (charm->GetCurrentArea() != map || (charm->GetStat(IE_STATE_ID) & STATE_BLIND)) return nullptr;
	if (!WithinRange(charm, Pos, charm->GetVisualRange()) || !map->IsVisibleLOS(charm->Pos, Pos)) return nullptr;
	return charm;
}

int Actor::DCCPolicyRollBurn(RollResult& roll, BurnReason reason, const char16_t* what)
{
	if (!IsDCCCharacter() || roll.success || roll.cursed) return 0;
	int gap = roll.target - roll.total;
	if (gap <= 0) return 0;
	BurnValue value = GetDCCBurnValue();
	int points = PolicyBurn(GetDCCLuckPolicy(), reason, gap, GetDCCLuck(), value);
	Actor* by = this;
	if (points <= 0) {
		// the halfling's good luck charm, by its own policy
		Actor* charm = DCCLuckCharm();
		if (!charm || !(charm->GetDCCLuckPolicy().flags & LUCK_ALLIES)) return 0;
		value = charm->GetDCCBurnValue();
		points = PolicyBurn(charm->GetDCCLuckPolicy(), reason, gap, charm->GetDCCLuck(), value);
		if (points <= 0) return 0;
		by = charm;
	}
	int before = roll.total;
	int bonus = by->DCCBurnLuck(points, value);
	roll.total += bonus;
	roll.success = roll.total >= roll.target;
	String how = value.IsDie() ? fmt::format(u"{}d{} = +{}", points, value.luckDie, bonus) : fmt::format(u"+{}", bonus);
	String forWhom = by == this ? String() : fmt::format(u" for {}", GetName());
	DCCFeedback(fmt::format(u"{} burns {} Luck{} on {} ({}): {} -> {} vs {}, {} (Luck {} left)", by->GetName(), points, forWhom, what, how, before, roll.total, roll.target,
				roll.success ? u"success" : u"still a failure", by->GetDCCLuck()),
		    FT_COMBAT);
	return points;
}

void Actor::DCCRecoverLuck()
{
	int loss = Signed(BaseStats[IE_DCC_LOSS_LCK]);
	if (loss <= 0) return;
	int left = LuckLossAfterNight(Signed(BaseStats[IE_CLASS]), Signed(BaseStats[IE_LEVEL]), loss);
	if (left == loss) return;
	SetBase(IE_DCC_LOSS_LCK, left);
	DCCFeedback(fmt::format(u"{} gets {} Luck back overnight", GetName(), loss - left), FT_MISC);
}

Deed Actor::GetDCCSignatureDeed() const
{
	return static_cast<Deed>(std::min<ieDword>(BaseStats[IE_DCC_DEED], DEED_COUNT - 1));
}

Deed Actor::GetDCCQueuedDeed() const
{
	return static_cast<Deed>(std::min<ieDword>(BaseStats[IE_DCC_NEXTDEED], DEED_COUNT - 1));
}

}
