# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The funnel readiness checks (M2.9) of the funneltest autotest (testdata/funneltest,
# scripts/run.sh funneltest-auto). Nothing runs unless the game's FNT_AUTOTEST global is 1, which
# only the autotest variant of the FNT01 area script sets once the party of 16 has formed. Then a
# step a tick (500 ms):
#   1. the party: 16 level-0 peasants (MaxPartySize 16), each with its occupation (OCCU) and its
#      place in the funnel (FUNL); Player16 as a script object
#   2. the roster: 16 slots shown, select one, toggle, select all
#   3. a formation move of all 16 (GameControlMoveSelected): everyone arrives, none stuck, no two
#      on the same spot; again in a column
#   4. withdrawal on a formation move and a scripted move (no action of its own): the giant rat's
#      free attack
#   5. the level-1 class picks with forced dice: a wizard (Table 5-8 rows without content
#      substituted, slots left open), an elf (its lucky spell through the learn window), a cleric
#      (the god's list), a low-Int wizard swapping one of two (the optional rule) and refused a second;
#      spells known enforced
#   6. the prompt load: level-0 saves vs death follow the policy without asking; the host's
#      level-0 option makes them ask; auto-resolve answers with the policy; two at once wait in
#      line, one window at a time; the party settings panel sets every character's policy
#   7. travel: the whole party of 16 to the cave (FNT02) and back
#   8. save and load: 16 party members with their OCCU/FUNL records (a death's time and place
#      included), spells and lucky spell
# Every check logs "[FNT] ok ..." or "[FNT] FAIL ...", the end "[FNT] autotest done: N checks,
# F failed"; FNT_AT_DONE becomes 1 if all passed, 2 if not.

import GemRB
from GUIDefines import *
from ie_stats import *

STATE_DEAD = 0x800
SaveName = "funneltest"

Step = "wait"
Token = 0
Checks = 0
Failures = 0
Steps = None
Saved = {} # what the save should bring back

# the sixteen in roster order: script name, Table 1-3 occupation, race, the skill tags
PEASANTS = [
	("protagon", 39, 1, ["ANIMALS", "FARMING", "OUTDOORS"]),
	("fntmira", 97, 1, ["ALCHEMY", "LORE_ARCANE", "LETTERS"]),
	("fntlael", 37, 2, ["LORE_RELIGION", "LORE_ARCANE", "LETTERS"]),
	("fnttobb", 65, 1, ["HERBALISM", "MEDICINE", "LORE_RELIGION"]),
	("fntgorm", 23, 4, ["MASONRY", "MINING", "DIGGING"]),
	("fntpell", 58, 5, ["CRAFT_FINE", "HAGGLE"]),
	("fntbrin", 8, 1, ["SMITHING", "APPRAISAL"]),
	("fntodo", 98, 1, ["CARPENTRY", "OUTDOORS", "CLIMB"]),
	("fntsash", 53, 1, ["STEALTH_STREET", "DECEPTION"]),
	("fntwyn", 88, 1, ["MILITARY"]),
	("fnthild", 11, 1, ["HAGGLE", "COOKING"]),
	("fntrolf", 51, 1, ["LORE_RELIGION", "DIGGING"]),
	("fntessa", 84, 1, ["LORE_ARCANE", "LETTERS"]),
	("fntcorb", 89, 1, ["ANIMALS", "NOBILITY", "MILITARY"]),
	("fntmags", 66, 1, ["HERBALISM", "MEDICINE"]),
	("fntpip", 94, 1, ["STEALTH_STREET", "CLIMB"]),
]

def _Log (text):
	print ("[FNT] " + text)

def _Check (what, got, expected):
	global Checks, Failures
	Checks += 1
	if got == expected:
		_Log ("ok %s = %s" % (what, got))
	else:
		Failures += 1
		_Log ("FAIL %s = %s, expected %s" % (what, got, expected))

def _Slot (name):
	for pc in range (1, GemRB.GetPartySize () + 1):
		if GemRB.GetPlayerName (pc, 2).lower () == name:
			return pc
	return 0

