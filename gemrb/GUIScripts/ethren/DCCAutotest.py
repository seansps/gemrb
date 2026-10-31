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
#   9. Luck burn and Mighty Deeds (M2.4): burn policies on checks and saves (step 2), the
#      settings and burned Luck through the save and load (step 4), a thief's nightly
#      recovery and a warrior's lack of it (step 5); in the fight a policy burn that turns a
#      miss into a hit, "never", a warrior cancelling a fumble, a thief's luck dice, a
#      halfling burning for an ally at +2, a prompt answered with GemRB.AnswerPrompt and one
#      left to time out (the policy's answer), a save vs death that waits for the answer
#      (burn and survive; decline and die), and deeds: the deed die on attack and damage,
#      a signature trip on 3+, nothing on 2, a queued one-off disarm used once, defend, precision
#  10. combat completion (M2.3b): combat options and lasting injuries through the save and the
#      rests (steps 2, 4, 5), then in the fight: untrained weapons, fumble 4 and 16+, the dwarf's
#      shield bash, a backstab and surprise, grappling and a stray blow on the pinned, firing into
#      melee, charge, withdrawal, battle rage, subdual blows, cleave and rampage, Crit Table DR
#      (wounds, flinging, a severed arm), morale (first death, leader, alone and hurt), the record
#      screen's "Roll body over" and the Combat panel
#  11. spellcasting (M2.5): Mirela learns spells (mercurial magic with her Luck), spellburns, loses a
#      spell and gains a corruption before the save; the extension is checked after the load and
#      the rests (lost spells back, spellburn back a point a day after the first); then after the
#      fight a wizard, an elf and a cleric outside the party cast the starter spells with forced
#      dice: every band of all six spells, spell loss and a spellburned recast, the save DC = the
#      check total, misfires, corruption with and without a Luck burn (and the prompt, with the
#      wizard in the party), mercurial magic, disapproval and atonement, lay on hands, turn unholy,
#      the spellburn prompt of a real SpellRES cast, and amnesia
# Every check logs "[DCC] ok ..." or "[DCC] FAIL ...", and the end "[DCC] autotest done:
# N checks, F failed"; the DCT_AT_DCC global becomes 1 if all passed, 2 if not.
import GemRB
import DCCSheet
from GUIDefines import *
from ie_stats import *

STATE_DEAD = 0x800
STATE_PANIC = 0x4
LETHAL = 0x100
PERMANENT = 1 # FX_DURATION_INSTANT_PERMANENT
AFTER_BONUSES = 9 # FX_DURATION_INSTANT_PERMANENT_AFTER_BONUSES
# DCC:Combat (Parameter2): the injury kind, and the flags of lasting effects (DCC/Combat.h)
COMBAT_INJURY = 10
COMBAT_UNTIL_HEALED = 0x100
COMBAT_FOR_GOOD = 0x200

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
	_StepCheckLuck ()
	_StepCheckMagic ()

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

# --- M2.4 Luck burns on rolls --------------------------------------------------
ALLFLAGS = DCC_LUCK_PAUSE | DCC_LUCK_ALLIES

def _Luck (actor):
	return GemRB.GetDCCStats (actor)["Luck"]

def _StepCheckLuck ():
	# Sly (thief 2, Luck 15) keeps a policy in his CRE: misses to hits while Luck stays at 5+.
	# A Str check of 3 vs DC 10 is 7 short; his d4 luck die averages 2.5, so he burns 3: 4+4+2
	pc, sly = _Stats ("dctsly")
	_Check ("dctsly policy from the CRE", [sly["LuckPolicy"], sly["LuckThreshold"], sly["LuckFlags"]], [DCC_BURN_MISS_TO_HIT, 5, ALLFLAGS])
	_Check ("dctsly burns a d4 a point, gets 2 back a night", [sly["BurnPerPoint"], sly["BurnDie"], sly["LuckRecovery"]], [0, 4, 2])
	GemRB.DCCForceDice (pc, [4, 4, 2])
	roll = GemRB.DCCRoll (pc, 1, 0, 10, 3, 1)
	GemRB.DCCForceDice (pc, [])
	_Check ("dctsly thief burn 3d4 on a Str check", [roll["Total"], roll["Success"], roll["LuckBurned"]], [13, 1, 3])
	_Check ("dctsly Luck 15 -> 12", _Luck (pc), 12)

	# Brak (warrior 1, Luck 14, Str +2): "never" doesn't burn on a Str check of 8 + 2 vs 12
	pc, brak = _Stats ("dctbrak")
	_Check ("dctbrak default policy", [brak["LuckPolicy"], brak["LuckFlags"], brak["Deed"], brak["CancelsFumbles"]], [DCC_BURN_AVOID_DISASTER, ALLFLAGS, DCC_DEED_TRIP, 1])
	GemRB.DCCSetLuckPolicy (pc, DCC_BURN_NEVER, 10, ALLFLAGS)
	roll = GemRB.DCCRoll (pc, 1, 0, 12, 8, 1)
	_Check ("dctbrak never burns", [roll["Total"], roll["Success"], roll["LuckBurned"]], [10, 0, 0])
	# misses to hits while Luck stays at 10+: burns 2 (14 -> 12); then 3 short would take him to 9: no
	GemRB.DCCSetLuckPolicy (pc, DCC_BURN_MISS_TO_HIT, 10, ALLFLAGS)
	roll = GemRB.DCCRoll (pc, 1, 0, 12, 8, 1)
	_Check ("dctbrak burns 2 for a success", [roll["Total"], roll["Success"], roll["LuckBurned"]], [12, 1, 2])
	roll = GemRB.DCCRoll (pc, 1, 0, 12, 7, 1)
	_Check ("dctbrak keeps Luck 10+", [roll["Success"], roll["LuckBurned"]], [0, 0])
	# only to avoid death: a plain failed Fort save (5 + 2 vs DC 8) no, a save vs death yes (1 point)
	GemRB.DCCSetLuckPolicy (pc, DCC_BURN_AVOID_DISASTER, 10, ALLFLAGS)
	roll = GemRB.DCCRoll (pc, 0, 0, 8, 5, 1)
	_Check ("dctbrak no burn on a plain save", [roll["Success"], roll["LuckBurned"]], [0, 0])
	roll = GemRB.DCCRoll (pc, 0, 0, 8, 5, 2)
	_Check ("dctbrak burns 1 to survive", [roll["Total"], roll["Success"], roll["LuckBurned"]], [8, 1, 1])
	_Check ("dctbrak Luck 14 -> 11", _Luck (pc), 11)
	_Check ("log: brak's burn", _Logged ("Brak burns 1 Luck on a save (+1): 7 -> 8 vs 8, success (Luck 11 left)"), True)
	# settings that must come back from the save: misses while Luck 8+, pause only, deeds push / blind next
	GemRB.DCCSetLuckPolicy (pc, DCC_BURN_MISS_TO_HIT, 8, DCC_LUCK_PAUSE)
	GemRB.DCCSetDeeds (pc, DCC_DEED_PUSH, DCC_DEED_BLIND)
	# M2.3b: the combat options and battle rage, and lasting injuries (Ilsa's eye for good, Grub's
	# lost sense of smell until healed) come back from the save too
	GemRB.DCCSetCombat (pc, DCC_OPT_SUBDUAL, 2)
	GemRB.ApplyEffect (_Party ()["dctilsa"], "DCC:Combat", DCC_INJ_EYE, COMBAT_INJURY | COMBAT_FOR_GOOD, "", "", "", "", AFTER_BONUSES)
	GemRB.ApplyEffect (_Party ()["dctgrub"], "DCC:Combat", DCC_INJ_SMELL, COMBAT_INJURY | COMBAT_UNTIL_HEALED, "", "", "", "", AFTER_BONUSES)

