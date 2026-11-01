# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.3): the end of the funnel, the rules half of its screens (ChooseFour.py, ClassPick.py,
# Reserves.py, FunnelEnd.py): the survivors as cards, sorting them, picking the four, the classes a
# peasant may take and the gods a cleric may serve, the graveyard's lines. No GUI and no GemRB import at
# module level, like FunnelRoll.py, so it is tested outside the engine (gemrb/tests/GUIScripts/
# test_funnelchoose.py). The screens build a survivor from the engine with Survivor () and keep the rest
# here.
#
# A survivor (a dict): Id (global ID), Order (roster order), Name, Scores (Str Agi Sta Per Int Luck, the
# current ones), Mods, HP, MaxHP, Kills, NearDeaths, Occupation (its name), Race, Alignment (1-3),
# Player (-1 none), Level, Class, God, Dead.

import FunnelRoll

PARTY = 4 # the party after the funnel (DCC::FUNNEL_PARTY_SIZE)

# the sort buttons: key, label; abilities, Luck and HP sort highest first, the roster order lowest first
SORTS = (
	("Order", "Order"), ("Str", "Str"), ("Agi", "Agi"), ("Sta", "Sta"), ("Per", "Per"), ("Int", "Int"),
	("Luck", "Luck"), ("HP", "HP"), ("Kills", "Kills"), ("NearDeaths", "Near deaths"),
)

# dccclass.2da's IDs
CLASS_NAMES = {1: "Wizard", 2: "Warrior", 3: "Cleric", 4: "Thief", 5: "Dwarf", 6: "Elf", 7: "Halfling", 8: "Peasant"}
CLERIC = 3
CASTERS = (1, 3, 6) # wizard, cleric, elf: they learn spells at the class pick
LEVEL0 = 8
ALIGN_LETTERS = {1: "L", 2: "N", 3: "C"}

###################################################
# a survivor from the engine

def Survivor (actor, tables = None):
	"""the card of a party member or reserve (global ID or party slot), read from the engine"""
	import GemRB
	from ie_stats import IE_STR, IE_DEX, IE_CON, IE_CHR, IE_INT, IE_DCC_LUCK, IE_HITPOINTS, IE_MAXHITPOINTS, IE_RACE, IE_STATE_ID
	stats = GemRB.GetDCCStats (actor)
	record = GemRB.GetDCCFunnel (actor)
	place = record["Funnel"]
	scores = [GemRB.GetPlayerStat (actor, stat) for stat in (IE_STR, IE_DEX, IE_CON, IE_CHR, IE_INT, IE_DCC_LUCK)]
	return {
		"Id": record["GlobalID"], "Slot": record["Slot"], "Order": place["Order"], "Name": GemRB.GetPlayerName (actor, 0), "Scores": scores,
		"Mods": [ability[3] for ability in stats["Abilities"]] if stats else [0] * 6,
		"HP": GemRB.GetPlayerStat (actor, IE_HITPOINTS), "MaxHP": GemRB.GetPlayerStat (actor, IE_MAXHITPOINTS),
		"Kills": place.get ("Kills", 0), "NearDeaths": place.get ("NearDeaths", 0),
		"Occupation": OccupationName (tables, record["Occupation"]["Id"], record["Occupation"]["Sub"]),
		"Race": GemRB.GetPlayerStat (actor, IE_RACE), "Alignment": stats["AlignmentCode"] if stats else 2,
		"Player": place["Player"], "Level": stats["Level"] if stats else 0, "Class": stats["Class"] if stats else LEVEL0,
		"God": stats["God"] if stats else 0, "GodName": stats["GodName"] if stats else "",
		"Status": place["StatusName"], "ClassPending": place["ClassPending"],
		"Dead": bool (GemRB.GetPlayerStat (actor, IE_STATE_ID) & 0x800),
	}

def OccupationName (tables, ident, sub = 0, short = False):
	"""an occupation id (and its sub-roll) as text, lower case ("farmer (wheat)")"""
	if not ident or not tables:
		return ""
	occupations = tables["dccoccup"]
	name = occupations.Text (ident, "NAME", "occupation %d" % ident)
	name = name[0].lower () + name[1:]
	if not short and occupations.Get (ident, "SUB") == "FARMER" and sub & 15:
		name += " (%s)" % tables["dccoccit"].Text ("FARMER_%d" % (sub & 15), "NAME")
	return name

###################################################
# picking the four

def Limit (survivors):
	"""how many may go on: at most four, at most every survivor (DCC::FunnelPicks)"""
	return max (0, min (PARTY, len (survivors)))

def Toggle (picks, ident, survivors):
	"""a click on a card: picked if there is room, else unpicked; returns the new list (in pick order)"""
	known = [s["Id"] for s in survivors]
	if ident not in known:
		return list (picks)
	if ident in picks:
		return [p for p in picks if p != ident]
	if len (picks) >= Limit (survivors):
		return list (picks)
	return list (picks) + [ident]

def Ready (picks, survivors):
	"""at least one and at most four survivors are picked (fewer than four is allowed, decided
	2026-10-31: the others wait in the reserves)"""
	return 0 < len (picks) <= Limit (survivors) and len (set (picks)) == len (picks) and all (p in [s["Id"] for s in survivors] for p in picks)

def SortValue (survivor, key):
	if key == "Order":
		return survivor["Order"]
	if key in FunnelRoll.ABILITIES:
		return survivor["Scores"][FunnelRoll.ABILITIES.index (key)]
	return survivor.get (key, 0)