def _Dead (pc):
	return bool (GemRB.GetPlayerStat (pc, IE_STATE_ID) & STATE_DEAD)

def _Luck (pc):
	return GemRB.GetDCCStats (pc)["Luck"]

def _Wait (done, most = 40):
	waited = 0
	while not done () and waited < most:
		waited += 1
		yield

def _Logged (text):
	return any (text in line for line in GemRB.GetDCCCombatLog ())

def _LogCount (text):
	return sum (1 for line in GemRB.GetDCCCombatLog () if text in line)

def _Selected ():
	return [pc for pc in range (1, GemRB.GetPartySize () + 1) if GemRB.GameIsPCSelected (pc)]

def _Positions (pcs):
	return [GemRB.GetActorPosition (pc) for pc in pcs]

def _Spread (where):
	"""the smallest distance between any two of the positions"""
	least = 10 ** 6
	for i in range (len (where)):
		for j in range (i + 1, len (where)):
			dx = where[i]["X"] - where[j]["X"]
			dy = where[i]["Y"] - where[j]["Y"]
			least = min (least, int ((dx * dx + dy * dy) ** 0.5))
	return least

def _Spells (pc):
	return sorted (spell["Spell"] for spell in GemRB.GetDCCMagic (pc)["Spells"])

# ---------------------------------------------------------------------------------------------

def _Party ():
	"""1: sixteen peasants with their occupations and funnel records"""
	size = GemRB.GetPartySize ()
	_Check ("MaxPartySize", GemRB.GetVar ("MaxPartySize"), 16)
	_Check ("party of 16", size, 16)
	_Check ("Player16 and Player16Fill as script objects", GemRB.GetGameVar ("FNT_P16"), 1)
	for order, (name, occupation, race, tags) in enumerate (PEASANTS, 1):
		pc = _Slot (name)
		_Check ("%s in slot %d" % (name, order), pc, order)
		if not pc:
			continue
		stats = GemRB.GetDCCStats (pc)
		funnel = GemRB.GetDCCFunnel (pc)
		occ, place = funnel["Occupation"], funnel["Funnel"]
		_Check ("%s level 0" % name, [stats["Class"], stats["Level"]], [8, 0])
		_Check ("%s occupation" % name, [occ["Id"], sorted (occ["Skills"]), GemRB.GetPlayerStat (pc, IE_RACE)], [occupation, sorted (tags), race])
		_Check ("%s funnel record" % name, [place["StatusName"], place["Order"], place["Player"]], ["alive", order, -1])
	farmer = GemRB.GetDCCFunnel (_Slot ("protagon"))["Occupation"]
	_Check ("the farmer's items and sub-roll", [farmer["Weapon"], farmer["TradeGood"], farmer["Sub"]], ["fntpfork", "fnthen", 2])
	_Check ("class picks pending", [GemRB.GetDCCFunnel (_Slot (n))["Funnel"]["ClassPending"] for n in ("fntmira", "fntlael", "fnttobb", "fntodo")], [1, 1, 1, 0])
	yield

	# 2: the roster: sixteen slots; one, a toggle, everyone
	import Roster
	Roster.UpdateRoster ()
	_Check ("roster shows 16", len (Roster.Shown), 16)
	_Check ("roster names", Roster.Shown.get (16, "").split (" ")[0], "Pip")
	Roster.SelectOnly (5)
	yield
	_Check ("select one", _Selected (), [5])
	Roster.ToggleSelect (9)
	_Check ("toggle adds", _Selected (), [5, 9])
	Roster.ToggleSelect (5)
	_Check ("toggle takes away", _Selected (), [9])
	Roster.SelectAll ()
	yield
	_Check ("select all", len (_Selected ()), 16)
	_Check ("the default formation is the block", Roster.CurrentFormation (), 0)

	# 3: a formation move of all sixteen, then in a column: everyone gets there, nobody stuck
	for formation, target in ((0, (1250, 900)), (1, (1450, 1050))):
		GemRB.GameSetFormation (formation, GemRB.GameGetFormation ())
		GemRB.GameControlMoveSelected (target[0], target[1])
		yield
		yield from _Wait (lambda: not any (p["Moving"] for p in _Positions (range (1, 17))), 60)
		where = _Positions (range (1, 17))
		_Check ("formation %d: all 16 stopped" % formation, sum (1 for p in where if not p["Moving"]), 16)
		# each at its own place in the formation, none stuck on the way; bumping can leave the odd one a body
		# width or two off its place (stock pathfinding), which is fine
		off = [(pc, p["X"], p["Y"], p["DestX"], p["DestY"]) for pc, p in zip (range (1, 17), where) if abs (p["X"] - p["DestX"]) + abs (p["Y"] - p["DestY"]) > 160]
		_Check ("formation %d: all at their places" % formation, off, [])
		far = [pc for pc, p in zip (range (1, 17), where) if abs (p["X"] - target[0]) + abs (p["Y"] - target[1]) > 600]
		_Check ("formation %d: all near the goal" % formation, far, [])
		_Check ("formation %d: no two on one spot" % formation, _Spread (where) >= 8, True)
		_Log ("formation %d positions: %s" % (formation, " ".join ("%d,%d" % (p["X"], p["Y"]) for p in where)))
	GemRB.GameSetFormation (0, GemRB.GameGetFormation ())