def _CheckLuckLoaded ():
	pc, brak = _Stats ("dctbrak")
	_Check ("loaded dctbrak Luck settings", [brak["LuckPolicy"], brak["LuckThreshold"], brak["LuckFlags"]], [DCC_BURN_MISS_TO_HIT, 8, DCC_LUCK_PAUSE])
	_Check ("loaded dctbrak deeds", [brak["Deed"], brak["NextDeed"]], [DCC_DEED_PUSH, DCC_DEED_BLIND])
	_Check ("loaded dctbrak burned Luck", [brak["Luck"], brak["LuckMax"], brak["Abilities"][5][2]], [11, 14, 3])
	pc, sly = _Stats ("dctsly")
	_Check ("loaded dctsly burned Luck", [sly["Luck"], sly["LuckMax"]], [12, 15])
	_Check ("loaded dctbrak combat options and battle rage", [brak["CombatOptions"], brak["Rage"]], [DCC_OPT_SUBDUAL, 2])
	GemRB.DCCSetCombat (_Party ()["dctbrak"], 0, 0)
	pc, ilsa = _Stats ("dctilsa")
	_Check ("loaded dctilsa eye put out", [ilsa["InjuryBits"] & DCC_INJ_EYE, ilsa["Injuries"]], [DCC_INJ_EYE, ["blind in one eye (for good)"]])
	pc, grub = _Stats ("dctgrub")
	_Check ("loaded dctgrub no sense of smell", [grub["InjuryBits"] & DCC_INJ_SMELL, grub["Injuries"]], [DCC_INJ_SMELL, ["no sense of smell (until healed)"]])

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
	_CheckLuckLoaded ()
	_CheckMagicLoaded ()
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
	# a thief gets his level (2) of burned Luck back a night; a warrior gets nothing back
	_Check ("field rest dctsly Luck 12 -> 14", _Luck (_Party ()["dctsly"]), 14)
	_Check ("field rest dctbrak Luck stays 11", _Luck (_Party ()["dctbrak"]), 11)
	# M2.3b: an injury "until healed" heals with the night, one for good stays
	_Check ("field rest dctgrub smell back", _Stats ("dctgrub")[1]["InjuryBits"] & DCC_INJ_SMELL, 0)
	_Check ("field rest dctilsa eye still out", _Stats ("dctilsa")[1]["InjuryBits"] & DCC_INJ_EYE, DCC_INJ_EYE)
	_CheckMagicRested (False)
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
	# up to the natural maximum only
	_Check ("bed rest dctsly Luck 14 -> 15 (max)", _Luck (_Party ()["dctsly"]), 15)
	_Check ("bed rest dctbrak Luck stays 11", _Luck (_Party ()["dctbrak"]), 11)
	_CheckMagicRested (True)

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

	# the Luck and Deeds panels (M2.4): the same calls their buttons make
	import DCCLuck
	brak = _Party ()["dctbrak"]
	GUIREC.OpenRecordsWindow (brak)
	DCCLuck.OpenLuckWindow (brak)
	_Check ("luck panel open", [DCCLuck.Window is not None, DCCLuck.Kind], [True, "luck"])
	DCCLuck._SetPolicy (DCC_BURN_NEVER)
	DCCLuck._SetPolicy (None, 12)
	DCCLuck._SetPolicy (None, None, 0)
	stats = GemRB.GetDCCStats (brak)
	_Check ("luck panel: never, keep 12, no pause", [stats["LuckPolicy"], stats["LuckThreshold"], stats["LuckFlags"]], [DCC_BURN_NEVER, 12, 0])
	DCCLuck.OpenDeedsWindow (brak)
	_Check ("deeds panel open", DCCLuck.Kind, "deeds")
	DCCLuck._SetDeeds (DCC_DEED_DISARM)
	DCCLuck._SetDeeds (-1, DCC_DEED_RALLY)
	stats = GemRB.GetDCCStats (brak)
	_Check ("deeds panel: signature disarm, next rally", [stats["Deed"], stats["NextDeed"]], [DCC_DEED_DISARM, DCC_DEED_RALLY])
	lines = DCCSheet.Lines (brak)
	for line in lines[-4:]:
		_Log ("record: " + line)
	_Check ("record shows the deeds", "Deeds: signature disarm, next attack rally" in lines, True)
	_Check ("record shows the Luck burn", "Luck burn: never; +1 each" in lines, True)
	# M2.3b: the record lists lasting injuries
	_Check ("record lists ilsa's eye", "Injuries: blind in one eye (for good)" in DCCSheet.Lines (_Party ()["dctilsa"]), True)
	DCCLuck.OpenDeedsWindow (_Party ()["dctsly"])
	_Check ("no deeds panel for a thief", DCCLuck.Window, None)
	GUIREC.CloseRecordsWindow ()
	_Check ("panels closed", [GUIREC.Window, DCCLuck.Window], [None, None])
	# back to the settings of step 2
	GemRB.DCCSetLuckPolicy (brak, DCC_BURN_MISS_TO_HIT, 8, DCC_LUCK_PAUSE)
	GemRB.DCCSetDeeds (brak, DCC_DEED_PUSH, DCC_DEED_BLIND)
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

def _LogCount (text):
	return sum (1 for line in GemRB.GetDCCCombatLog () if text in line)

def _Pending (actor):
	return GemRB.GetDCCStats (actor)["AttackPending"]

def _FightLuck ():
	"""M2.4 in combat: Luck burns (policies, prompts) and Mighty Deeds, with forced dice"""
	korg, pip, gob1, gob2 = _ID ("dctkorg"), _ID ("dctpip"), _ID ("dctgob1"), _ID ("dctgob2")
	sly, brak = _Party ()["dctsly"], _Party ()["dctbrak"]

	# the deed die adds to attack and damage; Korg's signature trip on a 3: d20 10 + d4 3 + Str 2 = 15,
	# 1d8 4 + 2 + 3 = 9; goblin 1's Ref save (d20 2 + 1) against the attack's 15 fails: prone
	_Check ("korg's signature deed", GemRB.GetDCCStats (korg)["Deed"], DCC_DEED_TRIP)
	GemRB.DCCForceDice (gob1, [2])
	r = _Attack (korg, gob1, [3, 10, 4])
	GemRB.DCCForceDice (gob1, [])
	_Check ("korg deed 3 on attack and damage, trip", _Pick (r[0], ["Deed", "Modifier", "Total", "AC", "Hit", "Damage", "DeedKind", "DeedDone", "DeedResult"]),
		[3, 5, 15, 10, 1, 9, DCC_DEED_TRIP, 1, "tripped"])
	yield
	_Check ("goblin 1 tripped: prone", GemRB.GetPlayerStat (gob1, IE_DCC_PRONE), 1)
	_Check ("log: korg's trip", _Logged ("Korg's deed d4 = 3: trip! tripped; Goblin 1 Ref save 2 +1 = 3 vs DC 15: failed; prone for 1 round"), True)
	# a 2 on the deed die: a hit without a deed
	r = _Attack (korg, gob2, [2, 12, 4])
	_Check ("korg deed 2: no deed", _Pick (r[0], ["Total", "Hit", "Damage", "DeedKind", "DeedDone"]), [16, 1, 8, DCC_DEED_TRIP, 0])
	_Check ("log: no trip", _Logged ("vs AC 10, hit: 1d8 +4 = 8 damage (deed 2: no trip)"), True)

	# a one-off disarm for the next attack only: a 4 sends goblin 2's blade out of reach (no attack for a round)
	GemRB.DCCSetDeeds (korg, -1, DCC_DEED_DISARM)
	_Check ("korg one-off disarm queued", GemRB.GetDCCStats (korg)["NextDeed"], DCC_DEED_DISARM)
	r = _Attack (korg, gob2, [4, 12, 4])
	_Check ("korg one-off disarm 4", _Pick (r[0], ["Hit", "Damage", "DeedKind", "DeedDone", "DeedResult"]), [1, 10, DCC_DEED_DISARM, 1, "weapon flies out of reach"])
	_Check ("one-off used up", [GemRB.GetDCCStats (korg)["NextDeed"], GemRB.GetDCCStats (korg)["Deed"]], [0, DCC_DEED_TRIP])
	yield
	_Check ("goblin 2 can't attack", GemRB.GetPlayerStat (gob2, IE_DCC_NOATTACK), 1)
	_Check ("log: goblin 2 disarmed", _Logged ("Goblin 2 drops its weapon"), True)
	r = _Attack (korg, gob2, [1, 12, 4])
	_Check ("korg back to the signature deed", [r[0]["DeedKind"], r[0]["DeedDone"]], [DCC_DEED_TRIP, 0])

	# defensive maneuvers on a 3: +1 AC for Korg; a precision shot on a 3: +1d4 damage (2)
	ac = GemRB.GetDCCStats (korg)["AC"]
	GemRB.DCCSetDeeds (korg, DCC_DEED_DEFEND, -1)
	r = _Attack (korg, gob2, [3, 12, 4])
	yield
	_Check ("korg defend 3: +1 AC", [r[0]["DeedDone"], GemRB.GetDCCStats (korg)["AC"]], [1, ac + 1])
	GemRB.DCCSetDeeds (korg, DCC_DEED_PRECISION, -1)
	r = _Attack (korg, gob2, [3, 12, 4, 2])
	_Check ("korg precision 3: +1d4", _Pick (r[0], ["DeedDone", "Damage"]), [1, 11])
	GemRB.DCCSetDeeds (korg, DCC_DEED_TRIP, -1)

	# Luck: misses to hits while Luck stays at 5+: 6 + 1 + 2 = 9 vs 10 burns 1 (10 -> 9); 4 + 2 + 1 = 7 damage
	GemRB.DCCSetLuckPolicy (korg, DCC_BURN_MISS_TO_HIT, 5, ALLFLAGS)
	r = _Attack (korg, gob2, [1, 6, 4])
	_Check ("korg policy burn: miss to hit", _Pick (r[0], ["Total", "AC", "Hit", "LuckBurned", "LuckBonus", "LuckBy", "Damage"]), [10, 10, 1, 1, 1, 0, 7])
	_Check ("korg Luck 10 -> 9", _Luck (korg), 9)
	_Check ("log: korg burns", _Logged ("Korg burns 1 Luck (+1): 9 -> 10, hit (Luck 9 left)"), True)
	# never: the same miss stays a miss (Pip only burns to avoid disaster)
	GemRB.DCCSetLuckPolicy (korg, DCC_BURN_NEVER, 5, ALLFLAGS)
	r = _Attack (korg, gob2, [1, 6])
	_Check ("korg never burns", _Pick (r[0], ["Total", "Hit", "LuckBurned"]), [9, 0, 0])
	_Check ("korg Luck stays 9", _Luck (korg), 9)
	# Pip, the halflings' good luck charm, burns for Korg in sight at +2 a point: 5 + 1 + 2 = 8, 1 point is 10
	GemRB.DCCSetLuckPolicy (pip, DCC_BURN_MISS_TO_HIT, 5, ALLFLAGS)
	r = _Attack (korg, gob2, [1, 5, 4])
	_Check ("pip burns for korg at +2", _Pick (r[0], ["Total", "Hit", "LuckBurned", "LuckBonus", "LuckBy"]), [10, 1, 1, 2, pip])
	_Check ("pip's Luck 10 -> 9, korg's 9", [_Luck (pip), _Luck (korg)], [9, 9])
	_Check ("log: pip burns for korg", _Logged ("Pip burns 1 Luck for Korg (+2): 8 -> 10, hit (Luck 9 left)"), True)
	GemRB.DCCSetLuckPolicy (pip, DCC_BURN_AVOID_DISASTER, 10, ALLFLAGS)
	# a warrior burns 1 Luck to cancel the fumble roll: the natural 1 misses, no fumble die
	GemRB.DCCSetLuckPolicy (korg, DCC_BURN_AVOID_DISASTER, 10, ALLFLAGS)
	r = _Attack (korg, gob2, [1, 1, 7])
	_Check ("korg cancels a fumble", _Pick (r[0], ["Hit", "Fumble", "FumbleCancelled", "FumbleDie", "Result", "LuckBurned"]), [0, 1, 1, 0, "", 1])
	_Check ("korg Luck 9 -> 8", _Luck (korg), 8)
	_Check ("log: korg shrugs off the fumble", _Logged ("Korg burns 1 Luck to shrug off the fumble (Luck 8 left)"), True)

	# a thief's luck dice in a fight: Sly (fists, +1) 4 + 1 = 5 vs 10 burns 2 (d4 3 + 3), Luck 15 -> 13
	r = _Attack (sly, gob2, [4, 3, 3, 1, 1])
	_Check ("sly burns 2d4 to hit", _Pick (r[0], ["Total", "Hit", "LuckBurned", "LuckBonus"]), [11, 1, 2, 6])
	_Check ("sly Luck 15 -> 13", _Luck (sly), 13)

	# a key roll asks Brak's player (he pauses on key rolls): goblin 2 at AC 25, d20 19 + d3 1 + 2 = 22
	# would crit with 3 more. The prompt offers 0..11 (his Luck), the policy (misses while 8+) says 3;
	# the player burns 4 (11 -> 7): 26, a crit, d12 2 - 1 (Luck 7) = 1 on Table III
	GemRB.DCCSetDeeds (brak, DCC_DEED_TRIP, DCC_DEED_NONE)
	GemRB.ApplyEffect (gob2, "DCC:Combat", 15, 5)
	yield
	GemRB.DCCForceDice (brak, [1, 19, 1, 2, 3])
	r = GemRB.DCCAttack (brak, gob2)
	_Check ("brak's attack waits for the prompt", [len (r), _Pending (brak)], [0, 1])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("Luck prompt for brak", prompt and [prompt["Tag"], prompt["Mine"], prompt["Actor"], prompt["Fields"][0]["Max"], prompt["Fields"][0]["Value"]],
		["luckburn", True, _ID ("dctbrak"), 11, 3])
	if prompt:
		_Log ("prompt: " + prompt["Text"])
		_Check ("answered the prompt", GemRB.AnswerPrompt (prompt["ID"], [4]), True)
	waited = 0
	while _Pending (brak) and waited < 10:
		waited += 1
		yield
	GemRB.DCCForceDice (brak, [])
	last = GemRB.GetDCCLastAttack (brak)
	_Check ("prompt burn 4: hit and crit", last and _Pick (last[0], ["Natural", "Total", "Hit", "Critical", "LuckBurned", "LuckBonus", "CritRoll"]), [19, 26, 1, 1, 4, 4, 1])
	_Check ("brak Luck 11 -> 7", _Luck (brak), 7)

	# a prompt nobody answers times out to the policy: misses while Luck 1+ burns the 3 (7 -> 4);
	# d12 3 - 2 (Luck 4) = 1
	GemRB.DCCSetLuckPolicy (brak, DCC_BURN_MISS_TO_HIT, 1, DCC_LUCK_PAUSE)
	GemRB.DCCForceDice (brak, [1, 19, 1, 3, 3])
	GemRB.DCCAttack (brak, gob2)
	_Check ("brak's second attack waits", _Pending (brak), 1)
	waited = 0
	while _Pending (brak) and waited < 60:
		waited += 1
		yield
	GemRB.DCCForceDice (brak, [])
	last = GemRB.GetDCCLastAttack (brak)
	_Check ("timeout: the policy's burn", last and _Pick (last[0], ["Total", "Hit", "Critical", "LuckBurned", "CritRoll"]), [25, 1, 1, 3, 1])
	_Check ("brak Luck 7 -> 4", _Luck (brak), 4)
	_Check ("log: brak's policy burn", _Logged ("Brak burns 3 Luck (+3): 22 -> 25, hit and crit (Luck 4 left)"), True)
	GemRB.ApplyEffect (gob2, "DCC:Combat", -15, 5)

	# a save vs death (DCTSLAY: Fort vs DC 11 or die) pauses and asks too; the death waits for the answer.
	# Brak (Fort -2 with Sta 2, Luck 4) rolls 11: 9, 2 short; the policy (misses while 1+) would burn 2, he burns 2 (4 -> 2)
	GemRB.DCCForceDice (brak, [11])
	GemRB.ApplySpell (brak, "DCTSLAY")
	GemRB.DCCForceDice (brak, [])
	_Check ("log: brak's save vs death waits", _Logged ("Brak fails a Fortitude save against death: 9 vs DC 11; burn Luck?"), True)
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("death save prompt", prompt and [prompt["Tag"], prompt["Fields"][0]["Max"], prompt["Fields"][0]["Value"]], ["luckburn", 4, 2])
	if prompt:
		_Log ("prompt: " + prompt["Text"])
		GemRB.AnswerPrompt (prompt["ID"], [2])
	for i in range (4):
		yield
	_Check ("brak survives by burning 2", [_Dead (brak), _Luck (brak)], [False, 2])
	_Check ("log: brak survives", _Logged ("Brak burns 2 Luck on a save vs death (+2): 9 -> 11 vs DC 11, survives (Luck 2 left)"), True)
	# Sly (Fort +1) rolls 3: 4 vs 11; he could burn but his player says 0: the death happens then
	GemRB.DCCForceDice (sly, [3])
	GemRB.ApplySpell (sly, "DCTSLAY")
	GemRB.DCCForceDice (sly, [])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("sly's death save prompt", prompt and prompt["Tag"], "luckburn")
	_Check ("sly alive while it waits", _Dead (sly), False)
	if prompt:
		GemRB.AnswerPrompt (prompt["ID"], [0])
	waited = 0
	while not _Dead (sly) and waited < 20:
		waited += 1
		yield
	_Check ("sly declines: dead", [_Dead (sly), _Luck (sly)], [True, 13])

	# back to the M2.3 setup for the last check: Korg's Luck 10 and no burns
	GemRB.SetPlayerStat (korg, IE_DCC_LOSS_LCK, 0)
	GemRB.DCCSetLuckPolicy (korg, DCC_BURN_NEVER, 10, ALLFLAGS)
	yield

