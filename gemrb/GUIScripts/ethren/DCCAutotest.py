# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The DCC checks of the dcctest autotest (testdata/dcctest, scripts/run.sh dcctest-auto).
#
# Nothing runs unless the game's DCC_AUTOTEST global is 1, which only the autotest
# variant of the dcctest area script sets once the party has formed. Then, a step a
# tick (so the engine refreshes the stats in between):
#   1. logs every party member's numbers (DCCSheet.Compact) and compares them with
#      dctexp.2da, which dcctest ships in its autotest/ folder (hand-worked from the spec)
#   2. rolls saves and checks with fixed dice through GemRB.DCCRoll
#   3. drains abilities (DCC:AbilityLoss) and checks the states at 0
#   4. saves, loads that save and checks that Luck, augurs and loss came back
#   5. rests in the field, then in bed, and checks what healed (never Luck)
#   6. levels a peasant up, and kills one character with a lethal drain
#   7. opens the record screen (GUIREC) and pages through the party
#   8. fights (M2.3): goblins and a few DCC fighters outside the party attack with forced
#      dice (GemRB.DCCForceDice): hits, misses, crits on Table III and the next-lower
#      fallback, a monster's crit, fumbles by armor, two weapons for a thief and a halfling,
#      cursed by fortune, a real Attack action with initiative, bleeding out (healed, and
#      dead then recovered by rolling the body over) and a level 0 death
# Every check logs "[DCC] ok ..." or "[DCC] FAIL ...", and the end "[DCC] autotest done:
# N checks, F failed"; the DCT_AT_DCC global becomes 1 if all passed, 2 if not.
import GemRB
import DCCSheet
from GUIDefines import *
from ie_stats import *

STATE_DEAD = 0x800
LETHAL = 0x100
PERMANENT = 1 # FX_DURATION_INSTANT_PERMANENT

Step = "wait"
Token = 0
Checks = 0
Failures = 0

def _Log (text):
	print ("[DCC] " + text)

def _Check (what, got, expected):
	global Checks, Failures
	Checks += 1
	if got == expected:
		_Log ("ok %s = %s" % (what, got))
	else:
		Failures += 1
		_Log ("FAIL %s = %s, expected %s" % (what, got, expected))

def _Party ():
	"""script name -> party slot"""
	return {GemRB.GetPlayerName (pc, 2).lower (): pc for pc in range (1, GemRB.GetPartySize () + 1)}

def _Stats (name):
	pc = _Party ().get (name)
	return (pc, GemRB.GetDCCStats (pc)) if pc else (None, None)

def _Drain (pc, ability, points, lethal = False):
	GemRB.ApplyEffect (pc, "DCC:AbilityLoss", points, ability | (LETHAL if lethal else 0), "", "", "", "", PERMANENT)

# the dctexp.2da columns and where GetDCCStats keeps them
Columns = {
	"CLASS": lambda s: s["Class"], "LEVEL": lambda s: s["Level"], "XP": lambda s: s["XP"],
	"HP": lambda s: s["HP"], "MAXHP": lambda s: s["MaxHP"], "LUCK": lambda s: s["Luck"],
	"AUGUR": lambda s: s["Augur"], "AUGURMOD": lambda s: s["AugurMod"], "AC": lambda s: s["AC"],
	"FORT": lambda s: s["Fort"], "REF": lambda s: s["Ref"], "WILL": lambda s: s["Will"],
	"INIT": lambda s: s["Init"], "ATTACK": lambda s: s["Attack"], "DEEDDIE": lambda s: s["DeedDie"],
	"MELEE": lambda s: s["Melee"], "MISSILE": lambda s: s["Missile"], "CRITDIE": lambda s: s["CritDie"],
	"CRITTABLE": lambda s: s["CritTable"], "THREAT": lambda s: s["Threat"],
	"ACT1": lambda s: s["ActionDice"][0] if s["ActionDice"] else 0, "SPEED": lambda s: s["Speed"],
	"HITDIE": lambda s: s["HitDie"], "LUCKDIE": lambda s: s["LuckDie"], "STATES": lambda s: s["States"],
	"NEXTXP": lambda s: s["NextXP"], "SPELLS": lambda s: s["SpellsKnown"],
}