def Sorted (survivors, key = "Order", descending = None):
	"""by a sort button's key; the roster order breaks ties. Order sorts lowest first, the rest highest"""
	if descending is None:
		descending = key != "Order"
	ordered = sorted (survivors, key = lambda s: s["Order"])
	return sorted (ordered, key = lambda s: SortValue (s, key), reverse = descending)

###################################################
# what a card says

def AbilityLine (survivor, which):
	return "  ".join ("%s %d (%s)" % (FunnelRoll.ABILITIES[i], survivor["Scores"][i], FunnelRoll.Signed (survivor["Mods"][i])) for i in which)

def RecordLine (survivor):
	"""the funnel's record: what it did (M3.3)"""
	kills, near = survivor["Kills"], survivor["NearDeaths"]
	return "Kills %d   Near deaths %d" % (kills, near)

def ClassLine (survivor):
	if survivor["Level"] <= 0 or survivor["Class"] == LEVEL0:
		# the funnel's survivors have the XP for level 1 and pick their class when they join the party
		return "Level 0, class pick pending" if survivor.get ("ClassPending") else "Level 0"
	line = "%s %d" % (CLASS_NAMES.get (survivor["Class"], "?"), survivor["Level"])
	if survivor.get ("GodName"):
		line += ", of " + survivor["GodName"]
	return line

def CardLines (survivor):
	"""a survivor's card: abilities, occupation and species, HP and alignment, the funnel's record"""
	occupation = survivor["Occupation"] or "no occupation"
	if survivor["Race"] != 1:
		occupation += " (%s)" % FunnelRoll.RACE_NAMES.get (survivor["Race"], "?")
	return [
		AbilityLine (survivor, (0, 1, 2)),
		AbilityLine (survivor, (3, 4, 5)),
		occupation,
		"HP %d/%d   %s   %s" % (max (0, survivor["HP"]), survivor["MaxHP"], FunnelRoll.ALIGNMENTS.get (survivor["Alignment"], "?"), ClassLine (survivor)),
		RecordLine (survivor),
	]

###################################################
# the level-1 class pick

def ClassesFor (classes, race):
	"""the classes a peasant of this race may take (dccclass.2da: a demihuman takes its own, a human
	any of the human ones), as [(id, name)] in the table's order"""
	choices = []
	for row in classes.order:
		ident, needs = classes.Int (row, "ID"), classes.Int (row, "RACE")
		if ident == LEVEL0 or not needs:
			continue
		if needs == race:
			choices.append ((ident, classes.Text (row, "NAME", row.capitalize ())))
	return choices

def ClassInfo (classes, ident):
	for row in classes.order:
		if classes.Int (row, "ID") == ident:
			text = "%s: hit die d%d" % (classes.Text (row, "NAME", row.capitalize ()), classes.Int (row, "HIT_DIE"))
			if classes.Int (row, "DEED"):
				text += ", the deed die"
			if ident in CASTERS:
				text += ", learns spells (cleric: from the god's list; wizard and elf: rolled)" if ident == CLERIC else ", learns spells (rolled, some may be swapped)"
			return text
	return ""

def GodsFor (gods, alignment):
	"""the gods a cleric of this alignment may serve: [(id, name)] in the table's order"""
	letter = ALIGN_LETTERS.get (alignment)
	return [(gods.Int (row, "ID"), gods.Text (row, "NAME", row)) for row in gods.order if (gods.Get (row, "ALIGN") or "").upper () == letter]

def GodDomain (gods, god):
	"""what a god is the god of ("death, dark magic"), "" for none or an unknown one"""
	for row in gods.order:
		if gods.Int (row, "ID") == god:
			return gods.Text (row, "DOMAIN")
	return ""

def GodLine (gods, god):
	"""a god with its domain, for the class pick ("Wolaeth, god of death, dark magic")"""
	for row in gods.order:
		if gods.Int (row, "ID") == god:
			domain = gods.Text (row, "DOMAIN")
			name = gods.Text (row, "NAME", row)
			return "%s, god of %s" % (name, domain) if domain else name
	return ""

def GodFollows (gods, god, alignment):
	"""the god after an alignment change: kept if it is of that alignment, else none (DCC::GodTable::Follow)"""
	return god if god in [g for g, name in GodsFor (gods, alignment)] else 0

def PickValid (classes, gods, race, classID, alignment, god):
	"""a class pick the engine takes: a class of the race, an alignment, and for a cleric a god of it"""
	if classID not in [c for c, name in ClassesFor (classes, race)] or alignment not in ALIGN_LETTERS:
		return False
	if classID == CLERIC:
		return god in [g for g, name in GodsFor (gods, alignment)]
	return True

###################################################
# the graveyard

def GraveLine (grave, occupation):
	"""eg. "Gristle, cordwainer: crossbow bolt (Marauder), the wooded trail" (M3.2's line: the
	occupation, also for one who had taken a class)"""
	name = grave["Name"]
	who = "%s, %s" % (name, occupation) if occupation else name
	killer, cause = grave.get ("Killer", ""), grave.get ("Cause", "")
	if killer and cause:
		how = "%s (%s)" % (cause, killer)
	else:
		how = cause or killer or "unknown causes"
	where = grave.get ("Place", "") or grave.get ("DiedIn", "")
	return "%s: %s%s" % (who, how, ", " + where if where else "")
