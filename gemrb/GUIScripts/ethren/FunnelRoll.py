# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.1): rolling the level-0 funnel's peasants, the rules half of the rolling screen
# (RollScreen.py). docs/rules/funnel.md, "Creation steps": per peasant, 3d6 in order for the six
# abilities (4d6 drop the lowest as a house option), Table 1-3's occupation (d100, with its sub-rolls:
# the farmer's crop, a duplicate's animal, the pushcart; 1d6 ammunition for missile weapons), HP 1d4 +
# Sta (at least 1), the birth augur (d30) with its Luck modifier, 5d12 copper, the random equipment
# item (Table 3-4, d24, M3.3); plus what the game needs: a sex, a name, an alignment (the roll is
# only the default: the player may pick another on the card, M3.3) and the looks (M2.11's sprites,
# colours, hair and beard).
#
# All of it is here and nothing else: no GUI, and no GemRB import at module level, so the rolls can be
# unit-tested outside the engine (engine/gemrb/tests/python/test_funnelroll.py). Tables come in as
# Table objects (LoadTables reads them through GemRB.LoadTable; ParseTwoDA reads a 2DA file's text),
# dice as a function die(sides) -> 1..sides (EngineDie: GemRB.Roll, the engine's RNG, so a seeded run
# rolls the same sixteen). The dice are always rolled in the same order for a card.

ABILITIES = ("Str", "Agi", "Sta", "Per", "Int", "Luck")
LUCK = 5
RACE_NAMES = {1: "human", 2: "elf", 4: "dwarf", 5: "halfling"}
LOOK_ROWS = {1: "HUMAN", 2: "ELF", 4: "DWARF", 5: "HALFLING"}
ALIGNMENTS = {1: "Lawful", 2: "Neutral", 3: "Chaotic"}
# IE_ALIGNMENT: the high nibble is law (1) / neutral (2) / chaos (3), the low one stays neutral
ALIGNMENT_IE = {1: 0x12, 2: 0x22, 3: 0x32}
# what a birth augur's lucky roll is, for the card (dccaugur.2da's TARGET)
AUGUR_TARGETS = {
	"ATTACK_ALL": "all attacks", "ATTACK_MELEE": "melee attacks", "ATTACK_MISSILE": "missile attacks",
	"ATTACK_UNARMED": "unarmed attacks", "ATTACK_MOUNTED": "mounted attacks", "DAMAGE_ALL": "all damage",
	"DAMAGE_MELEE": "melee damage", "DAMAGE_MISSILE": "missile damage", "OCCUPATION_WEAPON": "occupation weapon",
	"SKILL_CHECKS": "skill checks", "TRAPS": "finding traps", "SECRET_DOORS": "secret doors",
	"SPELL_CHECKS": "spell checks", "SPELL_DAMAGE": "spell damage", "TURN_UNHOLY": "turn unholy",
	"HEALING": "magical healing", "SAVES_ALL": "all saves", "SAVES_TRAPS": "saves vs traps",
	"SAVES_POISON": "saves vs poison", "SAVE_REF": "Reflex", "SAVE_FORT": "Fortitude", "SAVE_WILL": "Will",
	"AC": "armor class", "INITIATIVE": "initiative", "HIT_POINTS": "hit points", "CRIT_TABLES": "crits",
	"CORRUPTION": "corruption", "FUMBLES": "fumbles", "LANGUAGES": "languages", "SPEED": "speed",
}

###################################################
# tables

class Table (object):
	"""a 2DA's rows by name: Table.rows {ROW: {COLUMN: text}}, in file order in Table.order"""
	def __init__ (self, columns, rows, default = "*"):
		self.columns = [c.upper () for c in columns]
		self.order = [name.upper () for name, values in rows]
		self.rows = {}
		self.default = default
		for name, values in rows:
			self.rows[name.upper ()] = dict (zip (self.columns, values))

	def Get (self, row, column, default = None):
		value = self.rows.get (str (row).upper (), {}).get (column.upper ())
		if value is None or value == "*":
			return default
		return value

	def Int (self, row, column, default = 0):
		value = self.Get (row, column)
		try:
			return int (value, 0)
		except (TypeError, ValueError):
			return default

	def Text (self, row, column, default = ""):
		"""a name: _ for spaces"""
		value = self.Get (row, column)
		return value.replace ("_", " ") if value else default

