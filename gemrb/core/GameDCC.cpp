// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// DCC games (M3.3): the end of the level-0 funnel and the reserves. The party of sixteen becomes
// the four the players chose; the other survivors wait off the map as reserve NPCs (Game::NPCs,
// in the GAM's NPC section, their area DCC::RESERVE_AREA) and the dead leave the party, their
// graves kept in Game::dccRecord. After the funnel the reserves join only at inns and camps (the GUI's
// Reserves screen), into a free place of the party of four: a place a hero left (buried or benched),
// kept with the co-op player it belongs to in dccRecord.places. A benched hero keeps everything and
// earns no XP (Actor::AddExperience). Kept apart from Game.cpp so the stock code stays as it is.

#include "Game.h"

#include "ie_stats.h"

#include "DisplayMessage.h"
#include "Interface.h"
#include "Logging/Logging.h"
#include "Map.h"

#include "DCC/Funnel.h"
#include "DCC/Rules.h"
#include "Scriptable/Actor.h"

#include <algorithm>

namespace GemRB {

static bool IsDead(const Actor* actor)
{
	return actor->GetStat(IE_STATE_ID) & STATE_DEAD;
}

int Game::DCCEndFunnel(const std::vector<Actor*>& chosen)
{
	// nobody left (everyone died) is the GUI's game over, not a party of none
	if (!core->HasFeature(GFFlags::RULES_DCC) || chosen.empty() || chosen.size() > size_t(DCC::FUNNEL_PARTY_SIZE)) return -1;
	for (size_t i = 0; i < chosen.size(); ++i) {
		const Actor* pick = chosen[i];
		if (!pick || InParty(pick) < 0 || IsDead(pick)) return -1;
		if (std::find(chosen.begin(), chosen.begin() + long(i), pick) != chosen.begin() + long(i)) return -1;
	}

	// every survivor has made it (decided 2026-10-31): the XP for level 1, through the normal XP path,
	// and its class pick pending (funnel.md "Advancement from level 0"); the chosen pick it now, the
	// reserves when they join the party
	const DCC::RulesData* rules = core->GetDCCRules();
	int levelOne = rules ? rules->xp.XPForLevel(1) : 10;
	for (Actor* pc : PCs) {
		if (IsDead(pc)) continue;
		int missing = levelOne - int(pc->GetBase(IE_XP));
		if (missing > 0) {
			pc->AddExperience(missing, 0);
			displaymsg->DisplayStringName(fmt::format(u"gains {} XP for surviving the funnel: ready for level 1", missing), GUIColors::XPCHANGE, pc);
		}
		if (!pc->GetBase(IE_LEVEL)) pc->dccExt.funnel.flags |= DCC::FUNNEL_CLASS_PENDING;
		pc->DCCExtChanged();
	}
	// the four (or fewer) go on
	for (Actor* pick : chosen) {
		pick->dccExt.funnel.status = DCC::FunnelStatus::Chosen;
		pick->DCCExtChanged();
	}
	// everyone else leaves the party: the living to the reserves, the dead to their graves
	std::vector<Actor*> others;
	for (Actor* pc : PCs) {
		if (std::find(chosen.begin(), chosen.end(), pc) == chosen.end()) others.push_back(pc);
	}
	for (Actor* pc : others) {
		DCCDismiss(pc);
	}
	// the chosen in the order they were picked
	PCs = chosen;
	for (size_t i = 0; i < PCs.size(); ++i) {
		PCs[i]->InParty = ieByte(i + 1);
	}
	selected.clear();
	for (Actor* pc : PCs) {
		pc->Selected = 0;
		SelectActor(pc, true, SELECT_NORMAL | SELECT_QUIET);
	}
	SelectPCSingle(1);
	// a party of four from now on, also after a load (the ethren GUI's EnterGame reads the global)
	SetPartySize(DCC::FUNNEL_PARTY_SIZE);
	locals[ieVariable("DCC_PARTY_SIZE")] = DCC::FUNNEL_PARTY_SIZE;
	// fewer than four went on: the places left are free, the host's
	dccRecord.places.assign(DCC::FUNNEL_PARTY_SIZE - PCs.size(), DCC::FUNNEL_NO_PLAYER);
	core->SetEventFlag(EF_PORTRAIT);
	Log(MESSAGE, "DCC", "The funnel is over: {} go on, {} leave the party; {} in the graveyard.", PCs.size(), others.size(), dccRecord.graves.size());
	return 0;
}

int Game::DCCDismiss(Actor* pc)
{
	if (!pc || InParty(pc) < 0) return -1;
	bool dead = IsDead(pc);
	// its place is free from now on, its player's
	std::vector<uint8_t> places = DCCFreePlaces();
	Map* map = pc->GetCurrentArea();
	Point where = pc->Pos;
	pc->ClearActions();
	pc->Stop();
	LeaveParty(pc, false);
	pc->InParty = 0;
	DCC::Funnel& funnel = pc->dccExt.funnel;
	if (dead) {
		// the body stays where it fell, the area's from now on; its things go on the ground there
		// (the funnel's wealth by attrition); its grave is in the game's record since the death
		if (map) pc->inventory.DropItemAtLocation(ResRef(), 0, map, where);
		int npc = InStore(pc);
		if (npc >= 0) DelNPC(unsigned(npc), false);
		Log(MESSAGE, "DCC", "{} is buried in {}.", fmt::WideToChar { pc->GetShortName() }, map ? map->GetScriptRef().c_str() : "no area");
	} else {
		// benched: to the reserves, off the map, waiting in the game's NPC list (and its saves) with
		// its level, XP, class, gear and records; it earns no XP there (AddExperience)
		funnel.status = DCC::FunnelStatus::Reserve;
		if (map) map->RemoveActor(pc);
		pc->AreaName = ResRef(DCC::RESERVE_AREA);
		Log(MESSAGE, "DCC", "{} goes to the reserves (level {}, {} XP).", fmt::WideToChar { pc->GetShortName() }, pc->GetBase(IE_LEVEL), pc->GetBase(IE_XP));
	}
	pc->DCCExtChanged();
	places.push_back(funnel.player);
	dccRecord.places = places;
	return 0;
}

int Game::DCCRecruit(Actor* reserve, int place)
{
	if (!reserve || InStore(reserve) < 0 || InParty(reserve) >= 0) return -1;
	if (reserve->dccExt.funnel.status != DCC::FunnelStatus::Reserve || IsDead(reserve)) return -1;
	std::vector<uint8_t> places = DCCFreePlaces();
	if (place < 0) place = 0;
	if (size_t(place) >= places.size()) return -1;
	uint8_t owner = places[size_t(place)];
	// it joins where the party is (the inn, the camp): next to the selected one (who talked to the
	// innkeeper; on the co-op host the caller's selection), else the first one standing
	const Actor* leader = GetSelectedPCSingle(true);
	if (leader && !leader->GetCurrentArea()) leader = nullptr;
	for (const Actor* pc : PCs) {
		if (leader) break;
		if (!IsDead(pc) && pc->GetCurrentArea()) {
			leader = pc;
		}
	}
	if (!leader && !PCs.empty()) leader = PCs[0];
	Map* map = leader ? leader->GetCurrentArea() : GetCurrentArea();
	if (!map) return -1;
	reserve->SetBase(IE_EA, EA_PC);
	map->AddActor(reserve, true);
	reserve->SetPosition(leader ? leader->Pos : reserve->Pos, true);
	// it rested at camp meanwhile (Ethren decision)
	reserve->SetBase(IE_HITPOINTS, reserve->GetBase(IE_MAXHITPOINTS));
	reserve->InParty = 0;
	int slot = JoinParty(reserve, JP_JOIN | JP_SELECT);
	DCC::Funnel& funnel = reserve->dccExt.funnel;
	funnel.status = DCC::FunnelStatus::Chosen;
	// the hero is the player's whose place it was (co-op: the one who picked, or the host for it)
	if (owner != DCC::FUNNEL_NO_PLAYER) funnel.player = owner;
	// a reserve kept the level-1 XP from the funnel's end and makes its class pick now
	if (!reserve->GetBase(IE_LEVEL)) funnel.flags |= DCC::FUNNEL_CLASS_PENDING;
	reserve->DCCExtChanged();
	places.erase(places.begin() + place);
	dccRecord.places = places;
	Log(MESSAGE, "DCC", "{} joins the party from the reserves (slot {}, level {}, {} XP).", fmt::WideToChar { reserve->GetShortName() }, reserve->InParty, reserve->GetBase(IE_LEVEL), reserve->GetBase(IE_XP));
	return slot;
}

std::vector<Actor*> Game::DCCReserves() const
{
	std::vector<Actor*> reserves;
	for (Actor* npc : NPCs) {
		if (npc->dccExt.funnel.status == DCC::FunnelStatus::Reserve) reserves.push_back(npc);
	}
	std::stable_sort(reserves.begin(), reserves.end(), [](const Actor* a, const Actor* b) {
		return a->dccExt.funnel.order < b->dccExt.funnel.order;
	});
	return reserves;
}

std::vector<uint8_t> Game::DCCFreePlaces() const
{
	std::vector<uint8_t> places;
	if (!partysize || PCs.size() >= partysize) return places;
	places = dccRecord.places;
	// the record follows the party: a place taken by other means (a stock join) goes, the oldest
	// first; a place nobody left (fewer than four went on, a stock leave) is the host's
	places.resize(partysize - PCs.size(), DCC::FUNNEL_NO_PLAYER);
	return places;
}

}