# --- M2.3b combat completion --------------------------------------------------

def _Wait (done, most = 40):
	"""yields until done () or most ticks"""
	waited = 0
	while not done () and waited < most:
		waited += 1
		yield

def _FightCompletion ():
	"""M2.3b: the crit and fumble results that are rules now, and the other combat rules, with forced dice"""
	ids = dict ((name, _ID ("dct" + name)) for name in ("durn", "nim", "wren", "arlo", "vala", "tank", "ox", "thug1", "thug2", "thug3",
		"weak1", "weak2", "weak3", "weak4", "drake", "orc1", "orc2", "orc3", "wolf"))
	import DCCLuck
	_Check ("M2.3b combatants in the area", all (ids.values ()), True)
	if not all (ids.values ()):
		return
	durn, nim, wren, arlo, vala, tank, ox = [ids[n] for n in ("durn", "nim", "wren", "arlo", "vala", "tank", "ox")]
	thug1, thug2, thug3, weak1, weak2, weak3, weak4, drake = [ids[n] for n in ("thug1", "thug2", "thug3", "weak1", "weak2", "weak3", "weak4", "drake")]

	# untrained: Wren, a wizard, with a club: d20 -> d16; 10 + 0 = 10 vs AC 10, 1d4 2
	_Check ("wren untrained in the club", GemRB.GetDCCStats (wren)["Trained"], 0)
	r = _Attack (wren, thug1, [10, 2])
	_Check ("wren untrained: d16", _Pick (r[0], ["Die", "Natural", "Total", "Hit", "Damage"]), [16, 10, 10, 1, 2])
	_Check ("record: untrained weapon", any ("untrained weapon (-1d)" in line for line in DCCLuck.Lines (GemRB.GetDCCStats (wren))), True)
	# fumble 4 (unarmored d4): the weapon is fouled for a turn; the next attack can't be made with it
	r = _Attack (wren, thug1, [1, 4])
	_Check ("wren fumble 4: weapon fouled", _Pick (r[0], ["Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 4, 4, "weapon fouled"])
	yield
	_Check ("wren's weapon fouled", GemRB.GetDCCStats (wren)["Fouled"] > 0, True)
	r = _Attack (wren, thug1, [15])
	_Check ("wren can't use the fouled club", [len (r), _Logged ("Wren can't attack: its weapon is fouled")], [0, True])

	# the dwarf's shield bash: deed d3 2; handaxe d20 12 + 2 + 1 = 15, 1d6 4 + 3 = 7; bash d14 9 + 2 + 1 = 12, 1d3 3 + 3 = 6
	GemRB.DCCSetLuckPolicy (durn, DCC_BURN_NEVER, 10, DCC_LUCK_ALLIES)
	hp = _HP (thug1)
	r = _Attack (durn, thug1, [2, 12, 9, 4, 3])
	_Check ("durn attacks with axe and shield", len (r), 2)
	_Check ("durn's handaxe", _Pick (r[0], ["Die", "Total", "Hit", "Damage", "Bash"]), [20, 15, 1, 7, 0])
	_Check ("durn's shield bash", _Pick (r[1], ["Die", "Natural", "Total", "Hit", "Damage", "Bash"]), [14, 9, 12, 1, 6, 1])
	_Check ("log: shield bash", _Logged ("Durn attacks (shield bash) Thug 1: d14 9 + d3 2 +1 = 12 vs AC 10, hit: 1d3 +3 = 6 damage"), True)
	yield
	_Check ("thug 1 took 13", _HP (thug1), hp - 13)
	# fumble 16+ in plate (d16): hits itself (1d6 3 + 1 + 1 + 1 = 6), and is down until a DC 16 Agility check
	r = _Attack (durn, thug1, [1, 1, 5, 16, 3])
	_Check ("durn fumble 16", _Pick (r[0], ["Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 16, 16, "hits itself and falls"])
	yield
	d = GemRB.GetDCCStats (durn)
	_Check ("durn down until a DC 16 Agi check", [GemRB.GetPlayerStat (durn, IE_DCC_PRONE), d["StandDC"]], [1, 16])
	GemRB.DCCForceDice (durn, [20])
	yield from _Wait (lambda: _Logged ("Durn tries to get up"), 40)
	GemRB.DCCForceDice (durn, [])
	_Check ("durn gets up on a 20", [_Logged ("Durn tries to get up: Agi check 20 = 20 vs DC 16: up"), GemRB.GetPlayerStat (durn, IE_DCC_PRONE)], [True, 0])

	# the thief's backstab against an unaware foe (Nim invisible, thug 2 not yet fighting): +3, and the hit is a
	# crit on Table II; the dagger's backstab damage is 1d10. 8 + 3 + 2 (invisible) = 13; 1d10 6; 1d10 6 on II:
	# speed and actions halved for 1d4 (2) rounds. Thug 2 is surprised: no action this round
	GemRB.ApplyEffect (nim, "State:Invisible", 0, 0)
	yield
	r = _Attack (nim, thug2, [8, 6, 6, 2])
	_Check ("nim backstabs", _Pick (r[0], ["Backstab", "Total", "Hit", "Critical", "CritTable", "CritRoll", "Damage", "Result"]), [1, 13, 1, 1, 2, 6, 6, "leg slashed"])
	_Check ("log: backstab", _Logged ("Nim backstabs Thug 2 (unaware): +3 to hit, a hit is a crit"), True)
	_Check ("log: surprise", _Logged ("Thug 2 is caught by surprise: no action this round"), True)
	yield
	_Check ("thug 2 surprised, half its actions", [GemRB.GetPlayerStat (thug2, IE_DCC_NOATTACK), GemRB.GetDCCStats (thug2)["ActionLimit"]], [1, 1])

	# grappling: Durn d20 15 + 1 (Str) = 16 against thug 1's d20 5 + 1 (HD) = 6: pinned (held)
	GemRB.DCCForceDice (durn, [15])
	GemRB.DCCForceDice (thug1, [5])
	_Check ("durn pins thug 1", GemRB.DCCGrapple (durn, thug1), 1)
	_Check ("log: grapple", _Logged ("Durn grapples Thug 1: 16 vs 6, pinned"), True)
	yield
	_Check ("thug 1 pinned by durn", [GemRB.GetPlayerStat (thug1, IE_DCC_PINNED), GemRB.GetPlayerStat (thug1, IE_HELD)], [durn, 1])
	# its escape next round: d20 20 + 1 against Durn's 1 + 1
	GemRB.DCCForceDice (durn, [1])
	GemRB.DCCForceDice (thug1, [20])
	# meanwhile Tank misses the pinned thug (helpless: d24 2 + 1 = 3): 1 on the d2, the blow goes to Durn,
	# d24 18 + 1 = 19 vs AC 10, 1d8 4 + 1 = 5
	hp = _HP (durn)
	r = _Attack (tank, thug1, [1, 2, 1, 18, 4])
	_Check ("tank's miss on a pinned foe", _Pick (r[0], ["Die", "Total", "Hit"]), [24, 3, 0])
	_Check ("strays to the holder", len (r) > 1 and _Pick (r[1], ["Target", "Free", "Hit", "Damage"]), [durn, 1, 1, 5])
	_Check ("log: stray blow", _Logged ("Tank's miss goes astray towards Durn"), True)
	yield
	_Check ("durn took 5", _HP (durn), hp - 5)
	yield from _Wait (lambda: _Logged ("Thug 1 struggles against Durn's hold"), 40)
	GemRB.DCCForceDice (durn, [])
	GemRB.DCCForceDice (thug1, [])
	_Check ("thug 1 breaks free", [_Logged ("Thug 1 struggles against Durn's hold: 21 vs 2: free"), GemRB.GetPlayerStat (thug1, IE_DCC_PINNED)], [True, 0])

	# firing into a melee: Arlo's dart at thug 3, Tank next to it (-1); a miss (2), 1 on the d2: at Tank,
	# d20 18 hits, 1d4 3 + deed 1 = 4
	r = _Attack (arlo, thug3, [1, 2, 1, 18, 3])
	_Log ("arlo's dart: %s" % (r[0] if r else None))
	_Check ("arlo's dart misses into the melee", r and _Pick (r[0], ["Hit", "Fumble"]), [0, 0])
	_Check ("arlo's dart strays to tank", len (r) > 1 and _Pick (r[1], ["Target", "Hit", "Damage"]), [tank, 1, 4])
	_Check ("log: dart astray", _Logged ("Arlo's miss goes astray towards Tank"), True)

	# a charge: declared, then a 20' run-up: +2 to hit and -2 AC for a round. d20 10 + d3 2 + 2 = 14
	ac = GemRB.GetDCCStats (tank)["AC"]
	GemRB.DCCSetCombat (tank, DCC_OPT_CHARGE)
	GemRB.ExecuteString ("JumpToPoint([1180.760])", tank)
	for i in range (3):
		yield
	r = _Attack (tank, thug3, [2, 10, 4])
	_Check ("tank charges", _Pick (r[0], ["Modifier", "Total", "Hit"]), [4, 14, 1])
	_Check ("log: charge", _Logged ("Tank charges Thug 3 (20' run-up): +2 to hit, -2 AC for a round"), True)
	yield
	_Check ("tank -2 AC, the charge used up", [GemRB.GetDCCStats (tank)["AC"], GemRB.GetDCCStats (tank)["CombatOptions"]], [ac - 2, 0])

	# withdrawal: back next to thug 3, a swing engages them; walking away gives thug 3 a free attack (15 + 0, 1d6 3)
	GemRB.ExecuteString ("JumpToPoint([1180.1000])", tank)
	for i in range (3):
		yield
	_Attack (tank, thug3, [1, 2])
	GemRB.DCCForceDice (thug3, [15, 3])
	GemRB.ExecuteString ("MoveToPoint([1180.800])", tank)
	yield from _Wait (lambda: _Logged ("Thug 3 gets a free attack: Tank withdraws"), 30)
	GemRB.DCCForceDice (thug3, [])
	_Check ("log: withdrawal", _Logged ("Thug 3 gets a free attack: Tank withdraws"), True)
	_Check ("log: the free attack", _Logged ("Thug 3 free attack on Tank: d20 15"), True)
	GemRB.ExecuteString ("JumpToPoint([1180.1000])", tank)
	for i in range (3):
		yield

	# Phase 3: a scripted move without an action of its own (a formation follow, a move after its
	# action was released) is voluntary too: walking out of reach gives thug 3 its free attack
	GemRB.ClearActions (tank)
	GemRB.ExecuteString ("JumpToPoint([1180.1000])", tank)
	for i in range (3):
		yield
	_Attack (tank, thug3, [1, 2])
	freeAttacks = _LogCount ("Thug 3 gets a free attack: Tank withdraws")
	GemRB.DCCForceDice (thug3, [15, 3])
	GemRB.DCCWalk (tank, 1180, 800)
	yield from _Wait (lambda: _LogCount ("Thug 3 gets a free attack: Tank withdraws") > freeAttacks, 30)
	GemRB.DCCForceDice (thug3, [])
	_Check ("scripted move without an action: withdrawal", _LogCount ("Thug 3 gets a free attack: Tank withdraws"), freeAttacks + 1)
	# displaced out of reach (a teleport here, a push or fling alike) and walking on: no free attack
	GemRB.ClearActions (tank)
	GemRB.ExecuteString ("JumpToPoint([1180.1000])", tank)
	for i in range (3):
		yield
	_Attack (tank, thug3, [1, 2])
	GemRB.ExecuteString ("JumpToPoint([1180.880])", tank)
	yield
	where = GemRB.GetActorPosition (tank)
	_Check ("tank displaced out of reach", [where["X"], where["Y"]], [1180, 880])
	GemRB.DCCWalk (tank, 1180, 760)
	for i in range (12):
		yield
	_Check ("displaced, then walking on: no withdrawal", _LogCount ("Thug 3 gets a free attack: Tank withdraws"), freeAttacks + 1)
	GemRB.ClearActions (tank)
	GemRB.ExecuteString ("JumpToPoint([1180.1000])", tank)
	for i in range (3):
		yield

	# battle rage (Crit Table III, row 0, Tank's Luck 3): he spends 2 (1 Per, 1 Int): +2d12; an ally is caught for 1d4
	GemRB.DCCSetCombat (tank, 0, 2)
	r = _Attack (tank, thug3, [1, 20, 3, 1, 4, 2, 5, 6])
	_Check ("tank crit III row 0", _Pick (r[0], ["Critical", "CritRoll", "Result"]), [1, -2, "wild frenzy catches an ally"])
	_Check ("log: battle rage", _Logged ("battle rage: Tank spends 1 Per and 1 Int, +2d12 = 11 damage"), True)
	yield
	t = GemRB.GetDCCStats (tank)
	_Check ("tank's Per and Int spent", [t["Abilities"][3][1], t["Abilities"][4][1]], [9, 9])

	# subdual blows: Vala's longsword a die lower (d7): 5 + deed 1 = 6 drops weakling 3 (3 hp): knocked out, not dead
	GemRB.DCCSetCombat (vala, DCC_OPT_SUBDUAL)
	r = _Attack (vala, weak3, [1, 15, 5])
	_Check ("vala's subdual blow", _Pick (r[0], ["Hit", "Subdual", "Damage"]), [1, 1, 6])
	_Check ("log: subdual", _Logged ("Vala attacks Weakling 3: d20 15 + d8 1 = 16 vs AC 10, hit: 1d7 +1 = 6 damage (deed 1: no trip) (subdual)"), True)
	yield
	w = GemRB.GetDCCStats (weak3)
	_Check ("weakling 3 knocked out", [_Dead (weak3), w["Subdued"], w["HP"], _Logged ("Weakling 3 is knocked out")], [False, 1, 0, True])
	GemRB.ApplyEffect (weak3, "CurrentHPModifier", 2, 0)
	yield
	_Check ("weakling 3 comes to", [GemRB.GetDCCStats (weak3)["Subdued"], _Logged ("Weakling 3 comes to")], [0, True])
	GemRB.DCCSetCombat (vala, 0)

	# cleave (Crit Table V, 21): d30 21, +3d12; the blow kills weakling 1, a free attack on weakling 2 (15 + 1, 1d8 5 + 1)
	r = _Attack (vala, weak1, [1, 20, 8, 21, 1, 1, 1, 15, 5])
	_Check ("vala crit V-21", _Pick (r[0], ["CritTable", "CritRoll", "Result", "Damage"]), [5, 21, "cleaving blow", 12])
	_Check ("cleave: a free attack on weakling 2", len (r) > 1 and _Pick (r[1], ["Target", "Free", "Hit", "Damage"]), [weak2, 1, 1, 6])
	_Check ("log: cleave", _Logged ("Vala gets a free attack: Vala cleaves on into Weakling 2"), True)
	yield from _Wait (lambda: _Dead (weak1), 20)
	_Check ("weakling 1 dead", _Dead (weak1), True)
	# decapitated (V-28): weakling 2 dies, Vala keeps attacking foes within 10' until a miss: weakling 4 (15 + 1)
	r = _Attack (vala, weak2, [1, 20, 2, 28, 15, 3])
	_Check ("vala crit V-28", _Pick (r[0], ["CritRoll", "Result"]), [28, "decapitated"])
	_Check ("log: keeps attacking", _Logged ("Vala gets a free attack: Vala keeps attacking"), True)
	_Check ("the rampage hits weakling 4", len (r) > 1 and _Pick (r[1], ["Target", "Hit"]), [weak4, 1])
	yield from _Wait (lambda: _Dead (weak2), 20)
	_Check ("weakling 2 dead", _Dead (weak2), True)

	# Crit Table DR (a 7 HD drake: DR/d20) on Ox: 20, double damage, Str and Agi drop to 1, a wound that bleeds every round
	r = _Attack (drake, ox, [20, 2, 20])
	_Check ("drake crit DR-20", _Pick (r[0], ["CritTable", "CritRoll", "Result", "Damage"]), [7, 20, "spines through the body", 4])
	yield
	o = GemRB.GetDCCStats (ox)
	_Check ("ox Str and Agi 1", [o["Abilities"][0][1], o["Abilities"][1][1]], [1, 1])
	GemRB.DCCForceDice (ox, [5])
	yield from _Wait (lambda: _Logged ("Ox's wound: 5 damage"), 40)
	GemRB.DCCForceDice (ox, [])
	_Check ("log: ox's wound bleeds", _Logged ("Ox's wound: 5 damage"), True)
	GemRB.ApplyEffect (ox, "CurrentHPModifier", 10, 0)
	yield
	_Check ("a heal closes the wound", [_Logged ("Ox's wound is closed"), "bleeding wound (until healed)" in GemRB.GetDCCStats (ox)["Injuries"]], [True, False])
	# DR-9: flung 3d6 (1+1+1) x 10' = 30' (the d360 says 180: west), +1d4 per 30' (2)
	r = _Attack (drake, ox, [20, 1, 9, 1, 1, 1, 180, 2])
	_Check ("drake crit DR-9", _Pick (r[0], ["CritRoll", "Result"]), [9, "tail-slung away"])
	_Check ("log: flung", _Logged ("flung 30', +2 damage"), True)
	# DR-8: +1d12, the weapon arm torn off for good: the weapon drops, and Ox fights with the other hand (-1d)
	r = _Attack (drake, ox, [20, 1, 8, 4])
	_Check ("drake crit DR-8", _Pick (r[0], ["CritRoll", "Result"]), [8, "arm ripped from its socket"])
	yield
	o = GemRB.GetDCCStats (ox)
	_Check ("ox's arm severed for good", [o["InjuryBits"] & DCC_INJ_WEAPON_ARM, "weapon arm severed (for good)" in o["Injuries"]], [DCC_INJ_WEAPON_ARM, True])
	r = _Attack (ox, thug2, [1, 5])
	_Check ("ox with one arm: -1d", r and r[0]["Die"], 16)

	# morale: orc 2 dies; orc 1 (d20 15) stands, orc 3 (d20 3) flees
	orc1, orc2, orc3, wolf = ids["orc1"], ids["orc2"], ids["orc3"], ids["wolf"]
	GemRB.DCCForceDice (orc1, [15])
	GemRB.DCCForceDice (orc3, [3])
	GemRB.ApplyEffect (orc2, "Death", 0, 4)
	yield
	GemRB.DCCForceDice (orc1, [])
	GemRB.DCCForceDice (orc3, [])
	_Check ("log: orc 1 stands", _Logged ("Orc 1 checks morale (the first of its side fell): d20 15 = 15 vs 11: stands"), True)
	_Check ("log: orc 3 flees", _Logged ("Orc 3 checks morale (the first of its side fell): d20 3 = 3 vs 11: flees"), True)
	_Check ("orc 3 panics", bool (GemRB.GetPlayerStat (orc3, IE_STATE_ID) & STATE_PANIC), True)
	# the leader (orc 1, 2 HD) falls: orc 3 checks again (d20 18)
	GemRB.DCCForceDice (orc3, [18])
	GemRB.ApplyEffect (orc1, "Death", 0, 4)
	yield
	GemRB.DCCForceDice (orc3, [])
	_Check ("log: orc 3 checks for its leader", _Logged ("Orc 3 checks morale (its leader fell): d20 18 = 18 vs 11: stands"), True)
	# a lone wolf at half hp (d20 2): flees
	GemRB.DCCForceDice (wolf, [2])
	GemRB.ApplyEffect (wolf, "Damage", 5, 0)
	yield
	GemRB.DCCForceDice (wolf, [])
	_Check ("log: the wolf alone and hurt", _Logged ("Wolf checks morale (alone and hurt): d20 2 = 2 vs 11: flees"), True)

	# the record screen's "Roll body over": Sly died of the death spell; Luck check 5 under 13, d3 2: -1 Agi
	import GUIREC
	sly = _Party ()["dctsly"]
	_Check ("sly's body can be rolled over", GemRB.GetDCCStats (sly)["BodyRecoverable"], 1)
	GUIREC.OpenRecordsWindow (sly)
	_Check ("roll body over button and a helper", [GUIREC.BodyButton is not None, GUIREC.BodyHelper (sly) > 0], [True, True])
	GemRB.DCCForceDice (sly, [5, 2])
	_Check ("roll sly's body over", GUIREC.RecoverBody (sly), 1)
	GemRB.DCCForceDice (sly, [])
	yield
	_Check ("sly back at 1 hp", [_Dead (sly), GemRB.GetDCCStats (sly)["HP"]], [False, 1])
	_Check ("no second roll", GemRB.GetDCCStats (sly)["BodyRecoverable"], 0)
	# the Combat panel: a charge for the next attack, battle rage 3
	import DCCLuck
	brak = _Party ()["dctbrak"]
	DCCLuck.OpenCombatWindow (brak)
	_Check ("combat panel open", DCCLuck.Kind, "combat")
	DCCLuck._SetCombat (DCC_OPT_CHARGE)
	DCCLuck._SetCombat (None, 3)
	b = GemRB.GetDCCStats (brak)
	_Check ("combat panel: charge, rage 3", [b["CombatOptions"], b["Rage"]], [DCC_OPT_CHARGE, 3])
	_Check ("record shows the combat options", "Combat: charge next, battle rage 3" in DCCSheet.Lines (brak), True)
	GUIREC.CloseRecordsWindow ()
	GemRB.DCCSetCombat (brak, 0, 0)
	yield

def _FightDemons ():
	"""Funnel prep (M2.9): Crit Table DN's CORRUPT from real crits, with forced dice"""
	demon1, demon2, mord = _ID ("dctdemn1"), _ID ("dctdemn2"), _ID ("dctmord")
	_Check ("demons and their target", all ([demon1, demon2, mord]), True)
	d1, d2 = GemRB.GetDCCStats (demon1), GemRB.GetDCCStats (demon2)
	# Table 9-10: a 5 HD demon crits on DN with a d6, a 9 HD one with a d10
	_Check ("demon crit dice", [d1["CritTable"], d1["CritDie"], d2["CritTable"], d2["CritDie"]], [8, 6, 8, 10])
	before = len (GemRB.GetDCCMagic (mord)["Corruptions"])
	# a natural 20 crits; d6 5 on Crit Table DN: corruption; 1d10 4 + Luck 0 on the minor table (up to 5 HD)
	r = _Attack (demon1, mord, [20, 2, 5, 4])
	_Check ("demon crit DN-5", _Pick (r[0], ["Critical", "CritRoll", "Result"]), [1, 5, "corruption"])
	corruptions = GemRB.GetDCCMagic (mord)["Corruptions"]
	_Check ("a minor corruption, row 4", [(c["Table"], c["Row"]) for c in corruptions[before:]], [(1, 4)])
	# d10 10: double corruption, on the major table (6-10 HD): rows 3 and 7
	r = _Attack (demon2, mord, [20, 2, 10, 3, 7])
	_Check ("demon crit DN-10", _Pick (r[0], ["Critical", "CritRoll", "Result"]), [1, 10, "double corruption"])
	corruptions = GemRB.GetDCCMagic (mord)["Corruptions"]
	_Check ("two major corruptions, rows 3 and 7", [(c["Table"], c["Row"]) for c in corruptions[before + 1:]], [(2, 3), (2, 7)])
	_Check ("log: the demon's corruption", _Logged ("Mord is corrupted"), True)
	yield

def _Fight ():
	GemRB.GetDCCCombatLog (1)
	korg, vex, pip, eda, brun, peas = [_ID (n) for n in ("dctkorg", "dctvex", "dctpip", "dcteda", "dctbrun", "dctpeas")]
	gob1, gob2 = _ID ("dctgob1"), _ID ("dctgob2")
	_Check ("combatants in the area", all ([korg, vex, pip, eda, brun, peas, gob1, gob2]), True)
	# the M2.3 checks run without Luck burns (Korg, a warrior, would cancel his fumble by default)
	GemRB.DCCSetLuckPolicy (korg, DCC_BURN_NEVER, 10, DCC_LUCK_PAUSE | DCC_LUCK_ALLIES)

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

	yield from _FightLuck ()
	yield from _FightCompletion ()
	yield from _FightDemons ()

	# Korg fumbles: leather's d8, 7 on Table 4-2 drops the weapon
	r = _Attack (korg, gob2, [1, 1, 7])
	_Check ("korg fumble", _Pick (r[0], ["Fumble", "FumbleDie", "FumbleRoll", "Result"]), [1, 8, 7, "drops its weapon"])
	_Check ("log: fumble", _Logged ("Fumble (d8 7 = 7): drops its weapon; Korg drops its weapon"), True)

	for line in GemRB.GetDCCCombatLog ():
		_Log ("combat: " + line)

	yield from _Magic_ ()


# --- M2.5 spellcasting ----------------------------------------------------------

STATE_SLEEP = 0x20
DCC_INJ_AMNESIA = 64

def _Magic (actor):
	return GemRB.GetDCCMagic (actor)

def _Spell (actor, name):
	for spell in _Magic (actor)["Spells"]:
		if spell["Spell"] == name:
			return spell
	return None

def _Cast (caster, spell, target, dice, burn = None, situational = 0):
	GemRB.DCCForceDice (caster, dice)
	if burn is None:
		r = GemRB.DCCCast (caster, spell, target, None, situational)
	else:
		r = GemRB.DCCCast (caster, spell, target, burn, situational)
	return r

def _StepCheckMagic ():
	"""before the save: Mirela (wizard 1, Int 17 +2, Luck 6 -1) learns, spellburns, loses a spell, is corrupted"""
	pc = _Party ()["dctmirel"]
	m = _Magic (pc)
	_Check ("dctmirel caster", [m["CasterKind"], m["CasterLevel"], m["MaxSpell"], m["ArmorPenalty"]], [1, 1, 1, 0])
	# mercurial magic: d% 60 + (-1 x 10) = 50, no change; the manifestation d10 3: a force arrow
	GemRB.DCCForceDice (pc, [60, 3])
	_Check ("dctmirel learns magic missile: mercurial 50", GemRB.DCCLearnSpell (pc, "dwmmis"), 50)
	GemRB.DCCForceDice (pc, [100, 2])
	_Check ("dctmirel learns sleep: d% 100 - 10 = 90", GemRB.DCCLearnSpell (pc, "dwslee"), 90)
	GemRB.DCCForceDice (pc, [])
	_Check ("dctmirel can't learn a spell twice", GemRB.DCCLearnSpell (pc, "dwmmis"), -1)
	spell = _Spell (pc, "dwmmis")
	_Check ("dctmirel magic missile", spell and [spell["Mercurial"], spell["Manifestation"], spell["ManifestationText"], spell["Lost"]], [[50], 3, "force arrow", 0])
	_Check ("log: learns magic missile", _Logged ("Mirela learns Magic Missile (it shows as force arrow); mercurial magic 50: no change"), True)
	# a spellburned cast: d24 flavour 5, d20 12 + 1 CL + 2 Int + 1 Str = 16: one missile (14-17)
	r = _Cast (pc, "dwmmis", _ID ("dcttgt4"), [5, 12, 3], [1, 0, 0])
	_Check ("dctmirel spellburned cast", _Pick (r, ["Natural", "Total", "BandMin", "Burn", "BurnBonus", "Success", "Lost"]), [12, 16, 14, 1, 1, 1, 0])
	_Check ("log: spellburn", _Logged ("Mirela spellburns 1 (1 Str): pulls out and burns some hair"), True)
	_Check ("log: the check", _Logged ("Mirela casts Magic Missile: d20 12 +1 CL +2 Int +1 spellburn = 16: 14-17: one missile, 1d4+CL damage"), True)
	m = _Magic (pc)
	_Check ("dctmirel spellburn points", [m["Spellburn"]["Points"], m["Spellburn"]["Flavour"]], [[1, 0, 0], 5])
	_Check ("dctmirel Str after the burn", GemRB.GetDCCStats (pc)["Abilities"][0][:3], [8, 7, 1])
	# sleep lost for the day, and a major corruption (row 9: horns) the policy doesn't burn Luck for
	GemRB.DCCSetSpellState (pc, "dwslee", 1 | 4, [90])
	GemRB.DCCSetSpellburn (pc, -1, -1, 0)
	_Check ("dctmirel major corruption row 9", GemRB.DCCCorrupt (pc, 2, 1, 9, 0), 9)
	m = _Magic (pc)
	_Check ("dctmirel corruption", [[c["Table"], c["Row"]] for c in m["Corruptions"]], [[2, 9]])
	_Check ("dctmirel horns", "horns" in m["MutationNames"], True)
	_Check ("dctmirel tinted", m["Tint"] != 0, True)
	_Check ("dctmirel sleep lost", _Spell (pc, "dwslee")["Lost"], 1)

def _CheckMagicLoaded ():
	pc = _Party ()["dctmirel"]
	m = _Magic (pc)
	_Check ("loaded dctmirel spells", [[s["Spell"], s["Mercurial"], s["Manifestation"], s["Lost"]] for s in m["Spells"]],
		[["dwmmis", [50], 3, 0], ["dwslee", [90], 2, 1]])
	_Check ("loaded dctmirel corruption", [[c["Table"], c["Row"], c["Spell"]] for c in m["Corruptions"]], [[2, 9, ""]])
	_Check ("loaded dctmirel spellburn", [m["Spellburn"]["Points"], m["Spellburn"]["Policy"], m["Spellburn"]["Corruption"], m["Spellburn"]["LastDay"] > 0],
		[[1, 0, 0], 1, 0, True])
	_Check ("loaded dctmirel mutations", "horns" in m["MutationNames"], True)
	_Check ("loaded dctmirel Str loss", GemRB.GetDCCStats (pc)["Abilities"][0][:3], [8, 7, 1])

def _CheckMagicRested (bed):
	pc = _Party ()["dctmirel"]
	m = _Magic (pc)
	_Check ("%s dctmirel sleep back" % ("bed rest" if bed else "field rest"), _Spell (pc, "dwslee")["Lost"], 0)
	# spellburn: nothing back the first night (she burned that day), a point the next
	points = [0, 0, 0] if bed else [1, 0, 0]
	_Check ("%s dctmirel spellburn points" % ("bed rest" if bed else "field rest"), m["Spellburn"]["Points"], points)
	_Check ("%s dctmirel Str" % ("bed rest" if bed else "field rest"), GemRB.GetDCCStats (pc)["Abilities"][0][:3], [8, 8, 0] if bed else [8, 7, 1])

def _Sweep (caster, spell, target, mod, bands, ally = False):
	"""casts spell once per band with a forced natural (and a situational modifier past 20), checks the band"""
	base = spell
	for index, low in enumerate (bands):
		natural = max (2, min (19, low - mod))
		situational = low - mod - natural
		r = _Cast (caster, spell, target, [natural], None, situational)
		letter = "abcdefghijk"[index + (2 if bands[0] >= 12 and spell.startswith ("dw") else (1 if spell.startswith ("dc") else 0))]
		band = base + letter + ("2" if ally else "1")
		_Check ("%s %s band %d%s" % (GemRB.GetPlayerName (caster, 1), spell, low, " (ally)" if ally else ""),
			_Pick (r, ["Total", "BandMin", "Success", "Band"]), [low, low, 1, band])
		GemRB.DCCForceDice (caster, [])

def _Magic_ ():
	"""M2.5 in play: the starter spells with forced dice"""
	zed, elf, clem, hale = _ID ("dctzed"), _ID ("dctelf"), _ID ("dctclem"), _ID ("dcthurt")
	tgt1, tgt2, tgt3, tgt4 = [_ID ("dcttgt%d" % i) for i in range (1, 5)]
	skel1, skel2 = _ID ("dctskel1"), _ID ("dctskel2")
	_Check ("M2.5 casters and targets in the area", all ([zed, elf, clem, hale, tgt1, tgt2, tgt3, tgt4, skel1, skel2]), True)
	m = _Magic (zed)
	_Check ("dctzed caster", [m["CasterKind"], m["CasterLevel"], m["MaxSpell"], [s["Spell"] for s in m["Spells"]]], [1, 1, 1, ["dwmmis", "dwslee", "dwflam"]])
	m = _Magic (elf)
	_Check ("dctelf caster in leather", [m["CasterKind"], m["CasterLevel"], m["ArmorPenalty"]], [1, 1, -1])
	m = _Magic (clem)
	_Check ("dctclem caster", [m["CasterKind"], m["CasterLevel"], m["MaxSpell"], m["Disapproval"]], [2, 1, 1, 1])
	GemRB.GetDCCCombatLog (True)

	# every band of the six spells (Zed: CL 1 + Int 2; Clem: CL 1 + Per 2)
	_Sweep (zed, "dwmmis", tgt1, 3, [12, 14, 18, 20, 24, 28, 30, 32])
	_Sweep (zed, "dwslee", tgt2, 3, [12, 14, 18, 20, 24, 28, 30, 32])
	_Sweep (zed, "dwflam", tgt3, 3, [12, 14, 18, 20, 24, 28, 30, 32])
	# (Blessing's sweeps come last: its auras would bless everyone near for the other checks)
	_Sweep (clem, "dcpara", tgt4, 3, [12, 14, 18, 20, 24, 28, 30, 32])
	_Sweep (clem, "dcword", tgt4, 3, [12, 14, 18, 20, 24, 28, 30, 32])
	_Check ("dctclem range after successes", _Magic (clem)["Disapproval"], 1)
	GemRB.DCCSetSpellburn (zed, -1, -1, 0)
	for i in range (8):
		yield

	# the band's own dice: 18-19's 1d4 missiles (2), each 1d4 + 1 (4 and 1): 7 damage on target 2
	hp = _HP (tgt2)
	r = _Cast (zed, "dwmmis", tgt2, [15, 2, 4, 1])
	_Check ("magic missile 18-19", _Pick (r, ["Total", "BandMin", "Repeat"]), [18, 18, 1])
	yield from _Wait (lambda: _HP (tgt2) <= hp - 7, 30)
	_Check ("two missiles hit target 2 for 7", hp - _HP (tgt2), 7)
	# a natural 20: + CL again, 4d12 + 1
	hp = _HP (tgt3)
	r = _Cast (zed, "dwmmis", tgt3, [20, 12, 12, 12, 12])
	_Check ("magic missile critical", _Pick (r, ["Natural", "Total", "Critical", "BandMin"]), [20, 24, 1, 24])
	_Check ("log: critical", _Logged ("Zed casts Magic Missile: d20 20 +1 CL +2 Int +1 critical = 24: 24-27: one great missile, 4d12+CL"), True)
	yield from _Wait (lambda: _HP (tgt3) <= hp - 49, 30)
	_Check ("the great missile hits for 49", hp - _HP (tgt3), 49)

	# the save DC is the check total: sleep 14-17 on target 1 and the nearest foe (2 targets);
	# target 1 rolls 3 (fails), target 2 rolls 19 (saves); the band's duration 1d6 turns (2)
	GemRB.DCCForceDice (tgt1, [3])
	GemRB.DCCForceDice (tgt2, [19])
	r = _Cast (zed, "dwslee", tgt1, [12, 2])
	_Check ("sleep 14-17", _Pick (r, ["Total", "BandMin", "Success"]), [15, 14, 1])
	yield from _Wait (lambda: _Logged ("Target 1: Willpower save: 3 +0 = 3 vs DC 15: failed"), 20)
	_Check ("log: target 1's save vs DC 15", _Logged ("Target 1: Willpower save: 3 +0 = 3 vs DC 15: failed"), True)
	_Check ("target 1 asleep", bool (GemRB.GetPlayerStat (tgt1, IE_STATE_ID) & STATE_SLEEP), True)
	GemRB.DCCForceDice (tgt1, [])
	GemRB.DCCForceDice (tgt2, [])

	# spell loss: 2-11 loses the spell; casting it again needs spellburn of its level (Zed: never by policy)
	r = _Cast (zed, "dwflam", tgt3, [5])
	_Check ("flaming hands 2-11: lost", _Pick (r, ["Total", "BandMin", "Success", "Lost"]), [8, 2, 0, 1])
	_Check ("flaming hands lost for the day", _Spell (zed, "dwflam")["Lost"], 1)
	r = _Cast (zed, "dwflam", tgt3, [15])
	_Check ("a lost spell won't cast", _Pick (r, ["Band", "Success"]), ["", 0])
	r = _Cast (zed, "dwflam", tgt3, [7, 10, 2], [1, 0, 0])
	_Check ("a lost spell with 1 point of spellburn", _Pick (r, ["Burn", "BurnBonus", "Total", "BandMin", "Success"]), [1, 0, 13, 12, 1])
	_Check ("log: spellburn for the lost spell", _Logged ("Zed spellburns 1 (1 Str, 1 for the lost spell)"), True)
	# 20 points of spellburn count as a natural 20
	r = _Cast (zed, "dwmmis", tgt4, [9, 4], [8, 6, 6])
	_Check ("20 points of spellburn: a natural 20", _Pick (r, ["Natural", "Critical", "Burn", "BurnBonus", "Total"]), [20, 1, 20, 20, 44])
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR, 0)
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR + 1, 0)
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR + 2, 0)
	yield

	# a natural 1: magic missile's 1d6 + Luck 0 = 2: corruption, d8 5: the eyes go chalk white
	r = _Cast (zed, "dwmmis", tgt4, [1, 2, 5])
	_Check ("magic missile natural 1", _Pick (r, ["NatOne", "BandMin", "Lost", "NatOneOutcome", "CorruptionTable", "CorruptionRow", "CorruptionCancelled"]),
		[1, 1, 1, 1, 0, 5, 0])
	m = _Magic (zed)
	_Check ("zed corrupted: white eyes", [[c["Table"], c["Row"], c["Spell"]] for c in m["Corruptions"]], [[0, 5, "dwmmis"]])
	_Check ("zed's strange eyes", "strange eyes" in m["MutationNames"], True)
	_Check ("log: corruption", _Logged ("Zed is corrupted: the eyes go chalk white (corruption)"), True)
	# sleep's natural 1: 1d6 1 = corruption, d6 5 = major corruption, 1d10 10 - 1 + 0 = 9: horns
	r = _Cast (zed, "dwslee", tgt4, [1, 1, 5, 10])
	_Check ("sleep natural 1: major corruption", _Pick (r, ["NatOneOutcome", "CorruptionTable", "CorruptionRow"]), [1, 2, 9])
	# with "always burn Luck": flaming hands' natural 1, 1d6 1, d4 3 (its hair burns away) is cancelled
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR, 0)
	GemRB.DCCSetSpellState (zed, "dwflam", 0)
	GemRB.DCCSetSpellburn (zed, -1, -1, 3)
	luck = _Luck (zed)
	r = _Cast (zed, "dwflam", tgt4, [1, 1, 3])
	_Check ("flaming hands natural 1: Luck cancels it", _Pick (r, ["CorruptionRow", "CorruptionCancelled"]), [3, 1])
	_Check ("zed burned 1 Luck", _Luck (zed), luck - 1)
	_Check ("zed's corruptions", len (_Magic (zed)["Corruptions"]), 2)
	GemRB.DCCSetSpellburn (zed, -1, -1, 0)
	# a misfire: the elf (Luck +2) rolls a natural 1, 1d6 2 + 2 = 4: misfire, d6 2: the missiles ricochet, 1d3 3 - 1 = 2
	hp = _HP (elf)
	r = _Cast (elf, "dwmmis", tgt4, [1, 2, 2, 3])
	_Check ("elf natural 1: misfire", _Pick (r, ["NatOneOutcome", "MisfireRoll"]), [4, 2])
	_Check ("the ricochet hurts the elf for 2", hp - _HP (elf), 2)
	_Check ("log: misfire", _Logged ("Lirael's Magic Missile misfires (2): the missiles ricochet into the caster; Lirael takes 2 damage"), True)
	yield

	# mercurial magic: the elf (Luck +2) learns sleep: d% 50 + 20 = 70
	GemRB.DCCForceDice (elf, [50, 1])
	_Check ("elf learns sleep: d% 50 + 20 = 70", GemRB.DCCLearnSpell (elf, "dwslee"), 70)
	GemRB.DCCForceDice (elf, [])
	# the elf's armor (-1) and its lucky spell (+2, frozen): d20 10 + 1 + 1 - 1 + 2 = 13
	GemRB.DCCSetSpellState (elf, "dwmmis", 2, None, 2)
	r = _Cast (elf, "dwmmis", tgt4, [10])
	_Check ("elf in leather, lucky spell", _Pick (r, ["Total", "BandMin"]), [13, 12])
	_Check ("log: elf's check", _Logged ("Lirael casts Magic Missile: d20 10 +1 CL +1 Int -1 armor +2 Luck = 13"), True)
	# 96: the die a step up (d24), a natural 21 crits
	GemRB.DCCSetSpellState (zed, "dwflam", 4, [96])
	r = _Cast (zed, "dwflam", tgt3, [21, 3])
	_Check ("mercurial 96: d24", _Pick (r, ["Die", "Natural", "Critical", "Total"]), [24, 21, 1, 25])
	# 12: -4 to spell checks for 1d4 rounds after the cast (sleep 12-13, 1d6 turns 1, then 1d4 2)
	GemRB.DCCSetSpellState (zed, "dwslee", 4, [12])
	r = _Cast (zed, "dwslee", tgt4, [10, 1, 2])
	_Check ("mercurial 12 after the cast", _Magic (zed)["SpellMod"], -4)
	_Check ("log: mercurial 12", _Logged ("Zed's mercurial magic (12): -4 to spell checks for 1d4 rounds"), True)
	yield

	# disapproval (Clem: CL 1 + Per 2, Luck 0): a natural 1, 1d4 3: must win a convert, -1 until tomorrow
	r = _Cast (clem, "dcbles", clem, [1, 3])
	_Check ("clem natural 1: disapproval", _Pick (r, ["Disapproval", "DisapprovalRoll", "RangeAfter", "Success"]), [1, 3, 2, 0])
	_Check ("clem -1 to spell checks", _Magic (clem)["SpellMod"], -1)
	# a natural 2 is in the range now: 2d4 4 + 4 = 8: -4 with this spell until tomorrow
	r = _Cast (clem, "dcbles", clem, [2, 4, 4])
	_Check ("clem natural 2 in the range", _Pick (r, ["Disapproval", "DisapprovalRoll", "RangeAfter"]), [1, 8, 3])
	_Check ("blessing's own penalty", _Spell (clem, "dcbles")["Penalty"], 1)
	# a plain failure widens it too: 6 + 3 - 1 = 8
	r = _Cast (clem, "dcpara", tgt4, [6])
	_Check ("clem fails: the range widens", _Pick (r, ["Total", "Disapproval", "RangeAfter"]), [8, 0, 4])
	_Check ("log: disapproval", _Logged ("Clem rolls on the disapproval table: 1d4 3 = 3: must win a convert: -1 to spell checks until then"), True)
	# atonement: 100 gp narrows it by 2
	_Check ("clem atones with 100 gp", GemRB.DCCAtone (clem, 100), 2)
	_Check ("clem's range and gold", [_Magic (clem)["Disapproval"], GemRB.GetPlayerStat (clem, IE_GOLD)], [2, 100])

	# lay on hands on Hale (lawful, level 1 warrior: d12, 1 die at most): 12 + 3 - 1 = 14: 3 dice, capped at 1
	hp = _HP (hale)
	GemRB.DCCForceDice (clem, [7])
	_Check ("clem lays on hands", GemRB.DCCLayOnHands (clem, hale, 12), 7)
	GemRB.DCCForceDice (clem, [])
	_Check ("hale healed 7", _HP (hale) - hp, 7)
	_Check ("log: lay on hands", _Logged ("Clem heals Hale: 1d12 (same alignment) = 7 hp"), True)
	# turn unholy: 15 + 3 + 0 Luck - 1 = 17: 1 HD: 1d3 (2) + 1 turned; both skeletons fail their Will saves
	GemRB.DCCForceDice (skel1, [1])
	GemRB.DCCForceDice (skel2, [1])
	GemRB.DCCForceDice (clem, [2, 3, 3, 3, 4, 4, 4])
	_Check ("clem turns two skeletons", GemRB.DCCTurnUnholy (clem, 15), 2)
	GemRB.DCCForceDice (clem, [])
	_Check ("skeleton 1 flees", bool (GemRB.GetPlayerStat (skel1, IE_STATE_ID) & STATE_PANIC), True)
	_Check ("log: turning", _Logged ("Clem turns the unholy: d20 15 +1 CL +2 Per -1 = 17: success"), True)
	yield

	# mercurial 12's -4 runs out first
	yield from _Wait (lambda: _Magic (zed)["SpellMod"] == 0, 80)
	_Check ("mercurial 12's -4 ran out", _Magic (zed)["SpellMod"], 0)
	# the party: Zed takes dead Mirela's place, so his casts ask (spellburn) and offer Luck (corruption)
	mirela = _Party ().get ("dctmirel")
	if mirela:
		GemRB.LeaveParty (mirela)
	GemRB.ExecuteString ("JoinParty()", zed)
	yield from _Wait (lambda: "dctzed" in _Party (), 20)
	_Check ("zed joined the party", "dctzed" in _Party (), True)
	GemRB.DCCSetSpellState (zed, "dwmmis", 4, [])
	GemRB.DCCSetSpellburn (zed, 1, 2, 0)
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR, 0)
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR + 1, 0)
	GemRB.SetPlayerStat (zed, IE_DCC_LOSS_STR + 2, 0)
	points = _Magic (zed)["Spellburn"]["Points"]
	before = _Magic (zed)["Revision"]
	# a real SpellRES cast: the prompt before the check (the preset 2 from Str), answered 1 Str 1 Sta;
	# then d24 4, d20 10 + 3 + 2 = 15, 1d4 3
	GemRB.DCCForceDice (zed, [4, 10, 3])
	GemRB.ExecuteString ('SpellRES("dwmmis","dcttgt4")', zed)
	yield from _Wait (lambda: GemRB.GetPrompt () is not None, 20)
	prompt = GemRB.GetPrompt ()
	_Check ("spellburn prompt", prompt and [prompt["Tag"], [f["Value"] for f in prompt["Fields"]], [f["Max"] for f in prompt["Fields"]]],
		["spellburn", [2, 0, 0], [13, 11, 11]])
	if prompt:
		_Log ("prompt: " + prompt["Text"])
		GemRB.AnswerPrompt (prompt["ID"], [1, 0, 1])
	yield from _Wait (lambda: GemRB.GetDCCLastCast (zed)["Spell"] == "dwmmis" and _Magic (zed)["Revision"] != before, 40)
	r = GemRB.GetDCCLastCast (zed)
	_Check ("the cast after the prompt", _Pick (r, ["Burn", "BurnBonus", "Total", "BandMin"]), [2, 2, 15, 14])
	_Check ("zed's spellburn", _Magic (zed)["Spellburn"]["Points"], [points[0] + 1, points[1], points[2] + 1])
	GemRB.DCCForceDice (zed, [])
	# the corruption's Luck offer goes to the owner: yes burns a point, no keeps the corruption
	GemRB.DCCSetSpellburn (zed, 0, 0, 0)
	luck = _Luck (zed)
	r = _Cast (zed, "dwmmis", tgt4, [1, 2, 6])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("corruption prompt", prompt and prompt["Tag"], "corruption")
	if prompt:
		_Log ("prompt: " + prompt["Text"])
		GemRB.AnswerPrompt (prompt["ID"], [1])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 10)
	yield
	_Check ("burned 1 Luck to avoid it", [_Luck (zed), len (_Magic (zed)["Corruptions"])], [luck - 1, 2])
	GemRB.DCCSetSpellState (zed, "dwmmis", 4, [])
	r = _Cast (zed, "dwmmis", tgt4, [1, 2, 8])
	yield
	prompt = GemRB.GetPrompt ()
	if prompt:
		GemRB.AnswerPrompt (prompt["ID"], [0])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 10)
	yield
	m = _Magic (zed)
	_Check ("declined: the force stone stays", [len (m["Corruptions"]), "an orbiting stone" in m["MutationNames"]], [3, True])
	GemRB.DCCForceDice (zed, [])

	# the record screen and the spellbook window
	import DCCMagic, DCCSheet, GUIREC
	slot = _Party ()["dctzed"]
	lines = DCCSheet.Lines (slot)
	_Check ("sheet: zed's spells", any ("Magic Missile (1)" in line for line in lines), True)
	_Check ("sheet: zed's horns and what they do", any ("horns start to grow (major): cosmetic" in line for line in lines), True)
	_Check ("sheet: the white eyes", any ("the eyes go chalk white (the spell's own)" in line for line in lines), True)
	GUIREC.OpenRecordsWindow (slot)
	win = DCCMagic.OpenSpellsWindow (slot)
	_Check ("spellbook window", win is not None, True)
	DCCMagic.SetSpellburn (2)
	_Check ("spellbook: preset policy", _Magic (zed)["Spellburn"]["Policy"], 2)
	DCCMagic.SetSpellburn (-1, 3)
	_Check ("spellbook: preset 3", _Magic (zed)["Spellburn"]["Preset"], 3)
	DCCMagic.SetSpellburn (0, 0)
	DCCMagic.OpenSpellsWindow (clem)
	_Check ("clem's sheet lines", any ("Disapproval range 1-2" in line for line in DCCMagic.Lines (clem)), True)
	DCCMagic.Close ()
	GUIREC.CloseRecordsWindow ()
	yield

	# amnesia (a crit injury): no spells at all
	GemRB.ApplyEffect (zed, "DCC:Combat", DCC_INJ_AMNESIA, COMBAT_INJURY | COMBAT_UNTIL_HEALED, "", "", "", "", AFTER_BONUSES)
	yield
	r = _Cast (zed, "dwslee", tgt4, [15])
	_Check ("amnesia: no casting", [r["Band"], r["Success"], "amnesia" in " ".join (r["Notes"])], ["", 0, True])

	# Blessing's every band, on the cleric and on an ally (Clem still has disapproval's -1 until
	# tomorrow; blessing's own -4 is lifted)
	GemRB.DCCSetSpellState (clem, "dcbles", 0)
	_Sweep (clem, "dcbles", clem, 2, [12, 14, 18, 20, 24, 28, 30, 32])
	_Sweep (clem, "dcbles", hale, 2, [12, 14, 18, 20, 24, 28, 30, 32], True)
	yield
	_Check ("hale blessed", GemRB.GetPlayerStat (hale, IE_DCC_ATKMOD) > 0 or GemRB.GetPlayerStat (hale, IE_DCC_ROLLMOD) > 0, True)

	for line in GemRB.GetDCCCombatLog ():
		if "cast" in line or "spell" in line or "corrupt" in line or "Luck" in line or "disapproval" in line:
			_Log ("magic: " + line)

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
		import traceback
		Failures += 1
		_Log ("FAIL step %s raised %s: %s" % (Step, type (error).__name__, error))
		for line in traceback.format_exc ().splitlines ():
			_Log ("  " + line)
		Step = "done"

def OnGameStart ():
	"""MessageWindow.OnLoad: (re)starts the ticks; a load in the middle continues"""
	global Token, Step
	Token += 1
	if Step == "loading":
		Step = "loaded"
	token = Token
	GemRB.SetTimer (lambda: _Tick (token), 500, -1)