def ParseTwoDA (text):
	"""a 2DA file's text as a Table (for the tests; the game reads them with GemRB.LoadTable)"""
	lines = [line.split () for line in text.splitlines ()]
	lines = [words for words in lines if words and not words[0].startswith ("#")]
	default = lines[1][0] if len (lines) > 1 else "*"
	columns = lines[2]
	rows = [(words[0], words[1:]) for words in lines[3:]]
	return Table (columns, rows, default)

def ReadGemRBTable (name):
	"""a 2DA through the engine's resource manager, as a Table"""
	import GemRB
	table = GemRB.LoadTable (name)
	columns = [table.GetColumnName (c) for c in range (table.GetColumnCount ())]
	rows = []
	for r in range (table.GetRowCount ()):
		rows.append ((table.GetRowName (r), [str (table.GetValue (r, c, 0)) for c in range (len (columns))]))
	return Table (columns, rows)

TABLE_NAMES = ("dccoccup", "dccoccit", "dccaugur", "abilmod", "dcclooks", "dccnames", "dccequip")

def LoadTables (read = ReadGemRBTable):
	"""{name: Table} of the tables the rolls need"""
	return dict ((name, read (name)) for name in TABLE_NAMES)

def EngineDie (sides):
	"""a die on the engine's RNG (seeded in the autotests)"""
	import GemRB
	return GemRB.Roll (1, sides, 0)

###################################################
# the rules

class Options (object):
	"""the rolling screen's house rules, both off by default (ui-design.md, "Funnel screens")"""
	def __init__ (self, rerollOne = False, fourDice = False, ammo = 0):
		self.rerollOne = rerollOne # reroll a single card
		self.fourDice = fourDice # 4d6, drop the lowest, for each ability
		self.ammo = ammo # a missile weapon's ammunition: 0 the book's 1d6, else this many (dccstart.2da AMMO)

def AbilityMod (tables, score):
	"""abilmod.2da: the DCC curve (3: -3 ... 18: +3)"""
	score = max (3, min (18, score))
	return tables["abilmod"].Int (score, "MOD")

def Signed (value):
	return "%+d" % value if value else "0"

def RollAbility (die, fourDice):
	"""3d6, or 4d6 with the lowest dropped"""
	dice = [die (6) for i in range (4 if fourDice else 3)]
	if fourDice:
		dice.remove (min (dice))
	return sum (dice)

def FindOccupation (tables, d100):
	occupations = tables["dccoccup"]
	for row in occupations.order:
		if occupations.Int (row, "LOW") <= d100 <= occupations.Int (row, "HIGH"):
			return row
	raise ValueError ("Table 1-3 has no row for %d" % d100)

def ItemName (tables, key, weapon = False):
	items = tables["dccoccit"]
	name = items.Text (key, "NAME", key.lower ())
	if weapon and items.Get (key, "AS"):
		name += " (as %s)" % items.Text (key, "AS")
	return name

def RollOccupation (die, tables, earlier, ammoCount = 0):
	"""Table 1-3 with its sub-rolls. earlier: the occupation ids of the cards already rolled (a
	duplicate farmer or herder gets another animal). Returns the card's occupation part."""
	occupations, items = tables["dccoccup"], tables["dccoccit"]
	d100 = die (100)
	row = FindOccupation (tables, d100)
	ident = int (row)
	sub = 0
	good = occupations.Get (row, "GOOD")
	name = occupations.Text (row, "NAME")
	kind = occupations.Get (row, "SUB")
	animal = 0
	if kind == "FARMER":
		crop = die (8)
		sub = crop
		name += " (%s)" % items.Text ("FARMER_%d" % crop, "NAME")
	if kind in ("FARMER", "ANIMAL") and ident in earlier:
		# a duplicate: another animal (1 sheep, 2 goat, 3 cow, 4 duck, 5 goose, 6 mule)
		animal = die (6)
		good = "ANIMAL_%d" % animal
		sub = sub | (animal << 4) if kind == "FARMER" else animal
	if kind == "CART":
		load = die (6)
		sub = load
	weapon = occupations.Get (row, "WEAPON")
	card = {
		"D100": d100,
		"Occupation": ident,
		"OccupationName": name,
		"Race": occupations.Int (row, "RACE", 1),
		"Sub": sub,
		"Weapon": weapon,
		"WeaponName": ItemName (tables, weapon, True),
		"WeaponItem": (items.Get (weapon, "ITEM") or "").lower (),
		"Good": good,
		"GoodName": ItemName (tables, good),
		"GoodItem": (items.Get (good, "ITEM") or "").lower (),
		"GoodCopper": items.Int (good, "CP"),
		"Skills": [tag for tag in (occupations.Get (row, "SKILLS") or "").split ("|") if tag],
		"Ammo": 0,
		"AmmoName": "",
		"AmmoItem": "",
	}
	if kind == "CART":
		card["GoodName"] += " (%s)" % items.Text ("CART_%d" % sub, "NAME")
	ammo = items.Get (weapon, "AMMO")
	if ammo:
		card["Ammo"] = ammoCount or die (6)
		card["AmmoName"] = items.Text (ammo, "NAME")
		card["AmmoItem"] = (items.Get (ammo, "ITEM") or "").lower ()
	return card