def _CheckTable ():
	table = GemRB.LoadTable ("dctexp")
	if not table:
		_Check ("dctexp.2da loaded", False, True)
		return
	party = _Party ()
	for row in range (table.GetRowCount ()):
		name = table.GetRowName (row).lower ()
		pc = party.get (name)
		_Check (name + " in the party", pc is not None, True)
		if not pc:
			continue
		stats = GemRB.GetDCCStats (pc)
		for col in range (table.GetColumnCount ()):
			key = table.GetColumnName (col)
			value = table.GetValue (row, col, GTV_STR)
			if value == "*":
				continue
			if key in Columns:
				got = Columns[key] (stats)
			else: # a skill
				got = stats["Skills"].get (key)
			_Check ("%s %s" % (name, key), got, int (value))

def _StepCheck ():
	for pc in range (1, GemRB.GetPartySize () + 1):
		_Log (DCCSheet.Compact (pc))
	_CheckTable ()

	# rolls with fixed dice: d20 + save (or ability mod) vs DC
	pc, grub = _Stats ("dctgrub")
	roll = GemRB.DCCRoll (pc, 0, 0, 10, 15) # Fort -6: 15 - 6 = 9
	_Check ("dctgrub Fort save 15 vs DC 10 total", roll["Total"], 9)
	_Check ("dctgrub Fort save 15 vs DC 10 success", roll["Success"], 0)
	pc, ilsa = _Stats ("dctilsa")
	roll = GemRB.DCCRoll (pc, 0, 2, 5, 2) # Will +3: 2 + 3 = 5
	_Check ("dctilsa Will save 2 vs DC 5 success", roll["Success"], 1)
	roll = GemRB.DCCRoll (pc, 1, 0, 20, 17) # Str check +3: 20
	_Check ("dctilsa Str check 17 vs DC 20 success", roll["Success"], 1)
	roll = GemRB.DCCRoll (pc, 2, 0, 0, 18) # Luck check: 18 <= 18
	_Check ("dctilsa Luck check 18 under 18", roll["Success"], 1)

	# drains: Grub's Str to 0 (immobile), Brak's Sta past 0 (unconscious, floors at 0),
	# Ilsa's Luck to 0 (cursed by fortune) and 2 points of her Int (no state)
	party = _Party ()
	_Drain (party["dctgrub"], 0, 3)
	_Drain (party["dctbrak"], 2, 16)
	_Drain (party["dctilsa"], 5, 18)
	_Drain (party["dctilsa"], 4, 2)
	return "drained"

def _CheckDrained (when):
	pc, grub = _Stats ("dctgrub")
	_Check ("%s dctgrub Str" % when, grub["Abilities"][0][:3], [3, 0, 3])
	_Check ("%s dctgrub states" % when, grub["States"], DCC_STATE_IMMOBILE)
	_Check ("%s dctgrub held" % when, GemRB.GetPlayerStat (pc, IE_HELD), 1)
	pc, brak = _Stats ("dctbrak")
	_Check ("%s dctbrak Sta" % when, brak["Abilities"][2][:3], [15, 0, 16])
	_Check ("%s dctbrak states" % when, brak["States"], DCC_STATE_UNCONSCIOUS)
	_Check ("%s dctbrak Fort (1 + Sta 0's -3)" % when, brak["Fort"], -2)
	pc, ilsa = _Stats ("dctilsa")
	_Check ("%s dctilsa Luck" % when, [ilsa["Luck"], ilsa["LuckMax"]], [0, 18])
	_Check ("%s dctilsa Int" % when, ilsa["Abilities"][4][:3], [18, 16, 2])
	_Check ("%s dctilsa states" % when, ilsa["States"], DCC_STATE_CURSED)
	_Check ("%s dctilsa augur" % when, [ilsa["Augur"], ilsa["AugurMod"]], [23, 3])
	roll = GemRB.DCCRoll (pc, 0, 0, 5, 20) # Fort +3 against DC 5, but cursed
	_Check ("%s dctilsa cursed Fort save 20 vs DC 5" % when, [roll["Success"], roll["Cursed"]], [0, 1])
	roll = GemRB.DCCRoll (pc, 2, 0, 0, 1)
	_Check ("%s dctilsa cursed Luck check 1" % when, roll["Success"], 0)

