// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC script triggers and actions (DCCRules = 1; a game's trigger.ids and action.ids list them):
// occupation skill checks (M3.4) for area scripts (.baf) and dialogs (.d). See
// docs/architecture/rules-dcc.md, "Occupation skills (M3.4)".
//
// Triggers (true on success; the dialog form, a state's continue transitions branch on them):
//   DCCSkillCheck(O:Object*,S:Tags*,I:Ability*DCCAbil,I:DC*)   that character rolls
//   DCCPartySkillCheck(S:Tags*,I:Ability*DCCAbil,I:DC*)        the party member best placed rolls
//   DCCHasSkill(O:Object*,S:Tags*)                            trained in any of the tags
//   DCCPartyHasSkill(S:Tags*)                                 someone here is
// Actions (the world form: the result goes into a variable, 1 success, 2 failure, 3 waiting for a
// Luck answer; "NAME" is a GLOBAL, "LOCALS:NAME", "MYAREA:NAME" and "GLOBAL:NAME" name the scope):
//   DCCSkillCheck(O:Object*,S:Tags*,I:Ability*DCCAbil,I:DC*,I:Flags*DCCSkFl,S:Variable*)
//   DCCPartySkillCheck(S:Tags*,I:Ability*DCCAbil,I:DC*,I:Flags*DCCSkFl,S:Variable*)
//   DCCAddSkill(O:Object*,S:Tag*)                             an acquired tag (at most two)
// The token <DCCSKILLER> names who rolled the last check, for the dialog's next line.

#include "Game.h"
#include "Interface.h"
#include "Map.h"

#include "DCC/Skills.h"
#include "GameScript/GSUtils.h"
#include "GameScript/GameScript.h"
#include "GameScript/Matching.h"
#include "Logging/Logging.h"
#include "Scriptable/Actor.h"

#include <map>