def PickName (die, tables, sex, taken):
	names = tables["dccnames"]
	free = [row for row in names.order if names.Int (row, "SEX") == sex and row not in taken]
	if not free:
		free = [row for row in names.order if names.Int (row, "SEX") == sex] or names.order
	return free[die (len (free)) - 1].capitalize ()

def ParseRange (text, low, high):
	if not text or "-" not in text:
		return low, high
	a, b = text.split ("-", 1)
	return int (a), int (b)

def RollInRange (die, text, low, high):
	a, b = ParseRange (text, low, high)
	return a + die (b - a + 1) - 1

def RollLooks (die, tables, race, sex):
	"""M2.11's looks by species and sex: animation, hair and beard overlays, the seven colours
	(metal, minor, major, skin, leather, armor, hair as rows of pal16). dcclooks.2da's row gives the
	bald body (ANIM) and the styles that are rendered on its rig (S4: hair 1-7 and the beards on the
	male, 8-13 on the female), so a style never lands on a body it wasn't drawn for. The dice: the style,
	d100 for no hair (BALD), d100 for a beard (BEARD) and its style, then the colours"""
	looks = tables["dcclooks"]
	row = "%s_%s" % (LOOK_ROWS.get (race, "HUMAN"), "F" if sex == 2 else "M")
	if row not in looks.rows:
		row = "HUMAN_F" if sex == 2 else "HUMAN_M"
	styles = [int (s) for s in (looks.Get (row, "HAIR") or "0").split ("|")]
	hair = styles[die (len (styles)) - 1]
	if hair and die (100) <= looks.Int (row, "BALD"):
		hair = 0
	beard = 0
	beards = [int (s) for s in (looks.Get (row, "BEARDS") or "0").split ("|") if int (s)]
	if beards and die (100) <= looks.Int (row, "BEARD"):
		beard = beards[die (len (beards)) - 1]
	skin = RollInRange (die, looks.Get (row, "SKIN"), 28, 35)
	hairColour = RollInRange (die, looks.Get (row, "HAIRCOL"), 36, 45)
	minor = RollInRange (die, looks.Get (row, "DYES"), 7, 27)
	major = RollInRange (die, looks.Get (row, "DYES"), 7, 27)
	leather = RollInRange (die, looks.Get (row, "LEATHER"), 46, 51)
	metal = RollInRange (die, looks.Get (row, "METAL"), 0, 6)
	return {
		"Animation": looks.Int (row, "ANIM", 0x6500),
		"Hair": hair,
		"Beard": beard,
		"Colors": [metal, minor, major, skin, leather, metal, hairColour],
	}

def RollEquipment (die, tables):
	"""Table 3-4: the random equipment item of a 0-level character (d24, M3.3)"""
	equipment = tables.get ("dccequip")
	roll = die (24)
	if not equipment:
		return {"Equipment": roll, "EquipmentName": "", "EquipmentItem": "", "EquipmentCount": 0}
	return {
		"Equipment": roll,
		"EquipmentName": equipment.Text (roll, "NAME", "item %d" % roll),
		"EquipmentItem": (equipment.Get (roll, "ITEM") or "").lower (),
		"EquipmentCount": max (1, equipment.Int (roll, "COUNT", 1)),
	}

def SetAlignment (card, code):
	"""the player's pick (M3.3): 1 lawful, 2 neutral, 3 chaotic; False for another code"""
	if code not in ALIGNMENTS:
		return False
	card["Alignment"] = code
	return True