SaveName = "DCC autotest"

def _OurSaves ():
	return [game for game in GemRB.GetSaveGames () if game.GetName () == SaveName]

def _StepDrained ():
	_CheckDrained ("drained")
	# save (over the last run's save), then load that save
	for game in _OurSaves ():
		GemRB.DeleteSaveGame (game)
	_Check ("game saved", GemRB.SaveGame (None, SaveName), 0)
	return "save"

def _StepSave ():
	games = _OurSaves ()
	_Check ("save written", len (games), 1)
	if not games:
		return "done"
	_Log ("loading the save '%s'" % SaveName)
	game = games[0]
	def Load ():
		GemRB.LoadGame (game)
		GemRB.EnterGame ()
	GemRB.QuitGame ()
	GemRB.SetTimer (Load, 0, 0)
	return "loading"

def _StepLoaded ():
	_CheckDrained ("loaded")
	# a night in the field heals 1 point of loss
	info = GemRB.RestParty (1 | 16, -1, 1)
	_Check ("field rest allowed", info["Error"], False)
	return "rested"

def _StepRested ():
	pc, grub = _Stats ("dctgrub")
	_Check ("field rest dctgrub Str", grub["Abilities"][0][:3], [3, 1, 2])
	_Check ("field rest dctgrub states", grub["States"], 0)
	pc, brak = _Stats ("dctbrak")
	_Check ("field rest dctbrak Sta", brak["Abilities"][2][:3], [15, 0, 15])
	_Check ("field rest dctbrak states", brak["States"], DCC_STATE_UNCONSCIOUS)
	pc, ilsa = _Stats ("dctilsa")
	_Check ("field rest dctilsa Luck loss", ilsa["Abilities"][5][2], 18)
	_Check ("field rest dctilsa Int", ilsa["Abilities"][4][:3], [18, 17, 1])
	# bed rest (an inn): 2 points
	info = GemRB.RestParty (16, -1, 1)
	_Check ("bed rest allowed", info["Error"], False)
	return "bedrested"

def _StepBedRested ():
	pc, grub = _Stats ("dctgrub")
	_Check ("bed rest dctgrub Str", grub["Abilities"][0][:3], [3, 3, 0])
	pc, brak = _Stats ("dctbrak")
	_Check ("bed rest dctbrak Sta", brak["Abilities"][2][:3], [15, 2, 13])
	_Check ("bed rest dctbrak awake", brak["States"], 0)
	pc, ilsa = _Stats ("dctilsa")
	_Check ("bed rest dctilsa Luck (never heals)", [ilsa["Luck"], ilsa["Abilities"][5][2]], [0, 18])
	_Check ("bed rest dctilsa Int", ilsa["Abilities"][4][:3], [18, 18, 0])

	# level up: Ilsa is human, so no dwarf; with 10 XP she becomes a warrior
	GemRB.SetPlayerStat (pc, IE_XP, 10)
	stats = GemRB.GetDCCStats (pc)
	_Check ("dctilsa can level up at 10 XP", stats["CanLevelUp"], 1)
	_Check ("dctilsa refused the dwarf class", GemRB.DCCLevelUp (pc, 5), -1)
	before = stats["MaxHP"]
	hp = GemRB.DCCLevelUp (pc, 2)
	_Check ("dctilsa warrior hp roll in d12+3", 4 <= hp <= 15, True)
	_Log ("dctilsa rolled %d hp" % hp)
	stats = GemRB.GetDCCStats (pc)
	_Check ("dctilsa class, level", [stats["Class"], stats["Level"]], [2, 1])
	_Check ("dctilsa max hp", stats["MaxHP"], before + hp)
	_Check ("dctilsa warrior deed die, threat", [stats["DeedDie"], stats["Threat"], stats["CritDie"]], [3, 19, 12])

	# a lethal drain: Mirela's Stamina 10 drained by 10 kills her
	_Drain (_Party ()["dctmirel"], 2, 10, True)
	return "lethal"

Waited = 0