namespace GemRB {

using namespace DCC;

static bool DCCScripts()
{
	return core->HasFeature(GFFlags::RULES_DCC) && core->GetDCCRules();
}

static SkillTags Tags(const StringParam& text, const char* who)
{
	SkillTags tags = ParseSkillTags(text.c_str());
	if (!tags.valid) Log(ERROR, "GameScript", "{}: unknown skill tags \"{}\" (dccskill.2da's, ANYONE or UNTRAINABLE)", who, text);
	return tags;
}

static void SetSkiller(const Actor* roller)
{
	core->GetTokenDictionary()["DCCSKILLER"] = roller ? roller->GetName() : String();
}

// A trigger's check is rolled once while the game clock stands still: a dialog evaluates a state's
// continue transitions more than once, and a conversation freezes the clock, so a check in it can't
// be retried by coming back to the same state (funnel.md: no retry by clicking again). One script
// pass also keeps its result. Actions always roll.
struct SkillMemo {
	ieDword time = 0;
	SkillCheckResult result;
};
static std::map<std::string, SkillMemo> skillMemo;

static const SkillCheckResult* Remembered(const std::string& key)
{
	const Game* game = core->GetGame();
	auto it = skillMemo.find(key);
	if (!game || it == skillMemo.end() || it->second.time != game->GameTime) return nullptr;
	return &it->second.result;
}

static void Remember(const std::string& key, const SkillCheckResult& result)
{
	const Game* game = core->GetGame();
	if (!game) return;
	for (auto it = skillMemo.begin(); it != skillMemo.end();) {
		it = it->second.time != game->GameTime ? skillMemo.erase(it) : std::next(it);
	}
	skillMemo[key] = { game->GameTime, result };
}

static std::string AreaKey(const Scriptable* sender)
{
	const Map* area = sender ? sender->GetCurrentArea() : nullptr;
	return area ? std::string(area->GetScriptRef().c_str()) : std::string();
}

int GameScript::DCCSkillCheck(Scriptable* Sender, const Trigger* parameters)
{
	if (!DCCScripts()) return 0;
	Actor* actor = Scriptable::As<Actor>(GetScriptableFromObject(Sender, parameters));
	SkillTags tags = Tags(parameters->string0Parameter, "DCCSkillCheck");
	if (!actor || !tags.valid || !actor->DCCCanTrySkill()) return 0;
	std::string key = fmt::format("{}:{}:{}:{}", actor->GetGlobalID(), SkillTagsKey(tags), parameters->int0Parameter, parameters->int1Parameter);
	if (const SkillCheckResult* known = Remembered(key)) return known->success;
	SkillCheckResult result = actor->DCCSkillCheck(tags, parameters->int0Parameter, parameters->int1Parameter);
	Remember(key, result);
	SetSkiller(actor);
	return result.success;
}

int GameScript::DCCPartySkillCheck(Scriptable* Sender, const Trigger* parameters)
{
	if (!DCCScripts()) return 0;
	SkillTags tags = Tags(parameters->string0Parameter, "DCCPartySkillCheck");
	if (!tags.valid) return 0;
	std::string key = fmt::format("party@{}:{}:{}:{}", AreaKey(Sender), SkillTagsKey(tags), parameters->int0Parameter, parameters->int1Parameter);
	if (const SkillCheckResult* known = Remembered(key)) return known->success;
	Actor* roller = Actor::DCCBestSkillRoller(Sender ? Sender->GetCurrentArea() : nullptr, tags, parameters->int0Parameter, parameters->int1Parameter);
	SkillCheckResult result;
	if (roller) {
		result = roller->DCCSkillCheck(tags, parameters->int0Parameter, parameters->int1Parameter, SKILL_PARTY);
	} else {
		Actor::DCCFeedback(u"Nobody in the party here can try that.", FT_MISC);
	}
	Remember(key, result);
	SetSkiller(roller);
	return result.success;
}

static bool HasSkill(const Actor* actor, const SkillTags& tags)
{
	if (!actor || !actor->IsDCCCharacter()) return false;
	SkillTraining training = actor->DCCSkillPlan(tags, SKILL_ABILITY_DEFAULT, 0).training;
	return training == SkillTraining::Trained || training == SkillTraining::Anyone || training == SkillTraining::Thief;
}

int GameScript::DCCHasSkill(Scriptable* Sender, const Trigger* parameters)
{
	if (!DCCScripts()) return 0;
	SkillTags tags = Tags(parameters->string0Parameter, "DCCHasSkill");
	return tags.valid && HasSkill(Scriptable::As<Actor>(GetScriptableFromObject(Sender, parameters)), tags);
}

int GameScript::DCCPartyHasSkill(Scriptable* Sender, const Trigger* parameters)
{
	if (!DCCScripts()) return 0;
	SkillTags tags = Tags(parameters->string0Parameter, "DCCPartyHasSkill");
	const Game* game = core->GetGame();
	if (!tags.valid || !game) return 0;
	const Map* area = Sender ? Sender->GetCurrentArea() : nullptr;
	for (int i = 0; i < game->GetPartySize(false); ++i) {
		const Actor* pc = game->GetPC(i, false);
		if (!pc || !pc->DCCCanTrySkill() || (area && pc->GetCurrentArea() != area)) continue;
		if (HasSkill(pc, tags)) return 1;
	}
	return 0;
}

// "NAME" is a GLOBAL; "LOCALS:NAME", "MYAREA:NAME", "GLOBAL:NAME" (or an area's six letters) name the scope
static std::string SkillVariable(const StringParam& text)
{
	std::string name = text.c_str();
	if (name.empty()) return name;
	size_t colon = name.find(':');
	if (colon == 6) return name;
	if (colon != std::string::npos) {
		Log(WARNING, "GameScript", "Skill check variable {}: the scope has six letters (LOCALS, MYAREA, GLOBAL); using GLOBAL", name);
		name = name.substr(colon + 1);
	}
	return "GLOBAL" + name;
}

// where the result goes once it is known (at once, or when a Luck prompt is answered)
static Actor::DCCSkillDone SetResult(Scriptable* Sender, const std::string& variable)
{
	ieDword senderID = Sender->GetGlobalID();
	ResRef area = Sender->GetCurrentArea() ? Sender->GetCurrentArea()->GetScriptRef() : ResRef();
	return [senderID, area, variable](const SkillCheckResult& result) {
		Game* game = core->GetGame();
		if (!game) return;
		const Actor* roller = result.roller ? game->GetActorByGlobalID(result.roller) : nullptr;
		SetSkiller(roller);
		if (variable.empty()) return;
		// the script's owner, for LOCALS and MYAREA; a GLOBAL needs none
		int index = area.IsEmpty() ? -1 : game->FindMap(area);
		Map* map = index >= 0 ? game->GetMap(unsigned(index)) : nullptr;
		Scriptable* owner = map ? map->GetScriptableByGlobalID(senderID) : nullptr;
		if (!owner) owner = map ? static_cast<Scriptable*>(map) : game->GetCurrentArea();
		if (!owner) return;
		SetVariable(owner, variable, result.success ? SKILL_OUTCOME_PASSED : SKILL_OUTCOME_FAILED);
	};
}

void GameScript::DCCSkillCheck(Scriptable* Sender, Holder<Action> parameters)
{
	if (!DCCScripts()) return;
	std::string variable = SkillVariable(parameters->string1Parameter);
	Actor* actor = Scriptable::As<Actor>(GetScriptableFromObject(Sender, parameters));
	SkillTags tags = Tags(parameters->string0Parameter, "DCCSkillCheck");
	if (!actor || !tags.valid || !actor->DCCCanTrySkill()) {
		if (!variable.empty()) SetVariable(Sender, variable, SKILL_OUTCOME_FAILED);
		return;
	}
	if (!variable.empty()) SetVariable(Sender, variable, SKILL_OUTCOME_WAITING);
	uint32_t flags = uint32_t(parameters->int2Parameter) & (SKILL_DEADLY | SKILL_NO_LUCK);
	actor->DCCSkillCheck(tags, parameters->int0Parameter, parameters->int1Parameter, flags, 0, 0, true, SetResult(Sender, variable));
}

void GameScript::DCCPartySkillCheck(Scriptable* Sender, Holder<Action> parameters)
{
	if (!DCCScripts()) return;
	std::string variable = SkillVariable(parameters->string1Parameter);
	SkillTags tags = Tags(parameters->string0Parameter, "DCCPartySkillCheck");
	Actor* roller = tags.valid ? Actor::DCCBestSkillRoller(Sender->GetCurrentArea(), tags, parameters->int0Parameter, parameters->int1Parameter) : nullptr;
	if (!roller) {
		if (tags.valid) Actor::DCCFeedback(u"Nobody in the party here can try that.", FT_MISC);
		SetSkiller(nullptr);
		if (!variable.empty()) SetVariable(Sender, variable, SKILL_OUTCOME_FAILED);
		return;
	}
	if (!variable.empty()) SetVariable(Sender, variable, SKILL_OUTCOME_WAITING);
	uint32_t flags = (uint32_t(parameters->int2Parameter) & (SKILL_DEADLY | SKILL_NO_LUCK)) | SKILL_PARTY;
	roller->DCCSkillCheck(tags, parameters->int0Parameter, parameters->int1Parameter, flags, 0, 0, true, SetResult(Sender, variable));
}

void GameScript::DCCAddSkill(Scriptable* Sender, Holder<Action> parameters)
{
	if (!DCCScripts()) return;
	Actor* actor = Scriptable::As<Actor>(GetScriptableFromObject(Sender, parameters));
	SkillTags tags = Tags(parameters->string0Parameter, "DCCAddSkill");
	if (!actor || !tags.valid || SkillTagCount(tags.bits) != 1) return;
	if (!actor->DCCAddSkill(tags.bits)) {
		Log(WARNING, "GameScript", "DCCAddSkill: {} can't learn {} (two acquired skills already?)", fmt::WideToChar { actor->GetName() }, SkillTagsKey(tags));
	}
}

}
