# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# DCC character sheet text (ethren): what the record screen shows and what the
# dcctest autotest logs. The numbers come from GemRB.GetDCCStats (the engine works
# them out, see docs/architecture/rules-dcc.md); this only formats them.
import GemRB

Abilities = ("Str", "Agi", "Sta", "Per", "Int", "Luck")
AbilityNames = ("Strength", "Agility", "Stamina", "Personality", "Intelligence", "Luck")
States = ((1, "immobile"), (2, "unconscious"), (4, "witless"), (8, "cursed by fortune"))
CritTables = ("-", "I", "II", "III", "IV", "V", "M", "DR", "DN", "G", "U") # IE_DCC_CRITTABLE: I-V, then the monster tables
Alignments = ("Lawful", "Chaotic", "Neutral")
SkillNames = (
	("BACKSTAB", "Backstab"), ("SNEAK", "Sneak silently"), ("HIDE", "Hide in shadows"),
	("PICK_POCKET", "Pick pocket"), ("CLIMB", "Climb sheer surfaces"), ("PICK_LOCK", "Pick lock"),
	("FIND_TRAP", "Find trap"), ("DISABLE_TRAP", "Disable trap"), ("FORGE", "Forge document"),
	("DISGUISE", "Disguise self"), ("READ_LANG", "Read languages"), ("HANDLE_POISON", "Handle poison"),
	("SCROLL_DIE", "Cast spell from scroll")
)

def Signed (value):
	return "%+d" % value

def Words (text):
	return text.replace ("_", " ")

def Dice (count, sides, add = 0):
	if not sides:
		return "-"
	text = "%dd%d" % (count, sides)
	if add:
		text += Signed (add)
	return text

def StateNames (states):
	names = [name for bit, name in States if states & bit]
	return ", ".join (names) if names else "-"

def ActionDice (stats):
	return "+".join ("d%d" % die for die in stats["ActionDice"]) or "-"

def AttackText (stats):
	# deed die classes roll it on top of the (zero) fixed bonus
	if stats["DeedDie"]:
		return Signed (stats["Attack"]) + "+" + Dice (1, stats["DeedDie"], stats["DeedAdd"])[1:] + " (deed)"
	return Signed (stats["Attack"])

def ClassText (stats):
	if not stats["Character"]:
		return "not a DCC character"
	if stats["Level"] == 0:
		return "0-level"
	return "%s %d" % (Words (stats["ClassName"]), stats["Level"])

def Lines (pc):
	"""the sheet as lines of text"""
	stats = GemRB.GetDCCStats (pc)
	name = GemRB.GetPlayerName (pc, 0)
	if stats is None:
		return [name, "No DCC rules in this game."]

	lines = []
	lines.append ("%s, %s (%s)" % (name, ClassText (stats), Alignments[stats["Alignment"]]))
	xp = "XP %d" % stats["XP"]
	if stats["NextXP"] >= 0:
		xp += " / %d" % stats["NextXP"]
	if stats["CanLevelUp"]:
		xp += ", can level up"
	lines.append (xp)
	hp = "HP %d / %d" % (stats["HP"], stats["MaxHP"])
	if stats["HitDie"]:
		hp += "   Hit die d%d" % stats["HitDie"]
	lines.append (hp)
	lines.append ("")
	for i, ability in enumerate (stats["Abilities"]):
		score, current, loss, mod = ability
		text = "%-13s %2d" % (AbilityNames[i], current)
		if loss or current != score:
			text += " of %d" % score
		text += "  (%s)" % Signed (mod)
		if loss:
			text += "  loss %d" % loss
		lines.append (text)
	lines.append ("")
	augur = "Birth augur: %s (%s), %s" % (Words (stats["AugurName"]) or "none", Words (stats["AugurTarget"].lower ()), Signed (stats["AugurMod"]))
	lines.append (augur)
	lines.append ("Luck %d of %d" % (stats["Luck"], stats["LuckMax"]))
	lines.append ("")
	lines.append ("AC %d   Init %s   Speed %d'" % (stats["AC"], Signed (stats["Init"]), stats["Speed"]))
	lines.append ("Saves: Fort %s  Ref %s  Will %s" % (Signed (stats["Fort"]), Signed (stats["Ref"]), Signed (stats["Will"])))
	lines.append ("Attack %s   Melee %s   Missile %s" % (AttackText (stats), Signed (stats["Melee"]), Signed (stats["Missile"])))
	crit = Dice (stats["CritDice"], stats["CritDie"], stats["CritAdd"])
	if stats["CritTable"]:
		crit += "/" + CritTables[stats["CritTable"]]
	if stats["Threat"] and stats["Threat"] < 20:
		crit += " on %d-20" % stats["Threat"]
	lines.append ("Crit %s   Action dice %s" % (crit, ActionDice (stats)))
	if stats["LuckDie"]:
		lines.append ("Luck die d%d" % stats["LuckDie"])
	if stats["SpellsKnown"]:
		lines.append ("Spells known %d, up to level %d" % (stats["SpellsKnown"], stats["MaxSpellLevel"]))
	if stats["Skills"]:
		lines.append ("")
		for key, label in SkillNames:
			if key in stats["Skills"]:
				value = stats["Skills"][key]
				lines.append ("%-23s %s" % (label, ("d%d" % value) if key == "SCROLL_DIE" else Signed (value)))
	lines.append ("")
	lines.append ("States: " + StateNames (stats["States"]))
	return lines

def Compact (pc):
	"""one line for logs"""
	stats = GemRB.GetDCCStats (pc)
	name = GemRB.GetPlayerName (pc, 0)
	if stats is None:
		return name + ": no DCC rules"
	if not stats["Character"]:
		return "%s: not a DCC character (class %d)" % (name, stats["Class"])
	abilities = " ".join ("%s%d/%d" % (Abilities[i], a[1], a[0]) + ("-%d" % a[2] if a[2] else "") for i, a in enumerate (stats["Abilities"]))
	parts = [
		"%s: %s xp%d hp%d/%d" % (name, ClassText (stats), stats["XP"], stats["HP"], stats["MaxHP"]),
		abilities,
		"augur%d %s %s" % (stats["Augur"], stats["AugurTarget"], Signed (stats["AugurMod"])),
		"AC%d Fort%s Ref%s Will%s Init%s" % (stats["AC"], Signed (stats["Fort"]), Signed (stats["Ref"]), Signed (stats["Will"]), Signed (stats["Init"])),
		"atk %s melee%s missile%s crit %s/%s threat%d act %s" % (AttackText (stats), Signed (stats["Melee"]), Signed (stats["Missile"]),
			Dice (stats["CritDice"], stats["CritDie"], stats["CritAdd"]), CritTables[stats["CritTable"]], stats["Threat"], ActionDice (stats)),
		"speed%d hd%d states %s" % (stats["Speed"], stats["HitDie"], StateNames (stats["States"]))
	]
	if stats["Skills"]:
		parts.append ("skills " + " ".join ("%s%s" % (key, Signed (stats["Skills"][key])) for key, label in SkillNames if key in stats["Skills"] and key != "SCROLL_DIE"))
	return " | ".join (parts)