def _Withdrawal ():
	"""4: a peasant engaged with the rat walks off: a formation move, then a scripted move"""
	rat = GemRB.DCCFindActor ("fntrat")
	corb = _Slot ("fntcorb")
	for how in ("formation", "scripted"):
		GemRB.ClearActions (corb)
		GemRB.ExecuteString ("JumpToPoint([1180.1010])", corb)
		for i in range (3):
			yield
		GemRB.DCCForceDice (corb, [10, 1])
		GemRB.DCCAttack (corb, rat)
		GemRB.DCCForceDice (corb, [])
		before = _LogCount ("Giant rat gets a free attack: Corbin withdraws")
		GemRB.DCCForceDice (rat, [2, 1])
		if how == "formation":
			GemRB.GameSelectPC (corb, 1, SELECT_REPLACE)
			GemRB.GameControlMoveSelected (1180, 820)
		else:
			GemRB.DCCWalk (corb, 1180, 820)
		yield from _Wait (lambda: _LogCount ("Giant rat gets a free attack: Corbin withdraws") > before, 30)
		GemRB.DCCForceDice (rat, [])
		_Check ("withdrawal on a %s move" % how, _LogCount ("Giant rat gets a free attack: Corbin withdraws"), before + 1)
	_Check ("corbin alive", _Dead (corb), False)
	_Check ("level 0: the last weapon it fought with", GemRB.GetDCCFunnel (corb)["Occupation"]["LastWeapon"], "fntlswd")
	GemRB.GameSelectPC (0, 1)