def _StepLethal ():
	global Waited
	# dying takes the death animation; give it a few seconds
	pc = _Party ()["dctmirel"]
	dead = bool (GemRB.GetPlayerStat (pc, IE_STATE_ID) & STATE_DEAD)
	Waited += 1
	if not dead and Waited < 20:
		return "lethal"
	_Check ("dctmirel died of a lethal drain", dead, True)
	pc, ilsa = _Stats ("dctilsa")
	_Check ("dctilsa warrior saves", [ilsa["Fort"], ilsa["Ref"], ilsa["Will"]], [4, 4, 3])
	_Check ("dctilsa warrior init (Agi +3, level 1)", ilsa["Init"], 4)
	pc = _Party ()["dctbrak"]
	_Check ("dctbrak alive after a non-lethal drain", bool (GemRB.GetPlayerStat (pc, IE_STATE_ID) & STATE_DEAD), False)
	for pc in range (1, GemRB.GetPartySize () + 1):
		_Log (DCCSheet.Compact (pc))

	# the record screen: open it on Sly, page through the party and close it
	import GUIREC
	GUIREC.OpenRecordsWindow (_Party ()["dctsly"])
	_Check ("record window open", GUIREC.Window is not None, True)
	for line in DCCSheet.Lines (GUIREC.Shown):
		_Log ("record: " + line)
	for step in range (GemRB.GetPartySize ()):
		GUIREC._Step (1)
	_Check ("record window back on Sly", GUIREC.Shown, _Party ()["dctsly"])
	GUIREC.CloseRecordsWindow ()
	_Check ("record window closed", GUIREC.Window, None)
	return "fight"

# --- M2.3 combat -------------------------------------------------------------
# Everything here uses forced dice; the order an attack uses them is in
# ActorDCCCombat.cpp: (deed die,) attack die(s), then per hand that hits its damage,
# crit dice and the crit result's dice; on a fumble the fumble die and its dice.

def _ID (name):
	return GemRB.DCCFindActor (name)

def _Attack (attacker, target, dice):
	GemRB.DCCForceDice (attacker, dice)
	rolls = GemRB.DCCAttack (attacker, target)
	GemRB.DCCForceDice (attacker, [])
	return rolls

def _Pick (record, keys):
	return [record[key] for key in keys]

def _HP (actor):
	return GemRB.GetPlayerStat (actor, IE_HITPOINTS)

def _Dead (actor):
	return bool (GemRB.GetPlayerStat (actor, IE_STATE_ID) & STATE_DEAD)

def _Logged (text):
	return any (text in line for line in GemRB.GetDCCCombatLog ())

