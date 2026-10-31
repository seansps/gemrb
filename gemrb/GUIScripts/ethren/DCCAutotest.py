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
	_StepCheckLuck ()

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

def _CheckLuckLoaded ():
	pc, brak = _Stats ("dctbrak")
	_Check ("loaded dctbrak Luck settings", [brak["LuckPolicy"], brak["LuckThreshold"], brak["LuckFlags"]], [DCC_BURN_MISS_TO_HIT, 8, DCC_LUCK_PAUSE])
	_Check ("loaded dctbrak deeds", [brak["Deed"], brak["NextDeed"]], [DCC_DEED_PUSH, DCC_DEED_BLIND])
	_Check ("loaded dctbrak burned Luck", [brak["Luck"], brak["LuckMax"], brak["Abilities"][5][2]], [11, 14, 3])
	pc, sly = _Stats ("dctsly")
	_Check ("loaded dctsly burned Luck", [sly["Luck"], sly["LuckMax"]], [12, 15])

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