def _ClassPicks ():
	"""5: the level-1 class picks: spells by class and level, substitutes, open slots, choices"""
	import DCCLearn
	mira, lael, tobb, pip = _Slot ("fntmira"), _Slot ("fntlael"), _Slot ("fnttobb"), _Slot ("fntpip")

	# the elf has to take the elf class
	_Check ("an elf can't pick wizard", GemRB.DCCLevelUp (lael, 1), -1)

	# wizard, Int 16: 4 + 1 spells. Rows 4 (chill touch: no content, substituted by the first of the
	# three wizard spells, flaming hands), 16 (magic missile), 23 (sleep), then 1 and 2 (no content,
	# nothing left to substitute: two open slots)
	GemRB.DCCForceDice (mira, [3, 4, 1, 16, 23, 1, 2])
	hp = GemRB.DCCLevelUp (mira, 1)
	GemRB.DCCForceDice (mira, [])
	magic = GemRB.GetDCCMagic (mira)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("wizard notes: " + notes)
	_Check ("wizard: level 1, d4 3", [hp, GemRB.GetDCCStats (mira)["Class"], GemRB.GetDCCStats (mira)["Level"]], [3, 1, 1])
	_Check ("wizard: spells known 5, 3 learned", [magic["Allowed"][0], magic["Counted"][0]], [5, 3])
	_Check ("wizard: the three with content", _Spells (mira), ["dwflam", "dwmmis", "dwslee"])
	_Check ("wizard: chill touch substituted", "Chill touch has no content yet; substituted dwflam" in notes, True)
	_Check ("wizard: two slots open", [notes.count ("the slot stays open"), "2 slot(s) stay open" in notes], [2, True])
	_Check ("wizard: mercurial magic on each", all (spell["Mercurial"] for spell in magic["Spells"]), True)
	_Check ("wizard: fresh until confirmed", magic["Fresh"], 3)
	_Check ("wizard: no class pick pending", GemRB.GetDCCFunnel (mira)["Funnel"]["ClassPending"], 0)
	_Check ("wizard can't learn a cleric spell", GemRB.DCCLearnSpell (mira, "dcbles"), -1)
	DCCLearn.OpenLearnWindow (mira)
	_Check ("learn window lists them", sorted (DCCLearn.Last["Spells"]), ["dwflam", "dwmmis", "dwslee"])
	DCCLearn.Confirm ()
	_Check ("wizard confirmed", GemRB.GetDCCMagic (mira)["Fresh"], 0)
	yield

	# elf, Int 13: 3 spells; invoke patron and patron bond come free but have no content. Row 15
	# (invoke patron: the pair, no content) is substituted (the second of three: magic missile),
	# then 13 (flaming hands) and 23 (sleep). The lucky spell defaults to the first, the window picks
	# sleep: Luck 16, +2, frozen
	GemRB.DCCForceDice (lael, [4, 15, 2, 13, 23])
	hp = GemRB.DCCLevelUp (lael, 6)
	GemRB.DCCForceDice (lael, [])
	magic = GemRB.GetDCCMagic (lael)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("elf notes: " + notes)
	_Check ("elf: level 1", [hp, GemRB.GetDCCStats (lael)["Class"]], [4, 6])
	_Check ("elf: 3 spells", [magic["Allowed"][0], _Spells (lael)], [3, ["dwflam", "dwmmis", "dwslee"]])
	_Check ("elf: the free patron spells have no content", "free Invoke patron has no content yet" in notes and "free Patron bond has no content yet" in notes, True)
	_Check ("elf: the default lucky spell", [s["Spell"] for s in magic["Spells"] if s["Lucky"]], ["dwmmis"])
	DCCLearn.OpenLearnWindow (lael)
	_Check ("elf's learn window offers the lucky pick", GemRB.GetDCCMagic (lael)["PicksLucky"], 1)
	DCCLearn.PickLucky ("dwslee")
	DCCLearn.Confirm ()
	magic = GemRB.GetDCCMagic (lael)
	_Check ("elf: sleep is lucky, +2 frozen", [(s["Spell"], s["LuckyMod"]) for s in magic["Spells"] if s["Lucky"]], [("dwslee", 2)])
	Saved["lucky"] = "dwslee"
	yield

	# cleric, Per 14: Table 1-5 gives 4 first-level spells; three have content, chosen (the
	# default: dice 2 of 3, paralysis, then 1 of 2, blessing, then word of command), one open
	GemRB.DCCForceDice (tobb, [5, 2, 1])
	hp = GemRB.DCCLevelUp (tobb, 3)
	GemRB.DCCForceDice (tobb, [])
	magic = GemRB.GetDCCMagic (tobb)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("cleric notes: " + notes)
	_Check ("cleric: level 1", [hp, GemRB.GetDCCStats (tobb)["Class"]], [5, 3])
	_Check ("cleric: 4 allowed, 3 known", [magic["Allowed"][0], magic["Counted"][0]], [4, 3])
	_Check ("cleric: the god's list", _Spells (tobb), ["dcbles", "dcpara", "dcword"])
	_Check ("cleric: no mercurial magic", any (spell["Mercurial"] for spell in magic["Spells"]), False)
	_Check ("cleric: one open", "every spell with content is known; the slot stays open" in notes, True)
	_Check ("cleric: swap refused (knows it)", GemRB.DCCReplaceSpell (tobb, "dcpara", "dcbles"), False)
	_Check ("cleric can't learn a wizard spell", GemRB.DCCLearnSpell (tobb, "dwmmis"), -1)
	GemRB.DCCConfirmSpells (tobb)
	yield

	# a wizard with Int 5: 4 - 2 = 2 spells, rows 16 and 23; the optional rule swaps one of the two
	# (sleep for flaming hands) and refuses a second
	GemRB.DCCForceDice (pip, [2, 16, 23])
	GemRB.DCCLevelUp (pip, 1)
	GemRB.DCCForceDice (pip, [])
	magic = GemRB.GetDCCMagic (pip)
	_Check ("low-Int wizard: 2 spells", [magic["Allowed"][0], _Spells (pip)], [2, ["dwmmis", "dwslee"]])
	_Check ("low-Int wizard: one swap, flaming hands offered", [magic["Swaps"], [c["Spell"] for c in magic["Choices"][0]]], [1, ["dwflam"]])
	_Check ("can't learn a third", GemRB.DCCLearnSpell (pip, "dwflam"), -1)
	DCCLearn.OpenLearnWindow (pip)
	_Check ("swap sleep for flaming hands", DCCLearn.Swap ("dwslee", "dwflam"), True)
	_Check ("after the swap", _Spells (pip), ["dwflam", "dwmmis"])
	_Check ("a second swap refused", GemRB.DCCReplaceSpell (pip, "dwmmis", "dwslee"), False)
	_Check ("the chosen one rolled its own mercurial magic", [bool (s["Mercurial"]) and s["Replaced"] for s in GemRB.GetDCCMagic (pip)["Spells"] if s["Spell"] == "dwflam"], [True])
	DCCLearn.Confirm ()
	_Check ("pip confirmed", GemRB.GetDCCMagic (pip)["Fresh"], 0)
	Saved["spells"] = {"fntmira": _Spells (mira), "fntlael": _Spells (lael), "fnttobb": _Spells (tobb), "fntpip": _Spells (pip)}