def _Fight ():
	GemRB.GetDCCCombatLog (1)
	korg, vex, pip, eda, brun, peas = [_ID (n) for n in ("dctkorg", "dctvex", "dctpip", "dcteda", "dctbrun", "dctpeas")]
	gob1, gob2 = _ID ("dctgob1"), _ID ("dctgob2")
	_Check ("combatants in the area", all ([korg, vex, pip, eda, brun, peas, gob1, gob2]), True)

	# a monster's numbers come from its CRE's DCC block and Table 9-10 (1 HD humanoid: III/d6)
	gob = GemRB.GetDCCStats (gob1)
	_Check ("goblin attack, HD, AC, init", [gob["Character"], gob["Attack"], gob["HD"], gob["AC"], gob["Init"]], [0, -1, 1, 10, -1])
	_Check ("goblin crit III 1d6 on 20", [gob["CritTable"], gob["CritDice"], gob["CritDie"], gob["Threat"], gob["ActionDice"]], [3, 1, 6, 20, [20]])
	k = GemRB.GetDCCStats (korg)
	_Check ("korg deed d4, crit 1d14/III on 19-20, init +3", [k["DeedDie"], k["CritDie"], k["CritTable"], k["Threat"], k["Init"]], [4, 14, 3, 19, 3])

	# a hit: d20 14 + deed d4 2 + Str 2 = 18 vs AC 10; 1d8 5 + Str 2 + deed 2 = 9
	hp = _HP (gob1)
	r = _Attack (korg, gob1, [2, 14, 5])
	_Check ("korg hit", _Pick (r[0], ["Natural", "Deed", "Modifier", "Total", "AC", "Hit", "Critical", "Damage"]), [14, 2, 4, 18, 10, 1, 0, 9])
	yield
	_Check ("goblin took 9", _HP (gob1), hp - 9)
	_Check ("log: korg's hit", _Logged ("Korg attacks Goblin 1: d20 14 + d4 2 +2 = 18 vs AC 10, hit: 1d8 +4 = 9 damage"), True)

	# a miss: 5 + 1 + 2 = 8 vs 10
	r = _Attack (korg, gob1, [1, 5])
	_Check ("korg miss", _Pick (r[0], ["Total", "Hit", "Damage", "Fumble"]), [8, 0, 0, 0])
	_Check ("log: korg's miss", _Logged ("Korg attacks Goblin 1: d20 5 + d4 1 +2 = 8 vs AC 10, miss"), True)

	# the warrior's threat range: a natural 19 that hits crits; 1d14 14 on Crit Table III is +2d8
	hp = _HP (gob1)
	r = _Attack (korg, gob1, [1, 19, 4, 14, 5, 6])
	_Check ("korg threat 19 crit", _Pick (r[0], ["Hit", "Critical", "CritTable", "CritRoll", "Damage", "Result"]), [1, 1, 3, 14, 18, "crushing blow"])
	_Check ("log: crit III", _Logged ("CRITICAL (III: 1d14 = 14): crushing blow; +2d8 = 11 damage"), True)
	yield
	_Check ("goblin took 18", _HP (gob1), hp - 18)
	# 18 is a hit, not a crit
	r = _Attack (korg, gob1, [1, 18, 3])
	_Check ("korg natural 18 no crit", _Pick (r[0], ["Hit", "Critical", "Damage"]), [1, 0, 6])

	# III-10 shatters the goblin's (mundane) weapon; the same roll again can't, so III-9 happens
	r = _Attack (korg, gob1, [1, 20, 2, 10])
	_Check ("crit III-10 weapon shattered", _Pick (r[0], ["Critical", "CritRoll", "Result"]), [1, 10, "weapon shattered"])
	_Check ("log: weapon breaks", _Logged ("Goblin 1's weapon breaks"), True)
	yield
	r = _Attack (korg, gob1, [1, 20, 2, 10, 3, 4])
	_Check ("crit III-10 unarmed falls back to III-9", _Pick (r[0], ["CritRoll", "Result", "Damage"]), [10, "leg wound", 12])

	# III-2 knocks it down; attacks on a prone target get +2 in melee
	r = _Attack (korg, gob1, [1, 20, 2, 2])
	_Check ("crit III-2 prone", r[0]["Result"], "knocked down")
	yield
	_Check ("goblin prone", GemRB.GetPlayerStat (gob1, IE_DCC_PRONE), 1)
	r = _Attack (korg, gob1, [1, 10, 2])
	_Check ("+2 against a prone target", _Pick (r[0], ["Modifier", "Total"]), [5, 15])

	# a two-weapon thief, Agi 9: d14 and d12 (Table 4-3); the off hand's natural 1 fumbles on its own,
	# unarmored: d4, 3 on Table 4-2 is -2 on the next attack
	r = _Attack (vex, gob2, [10, 1, 4, 3])
	_Check ("vex two attacks", len (r), 2)
	_Check ("vex primary d14 hit", _Pick (r[0], ["Hand", "Die", "Natural", "Total", "Hit", "Damage", "Fumble"]), [0, 14, 10, 10, 1, 4, 0])
	_Check ("vex off hand d12 fumbles alone", _Pick (r[1], ["Hand", "Die", "Natural", "Hit", "Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 12, 1, 0, 1, 4, 3, "off balance"])
	yield
	_Check ("vex -2 on the next attack", GemRB.GetPlayerStat (vex, IE_DCC_ATKMOD), -2)
	r = _Attack (vex, gob2, [5, 5])
	_Check ("vex next attack at -2", _Pick (r[0], ["Modifier", "Total"]), [-2, 3])
	yield
	_Check ("vex penalty gone", GemRB.GetPlayerStat (vex, IE_DCC_ATKMOD), 0)

	# a halfling with two weapons: Agi 12 counts as 16, d16 and d16; a single 1 doesn't fumble
	r = _Attack (pip, gob2, [1, 9, 3])
	_Check ("pip two d16s", [r[0]["Die"], r[1]["Die"]], [16, 16])
	_Check ("pip single 1: miss, no fumble", _Pick (r[0], ["Natural", "Hit", "Fumble"]), [1, 0, 0])
	_Check ("pip off hand hits", _Pick (r[1], ["Natural", "Total", "Hit", "Damage"]), [9, 10, 1, 3])
	# a natural 16 auto-hits and crits (III, 1d8 1: +1d6)
	r = _Attack (pip, gob2, [16, 2, 4, 1, 2])
	_Check ("pip natural 16 crit", _Pick (r[0], ["Hit", "Critical", "CritRoll", "Damage"]), [1, 1, 1, 6])
	_Check ("pip off hand 2 misses", r[1]["Hit"], 0)
	# both 1s: one fumble, chainmail's d12
	r = _Attack (pip, gob2, [1, 1, 5])
	_Check ("pip double 1 fumbles", _Pick (r[0], ["Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 12, 5, "trips and falls"])
	_Check ("pip double 1: one fumble roll", r[1]["Fumble"], 0)

	# cursed by fortune (Ilsa's Luck is 0): even a natural 20 fails and doesn't crit
	ilsa = _Party ()["dctilsa"]
	r = _Attack (ilsa, gob2, [3, 20])
	_Check ("cursed natural 20", _Pick (r[0], ["Natural", "Hit", "Critical", "Cursed"]), [20, 0, 0, 1])

	# an immobile character (Str 0) can't attack
	grub = _Party ()["dctgrub"]
	_Drain (grub, 0, 3)
	yield
	r = _Attack (grub, gob2, [15])
	_Check ("immobile can't attack", [len (r), _Logged ("Grub can't attack")], [0, True])
	_Drain (grub, 0, -3)

	# a monster's crit on a PC: 1d6 6 on III is +1d8; 6 + 8 = 14 drops Eda (10 hp, level 2): bleeding out
	r = _Attack (gob2, eda, [20, 6, 6, 8])
	_Check ("goblin crit on eda", _Pick (r[0], ["Total", "Critical", "CritTable", "CritRoll", "Damage"]), [19, 1, 3, 6, 14])
	yield
	e = GemRB.GetDCCStats (eda)
	_Check ("eda bleeding out", [e["HP"], e["States"] & DCC_STATE_DYING, e["BleedRounds"], _Dead (eda)], [0, DCC_STATE_DYING, 2, False])
	_Check ("log: eda falls", _Logged ("Eda falls, bleeding out: 2 rounds to heal"), True)
	# healed in time: up from 0, and 1 Stamina less for good
	GemRB.ApplyEffect (eda, "CurrentHPModifier", 3, 0)
	yield
	e = GemRB.GetDCCStats (eda)
	_Check ("eda saved", [e["HP"], e["States"] & DCC_STATE_DYING, _Dead (eda)], [3, 0, False])
	_Check ("eda Sta 13 -> 12", e["Abilities"][2][0], 12)

	# Brun (level 1, 4 hp): 15 - 1 = 14 vs AC 10, 6 damage: 1 round to bleed out, nobody helps
	r = _Attack (gob2, brun, [15, 6])
	_Check ("goblin drops brun", _Pick (r[0], ["Total", "Hit", "Damage"]), [14, 1, 6])
	yield
	b = GemRB.GetDCCStats (brun)
	_Check ("brun bleeding out", [b["States"] & DCC_STATE_DYING, b["BleedRounds"]], [DCC_STATE_DYING, 1])
	# a 0-level peasant at 0 hp is just dead
	r = _Attack (gob2, peas, [15, 6])
	waited = 0
	while not (_Dead (brun) and _Dead (peas)) and waited < 40:
		waited += 1
		yield
	_Check ("brun bled out", _Dead (brun), True)
	_Check ("log: brun bleeds out", _Logged ("Brun bleeds out"), True)
	_Check ("level 0 peasant dead at once", [_Dead (peas), _Logged ("Peasant falls")], [True, False])
	_Check ("no body recovery at level 0", GemRB.DCCRecoverBody (peas, korg, 1), -1)

	# rolling Brun's body over: Luck check 5 under his 12; back at 1 hp, d3 2: -1 Agi for good, -4 for an hour
	GemRB.DCCForceDice (brun, [2])
	_Check ("brun recovered", GemRB.DCCRecoverBody (brun, korg, 5), 1)
	yield
	b = GemRB.GetDCCStats (brun)
	_Check ("brun alive at 1 hp", [_Dead (brun), b["HP"]], [False, 1])
	_Check ("brun Agi 10 -> 9", b["Abilities"][1][0], 9)
	_Check ("brun -4 to all rolls", GemRB.GetPlayerStat (brun, IE_DCC_ROLLMOD), -4)
	roll = GemRB.DCCRoll (brun, 0, 0, 10, 15) # Fort 1 + 15 - 4 = 12
	_Check ("brun's save with the -4", roll["Total"], 12)
	# Mirela died of the lethal drain earlier: Luck 6, a 15 fails, and there's no second try
	mirela = _Party ()["dctmirel"]
	_Check ("mirela's body: Luck check fails", GemRB.DCCRecoverBody (mirela, _Party ()["dctbrak"], 15), 0)
	_Check ("mirela's body: once only", GemRB.DCCRecoverBody (mirela, _Party ()["dctbrak"], 1), -1)

	# a real attack: Korg's Attack action rolls initiative (d20 10 + 3) and the round's deed die,
	# then swings with the projectile; 15 + 2 + 2 = 19
	before = GemRB.GetDCCLastAttack (korg)
	serial = before[0]["Serial"] if before else 0
	GemRB.DCCForceDice (korg, [10, 2, 15, 5])
	GemRB.ExecuteString ('Attack("dctgob1")', korg)
	waited = 0
	while waited < 60:
		last = GemRB.GetDCCLastAttack (korg)
		if last and last[0]["Serial"] > serial:
			break
		waited += 1
		yield
	GemRB.ExecuteString ("ClearActions(Myself)", korg)
	GemRB.DCCForceDice (korg, [])
	last = GemRB.GetDCCLastAttack (korg)
	_Check ("korg's attack action swung", bool (last) and last[0]["Serial"] > serial, True)
	if last:
		_Check ("korg's real attack", _Pick (last[0], ["Natural", "Deed", "Total", "Hit", "Damage"]), [15, 2, 19, 1, 9])
	_Check ("log: initiative", _Logged ("Korg rolls initiative: d20 10 +3 = 13"), True)
	for i in range (6):
		yield

	# Korg fumbles: leather's d8, 7 on Table 4-2 drops the weapon
	r = _Attack (korg, gob2, [1, 1, 7])
	_Check ("korg fumble", _Pick (r[0], ["Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 8, 7, "drops its weapon"])
	_Check ("log: fumble", _Logged ("Fumble (d8 7 = 7): drops its weapon; Korg drops its weapon"), True)

	for line in GemRB.GetDCCCombatLog ():
		_Log ("combat: " + line)

FightSteps = None

def _StepFight ():
	global FightSteps
	if FightSteps is None:
		FightSteps = _Fight ()
	try:
		next (FightSteps)
		return "fight"
	except StopIteration:
		return "done"

Steps = {
	"check": _StepCheck,
	"drained": _StepDrained,
	"save": _StepSave,
	"loaded": _StepLoaded,
	"rested": _StepRested,
	"bedrested": _StepBedRested,
	"lethal": _StepLethal,
	"fight": _StepFight,
}

def _Tick (token):
	global Step, Failures
	if token != Token or Step in ("loading", "finished"):
		return
	try:
		if Step == "wait":
			if GemRB.GetGameVar ("DCC_AUTOTEST") == 1:
				_Log ("autotest start")
				Step = "check"
			return
		if Step == "done":
			_Log ("autotest done: %d checks, %d failed" % (Checks, Failures))
			GemRB.SetGlobal ("DCT_AT_DCC", "GLOBAL", 1 if Failures == 0 else 2)
			Step = "finished"
			return
		Step = Steps[Step] ()
	except Exception as error:
		Failures += 1
		_Log ("FAIL step %s raised %s: %s" % (Step, type (error).__name__, error))
		Step = "done"

def OnGameStart ():
	"""MessageWindow.OnLoad: (re)starts the ticks; a load in the middle continues"""
	global Token, Step
	Token += 1
	if Step == "loading":
		Step = "loaded"
	token = Token
	GemRB.SetTimer (lambda: _Tick (token), 500, -1)
