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
	return "done"

Steps = {
	"check": _StepCheck,
	"drained": _StepDrained,
	"save": _StepSave,
	"loaded": _StepLoaded,
	"rested": _StepRested,
	"bedrested": _StepBedRested,
	"lethal": _StepLethal,
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