def _Prompts ():
	"""6: the prompt load: level-0 policies, the host's options, prompts in line, the panel"""
	import DCCParty
	hild, rolf, odo, wyn, brin = _Slot ("fnthild"), _Slot ("fntrolf"), _Slot ("fntodo"), _Slot ("fntwyn"), _Slot ("fntbrin")
	_Check ("prompt options off", [GemRB.GetGameVar ("DCC_PROMPT_AUTO"), GemRB.GetGameVar ("DCC_PROMPT_LOUD0")], [0, 0])

	# Hilde (Luck 10, avoid death) fails a save vs death by 3: no prompt for a peasant, the policy burns 3
	GemRB.DCCForceDice (hild, [8])
	GemRB.ApplySpell (hild, "FNTSLAY")
	GemRB.DCCForceDice (hild, [])
	yield
	_Check ("level 0: no prompt", GemRB.GetPrompt (), None)
	_Check ("level 0: the policy burns 3 and survives", [_Dead (hild), _Luck (hild)], [False, 7])

	# Rolf's player set "never": he dies without a word; the funnel records where and when
	GemRB.DCCSetLuckPolicy (rolf, DCC_BURN_NEVER, 10, DCC_LUCK_PAUSE)
	GemRB.DCCForceDice (rolf, [3])
	GemRB.ApplySpell (rolf, "FNTSLAY")
	GemRB.DCCForceDice (rolf, [])
	yield from _Wait (lambda: _Dead (rolf), 10)
	place = GemRB.GetDCCFunnel (rolf)["Funnel"]
	_Check ("rolf dies in the funnel", [_Dead (rolf), place["StatusName"], place["DiedIn"], place["DiedAt"] > 0], [True, "dead", "fnt01", True])
	_Check ("no prompt for rolf", GemRB.GetPrompt (), None)
	Saved["dead"] = (rolf, place["DiedAt"])

	# the host lets peasants ask: Odo's failed save waits for his player
	_Check ("level-0 option on", GemRB.DCCSetPromptOptions (0, 1), True)
	GemRB.DCCForceDice (odo, [9])
	GemRB.ApplySpell (odo, "FNTSLAY")
	GemRB.DCCForceDice (odo, [])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("level 0 asks now", prompt and [prompt["Tag"], prompt["Actor"] == GemRB.DCCFindActor ("fntodo")], ["luckburn", True])
	if prompt:
		GemRB.AnswerPrompt (prompt["ID"], [2])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 10)
	_Check ("odo burns 2 and lives", [_Dead (odo), _Luck (odo)], [False, 8])

	# auto-resolve after 2 s; two fail at once: one window, the other waits in line, both answer
	# themselves with the policy (Wyn Luck 10 and Brin 8, both Sta +1: 9 vs 11, burn 2)
	_Check ("auto-resolve 2 s", GemRB.DCCSetPromptOptions (2, 1), True)
	GemRB.DCCForceDice (wyn, [8])
	GemRB.ApplySpell (wyn, "FNTSLAY")
	GemRB.DCCForceDice (brin, [8])
	GemRB.ApplySpell (brin, "FNTSLAY")
	GemRB.DCCForceDice (wyn, [])
	GemRB.DCCForceDice (brin, [])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("two at once: one open, one waiting", prompt and [prompt["Tag"], prompt["Queued"]], ["luckburn", 1])
	_Check ("auto-resolve caps the wait", prompt and prompt["Remaining"] <= 2000, True)
	yield from _Wait (lambda: GemRB.GetPrompt () is not None and GemRB.GetPrompt ()["Queued"] == 0, 12)
	second = GemRB.GetPrompt ()
	_Check ("then the second, alone", second and [second["Queued"], second["ID"] != prompt["ID"]], [0, True])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 12)
	_Check ("both answered by their policy", [_Dead (wyn), _Luck (wyn), _Dead (brin), _Luck (brin)], [False, 8, False, 6])
	_Check ("options off again", GemRB.DCCSetPromptOptions (0, 0), True)

	# the party settings: everyone's Luck policy and pause at once
	count = DCCParty.SetLuckPolicy (DCC_BURN_MISS_TO_HIT, False)
	_Check ("panel: all 16 set", count, 16)
	stats = [GemRB.GetDCCStats (pc) for pc in range (1, 17)]
	_Check ("panel: misses too, no pause", sorted (set ((s["LuckPolicy"], s["LuckFlags"] & DCC_LUCK_PAUSE) for s in stats)), [(DCC_BURN_MISS_TO_HIT, 0)])
	DCCParty.SetLuckPolicy (DCC_BURN_AVOID_DISASTER, True)
	_Check ("panel: back to avoid death, pausing", sorted (set ((s["LuckPolicy"], s["LuckFlags"] & DCC_LUCK_PAUSE) for s in (GemRB.GetDCCStats (pc) for pc in range (1, 17)))), [(DCC_BURN_AVOID_DISASTER, DCC_LUCK_PAUSE)])
	_Check ("panel: spellburn for the three wizards and the elf", DCCParty.SetSpellburn (0), 3)
	DCCParty.OpenPartySettings ()
	yield
	DCCParty.ClosePartySettings ()

