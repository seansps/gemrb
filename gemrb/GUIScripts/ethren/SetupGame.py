# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the demo's SetupGame (GUIScripts/demo/SetupGame.py) plus the funnel (M3.1): with cards
# accepted on the rolling screen (RollScreen.Accepted) it makes the peasants from dccstart.2da's
# TEMPLATE, one per card in roster order, instead of the demo's protagonist (CreatePeasant below).
# Without cards (dccstart.2da ROLL 0, eg. funneltest's autotest) it is the demo's start.

import GemRB
import Start
import RollScreen
import FunnelRoll
from GUIDefines import GS_PARTYAI
from ie_stats import *
from ie_slots import SLOT_QUIVER
from ie_restype import RES_ITM

StartGold = 0

def OnLoad():
	cards = RollScreen.Accepted
	if cards:
		global StartGold
		StartGold = GemRB.GameGetPartyGold () # the game's own (the GAM's), before the peasants' copper
		owners = RollScreen.AcceptedOwners or {}
		template = RollScreen.StartValue ("TEMPLATE", "DCCPEAS")
		for card in cards:
			CreatePeasant (card, template, owners.get (card["Order"], -1))
		# the area scripts know the party was rolled (funneltest leaves its own fifteen out)
		GemRB.SetGlobal ("DCC_ROLLED", "GLOBAL", 1)
		_Log ("the party of %d is made; the purse holds %d cp" % (len (cards), GemRB.GameGetPartyGold ()))
	else:
		# skipping chargen
		GemRB.CreatePlayer ("protagon", 1|0x8000) # exportable
		GemRB.SetPlayerName (1, Start.Name, 0)
		GemRB.SetPlayerSound (1, "male001")
		# set strength, so items can be picked up (currently needed for encumbrance labels)
		GemRB.SetPlayerStat (1, IE_STR, 14)

	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	GemRB.SetVar("CHAPTER", 1)
	GemRB.EnterGame()

def _Log (text):
	print ("[Funnel] " + text)

COLOR_STATS = (IE_METAL_COLOR, IE_MINOR_COLOR, IE_MAJOR_COLOR, IE_SKIN_COLOR, IE_LEATHER_COLOR, IE_ARMOR_COLOR, IE_HAIR_COLOR)
SCORE_STATS = (IE_STR, IE_DEX, IE_CON, IE_CHR, IE_INT, IE_DCC_LUCK) # Str Agi Sta Per Int Luck

def _GiveItem (pc, card, item, what, slots = None, count = 0):
	"""an occupation item if the game has it (else logged); slots: the slot types to put it in"""
	if not item or not GemRB.HasResource (item, RES_ITM, 1):
		_Log ("%s: no item for %s%s" % (card["Name"], what, " (%s)" % item if item else ""))
		return ""
	slot = -1
	if slots:
		free = GemRB.GetSlots (pc, slots, -1)
		slot = free[0] if free else -1
	if count:
		GemRB.CreateItem (pc, item, slot, count, 0, 0)
	else:
		GemRB.CreateItem (pc, item, slot)
	return item

def CreatePeasant (card, template, owner = -1):
	"""one card as a party member in its roster slot: abilities, Luck and augur, HP, species, sex,
	alignment, the OCCU and FUNL records, the occupation's weapon (equipped), ammunition and trade
	good, the copper (into the purse), the body weight and the looks"""
	slot = card["Order"]
	GemRB.CreatePlayer (template, slot | 0x8000)
	pc = slot
	GemRB.SetPlayerName (pc, card["Name"], 0)
	for stat, score in zip (SCORE_STATS, card["Scores"]):
		GemRB.SetPlayerStat (pc, stat, score)
	GemRB.SetPlayerStat (pc, IE_DCC_AUGUR, card["Augur"])
	GemRB.SetPlayerStat (pc, IE_DCC_AUGURMOD, card["AugurMod"])
	GemRB.SetPlayerStat (pc, IE_MAXHITPOINTS, card["HP"])
	GemRB.SetPlayerStat (pc, IE_HITPOINTS, card["HP"])
	GemRB.SetPlayerStat (pc, IE_RACE, card["Race"])
	GemRB.SetPlayerStat (pc, IE_SEX, card["Sex"])
	GemRB.SetPlayerStat (pc, IE_ALIGNMENT, FunnelRoll.ALIGNMENT_IE.get (card["Alignment"], 0x22))
	looks = card["Looks"]
	GemRB.SetPlayerStat (pc, IE_ANIMATION_ID, looks["Animation"])
	for stat, colour in zip (COLOR_STATS, looks["Colors"]):
		GemRB.SetPlayerStat (pc, stat, colour)
	GemRB.DCCSetLooks (pc, looks["Hair"], looks["Beard"])
	# the occupation's things: the weapon (DCCSetOccupation puts it in hand), ammunition in the quiver
	weapon = _GiveItem (pc, card, card["WeaponItem"], card["WeaponName"])
	if card["Ammo"]:
		_GiveItem (pc, card, card["AmmoItem"], "%d %s" % (card["Ammo"], card["AmmoName"]), SLOT_QUIVER, card["Ammo"])
	good = ""
	if not card["GoodCopper"]:
		good = _GiveItem (pc, card, card["GoodItem"], card["GoodName"])
	GemRB.DCCSetOccupation (pc, card["Occupation"], card["Sub"], good, weapon, card["Skills"])
	GemRB.DCCSetFunnel (pc, 1, slot, owner, 0)
	# the body weight was rolled for the template's race when it joined: again for its own
	GemRB.DCCSetBodyWeight (pc)
	GemRB.GameSetPartyGold (GemRB.GameGetPartyGold () + card["Copper"] + card["GoodCopper"])
	_Log ("%s (player %d)" % (FunnelRoll.Summary (card), owner))
	return pc