def NextAlignment (code):
	"""the alignment after this one, round: lawful, neutral, chaotic"""
	return code % 3 + 1

def RollCard (die, tables, order, options = None, earlier = (), taken = ()):
	"""one peasant. order: its place, 1-16; earlier: the occupation ids rolled before it (duplicate
	animals); taken: names already given"""
	options = options or Options ()
	sex = 1 if die (2) == 1 else 2
	name = PickName (die, tables, sex, set (n.upper () for n in taken))
	scores = [RollAbility (die, options.fourDice) for ability in ABILITIES]
	mods = [AbilityMod (tables, score) for score in scores]
	card = {"Order": order, "Name": name, "Sex": sex, "Scores": scores, "Mods": mods}
	card.update (RollOccupation (die, tables, earlier, options.ammo))
	augurs = tables["dccaugur"]
	augur = die (30)
	card["Augur"] = augur
	card["AugurName"] = augurs.Text (augur, "NAME")
	card["AugurTarget"] = augurs.Get (augur, "TARGET") or ""
	card["AugurMod"] = mods[LUCK] # frozen at creation
	hp = die (4) + mods[2]
	if card["AugurTarget"] == "HIT_POINTS":
		hp += card["AugurMod"] # bountiful harvest: from level 0 on (open-questions.md, M3.1)
	card["HP"] = max (1, hp)
	card["Copper"] = sum (die (12) for i in range (5))
	card["Alignment"] = die (3)
	card["Looks"] = RollLooks (die, tables, card["Race"], sex)
	# M3.3: Table 3-4's item, last, so the earlier dice stay where M3.1 had them
	card.update (RollEquipment (die, tables))
	return card

def RollAll (die, tables, count = 16, options = None):
	cards = []
	for order in range (1, count + 1):
		cards.append (RollCard (die, tables, order, options, [c["Occupation"] for c in cards], [c["Name"] for c in cards]))
	return cards

def Reroll (die, tables, cards, index, options = None):
	"""the house rule: one card again, a new peasant in its place (a new name too)"""
	others = cards[:index] + cards[index + 1:]
	card = RollCard (die, tables, index + 1, options, [c["Occupation"] for c in cards[:index]], [c["Name"] for c in others])
	cards[index] = card
	return card

###################################################
# the card as text (the screen's lines, and what co-op players see)

def AbilityLine (card, which):
	return "  ".join ("%s %d (%s)" % (ABILITIES[i], card["Scores"][i], Signed (card["Mods"][i])) for i in which)

def AugurLine (card):
	target = AUGUR_TARGETS.get (card["AugurTarget"], card["AugurTarget"].replace ("_", " ").lower ())
	return "Augur: %s (%s) %s" % (card["AugurName"], target, Signed (card["AugurMod"]))

def GearLine (card):
	weapon = card["WeaponName"]
	if card["Ammo"]:
		weapon += ", %d %s" % (card["Ammo"], card["AmmoName"])
	line = "%s; %s" % (weapon, card["GoodName"])
	if card.get ("EquipmentName"):
		line += "; " + card["EquipmentName"][0].lower () + card["EquipmentName"][1:]
	return line

def OccupationLine (card):
	race = card["Race"]
	return card["OccupationName"] + ("" if race == 1 else " (%s)" % RACE_NAMES.get (race, "?"))

def CardLines (card):
	"""the card below its name: abilities, occupation, gear, HP and money, augur"""
	return [
		AbilityLine (card, (0, 1, 2)),
		AbilityLine (card, (3, 4, 5)),
		OccupationLine (card),
		GearLine (card),
		"HP %d   %d cp   %s %s" % (card["HP"], card["Copper"], ALIGNMENTS.get (card["Alignment"], ""), "female" if card["Sex"] == 2 else "male"),
		AugurLine (card),
	]

def CardText (card):
	"""the name and the lines, one per line (what co-op players get, NetSetLobbyCards)"""
	return "\n".join ([card["Name"]] + CardLines (card))

def Summary (card):
	"""one line for logs and tests"""
	return "%d %s: %s, %s, HP %d, %s, %d cp" % (card["Order"], card["Name"], " ".join (str (s) for s in card["Scores"]), card["OccupationName"], card["HP"], card["AugurName"], card["Copper"])