def _Travel ():
	"""7: the whole party of 16 (the living) to the cave and back"""
	living = [pc for pc in range (1, 17) if not _Dead (pc)]
	for i, pc in enumerate (living):
		row, column = divmod (i, 4)
		GemRB.ClearActions (pc)
		GemRB.ExecuteString ("JumpToPoint([%d.%d])" % (560 + column * 40, 420 + row * 34), pc)
	for i in range (4):
		yield
	GemRB.GameSelectPC (0, 1)
	GemRB.GameSelectPC (living[0], 1, SELECT_REPLACE)
	_Check ("travel: a click on ToCave", GemRB.GameControlTravel ("ToCave"), True)
	yield from _Wait (lambda: GemRB.GetGameVar ("FNT_CAVE") == 1, 60)
	for i in range (4):
		yield
	where = _Positions (living)
	_Check ("travel: the whole party in the cave", sorted (set (p["Area"].lower () for p in where)), ["fnt02"])
	_Check ("travel: %d arrived apart" % len (living), _Spread (where) >= 8, True)
	_Check ("travel: Player16 there (the area script saw it)", GemRB.GetGameVar ("FNT_CAVE"), 1)
	_Log ("arrival: " + " ".join ("%d,%d" % (p["X"], p["Y"]) for p in where))

def _SaveGames ():
	return [game for game in GemRB.GetSaveGames () if game.GetName () == SaveName]

def _Save ():
	"""8: save 16 with their records (one marked reserve and dealt to player 2), then load"""
	last = _Slot ("fntpip")
	_Check ("pip to the reserve, dealt to player 2", GemRB.DCCSetFunnel (last, 4, -1, 2), True)
	Saved["funnel"] = [GemRB.GetDCCFunnel (pc) for pc in range (1, 17)]
	for game in _SaveGames ():
		GemRB.DeleteSaveGame (game)
	_Check ("game saved", GemRB.SaveGame (None, SaveName), 0)

def _StepSave ():
	global Step
	games = _SaveGames ()
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
	_Check ("loaded: party of 16", GemRB.GetPartySize (), 16)
	after = [GemRB.GetDCCFunnel (pc) for pc in range (1, 17)]
	_Check ("loaded: every OCCU and FUNL record", after, Saved.get ("funnel"))
	pip = GemRB.GetDCCFunnel (_Slot ("fntpip"))["Funnel"]
	_Check ("loaded: pip in the reserve for player 2", [pip["StatusName"], pip["Player"]], ["reserve", 2])
	rolf, diedAt = Saved.get ("dead", (0, 0))
	place = GemRB.GetDCCFunnel (rolf)["Funnel"]
	_Check ("loaded: rolf's death", [_Dead (rolf), place["StatusName"], place["DiedAt"], place["DiedIn"]], [True, "dead", diedAt, "fnt01"])
	_Check ("loaded: the spells", dict ((name, _Spells (_Slot (name))) for name in ("fntmira", "fntlael", "fnttobb", "fntpip")), Saved.get ("spells"))
	lael = GemRB.GetDCCMagic (_Slot ("fntlael"))
	_Check ("loaded: the elf's lucky spell", [(s["Spell"], s["LuckyMod"]) for s in lael["Spells"] if s["Lucky"]], [(Saved.get ("lucky"), 2)])
	_Check ("loaded: in the cave", GemRB.GetActorPosition (1)["Area"].lower (), "fnt02")
	import Roster
	Roster.UpdateRoster ()
	_Check ("loaded: the roster", len (Roster.Shown), 16)
	return "done"

def _Run ():
	yield from _Party ()
	yield from _Withdrawal ()
	yield from _ClassPicks ()
	yield from _Prompts ()
	yield from _Travel ()
	_Save ()

def _StepRun ():
	global Steps
	if Steps is None:
		Steps = _Run ()
	try:
		next (Steps)
		return "run"
	except StopIteration:
		return "save"

StepTable = {
	"run": _StepRun,
	"save": _StepSave,
	"loaded": _StepLoaded,
}

def _Tick (token):
	global Step, Failures
	if token != Token or Step in ("loading", "finished"):
		return
	try:
		if Step == "wait":
			if GemRB.GetGameVar ("FNT_AUTOTEST") == 1:
				_Log ("autotest start")
				Step = "run"
			return
		if Step == "done":
			_Log ("autotest done: %d checks, %d failed" % (Checks, Failures))
			GemRB.SetGlobal ("FNT_AT_DONE", "GLOBAL", 1 if Failures == 0 else 2)
			Step = "finished"
			return
		Step = StepTable[Step] ()
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
