# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The real game's smoke test (game/, scripts/run.sh ethren-auto): nothing runs unless the table ethauto.2da
# exists, which only the game's autotest variant ships (game/src/tables/autotest, a ModPath of the -auto run).
# Then:
#   1. the rolling screen opens as for a player; the first card is made a dwarf, its looks stepped in the
#      paper doll (M3.5); both are screenshotted and the cards accepted by themselves (BeforeRoll);
#   2. the funnel starts in Delmord's town hall (DL0101) with sixteen peasants on walkable ground;
#   3. the party walks every way Delmord has, through the doors and along the road, and back to the hall:
#      hall -> western edge -> village east -> tavern -> east -> smithy -> east -> shrine -> east -> western
#      edge -> hall, each arrival checked (all sixteen in the new area);
#   4. screenshots in the save path: game_looks (the paper doll), game_roll (the rolling screen), game_<area> (each area as drawn, the
#      first visit), game_hall_screen (the hall with the GUI).
# Every check logs "[ETH] ok ..." or "[ETH] FAIL ...", the end "[ETH] autotest done: N checks, F failed";
# ETH_AT_DONE becomes 1 if all passed, 2 if not (run.sh's AutotestQuit).

import GemRB
from GUIDefines import *
from ie_stats import *

Chosen = {} # what the rolling screen test chose for the first card (M3.5)
Checks = 0
Failures = 0
Steps = None
Token = 0

# (area, door to open first or None, exit, the area it leads to)
ROUTE = (
	("DL0101", "HallDoor", "HallDoorExit", "DL0100"),
	("DL0100", None, "ToEast", "DL0200"),
	("DL0200", "TavernDoor", "TavernDoorExit", "DL0201"),
	("DL0201", "TavernDoor", "TavernDoorExit", "DL0200"),
	("DL0200", "SmithyDoor", "SmithyDoorExit", "DL0202"),
	("DL0202", "SmithyDoor", "SmithyDoorExit", "DL0200"),
	("DL0200", "ShrineDoor", "ShrineDoorExit", "DL0203"),
	("DL0203", None, "ShrineExit", "DL0200"),
	("DL0200", None, "ToWest", "DL0100"),
	("DL0100", "HallDoor", "HallDoorExit", "DL0101"),
)
NAMES = {"DL0100": "west", "DL0101": "hall", "DL0200": "east", "DL0201": "tavern", "DL0202": "smithy", "DL0203": "shrine"}

def Enabled ():
	return GemRB.LoadTable ("ethauto", True) is not None

def _Log (text):
	print ("[ETH] " + text)

def _Check (what, got, expected):
	global Checks, Failures
	Checks += 1
	if got == expected:
		_Log ("ok %s = %s" % (what, got))
	else:
		Failures += 1
		_Log ("FAIL %s = %s, expected %s" % (what, got, expected))

def _Shot (name, center = 0, screen = 0):
	path = GemRB.GetSystemVariable (SV_SAVEPATH) + "/game_%s.bmp" % name
	_Check ("screenshot %s" % name, GemRB.SaveScreenshot (path, center, screen), True)

def _Wait (done, most = 40):
	waited = 0
	while not done () and waited < most:
		waited += 1
		yield

def BeforeRoll ():
	"""Start.NewGame, once the rolling screen is open: screenshot it, then accept the cards"""
	if not Enabled ():
		return
	import RollScreen
	import FunnelRoll
	def choose ():
		"""M3.5: the first card a dwarf with a style stepped on, the paper doll open and turned"""
		card = RollScreen.Cards[0]
		occupations = dict ((race, occ["OccupationName"]) for race, occ in card["Occupations"].items ())
		numbers = [list (card["Scores"]), card["HP"], card["Augur"], card["Copper"]]
		if card["Race"] == 4:
			RollScreen.SetSpecies (0, 1)
		_Check ("roll: the card's species can be chosen", RollScreen.SetSpecies (0, 4), True)
		_Check ("roll: a dwarf shows the dwarf occupation rolled with the card", card["OccupationName"], occupations[4])
		_Check ("roll: the numbers stay as rolled", [list (card["Scores"]), card["HP"], card["Augur"], card["Copper"]], numbers)
		if card["Sex"] != 1:
			RollScreen.NextSex (0)
		RollScreen.OpenLooks (0)
		Chosen["Hair"] = RollScreen.StepLook (0, "Hair", 1)
		Chosen["Beard"] = RollScreen.StepLook (0, "Beard", 1)
		Chosen["Major"] = RollScreen.StepLook (0, "Tunic", 1)
		RollScreen._Turn (-2)
		GemRB.SetTimer (look, 800, 0)
	def look ():
		_Shot ("looks", 0, 1)
		RollScreen.CloseLooks ()
		_Shot ("roll", 0, 1)
		_Log ("rolling screen: accept")
		RollScreen.Accept ()
	GemRB.SetTimer (choose, 1500, 0)

def _Party ():
	return list (range (1, GemRB.GetPartySize () + 1))

def _Areas ():
	return sorted (set (GemRB.GetActorPosition (pc)["Area"].upper () for pc in _Party ()))

def _Run ():
	party = _Party ()
	_Check ("the funnel's sixteen", len (party), 16)
	_Check ("they start in the town hall", _Areas (), ["DL0101"])
	# M3.5: the first card came in as chosen: a dwarf with the looks stepped to
	stats = GemRB.GetDCCStats (1)
	_Check ("the first peasant is a dwarf", GemRB.GetPlayerStat (1, IE_RACE), 4)
	_Check ("with the hair and beard chosen", [stats.get ("Hair"), stats.get ("Beard")], [Chosen.get ("Hair"), Chosen.get ("Beard")])
	_Check ("and the tunic colour chosen", GemRB.GetPlayerStat (1, IE_MAJOR_COLOR) & 0xff, Chosen.get ("Major"))
	for i in range (4):
		yield
	_Shot ("hall", 1)
	_Shot ("hall_screen", 1, 1)
	seen = {"DL0101"}
	for area, door, exit, to in ROUTE:
		_Check ("in %s before %s" % (area, exit), _Areas (), [area])
		GemRB.GameSelectPC (0, 1)
		GemRB.GameSelectPC (1, 1, SELECT_REPLACE)
		if door:
			GemRB.ClearActions (1)
			GemRB.ExecuteString ("OpenDoor(\"%s\")" % door, 1)
			yield from _Wait (lambda: GemRB.EvaluateString ("OpenState(\"%s\",1)" % door, True), 60)
			_Check ("%s: %s opens" % (area, door), GemRB.EvaluateString ("OpenState(\"%s\",1)" % door, True), True)
		_Check ("%s: travel by %s" % (area, exit), GemRB.GameControlTravel (exit), True)
		yield from _Wait (lambda: _Areas () == [to], 80)
		if _Areas () != [to]:
			# where everyone stands: who keeps the party from going
			_Log ("stuck: " + "  ".join ("%d %s %d,%d%s" % (pc, GemRB.GetPlayerName (pc, 0), p["X"], p["Y"], " ->%d,%d" % (p["DestX"], p["DestY"]) if p["Moving"] else "")
				for pc, p in ((pc, GemRB.GetActorPosition (pc)) for pc in _Party ())))
		_Check ("%s -> %s: the whole party arrived" % (exit, to), _Areas (), [to])
		for i in range (4):
			yield
		if to not in seen:
			seen.add (to)
			_Shot (NAMES[to], 1)

def OnGameStart ():
	"""MessageWindow.OnLoad: start ticking once the game is up"""
	global Token
	if not Enabled ():
		return
	Token += 1
	token = Token
	GemRB.SetTimer (lambda: _Tick (token), 500, -1)

def _Tick (token):
	global Steps, Failures
	if token != Token or Steps is False:
		return
	try:
		if Steps is None:
			_Log ("autotest start")
			Steps = _Run ()
		next (Steps)
	except StopIteration:
		_Done ()
	except Exception as e:
		import traceback
		traceback.print_exc ()
		Failures += 1
		_Log ("FAIL exception %s" % e)
		_Done ()

def _Done ():
	global Steps
	Steps = False
	_Log ("autotest done: %d checks, %d failed" % (Checks, Failures))
	GemRB.SetGlobal ("ETH_AT_DONE", "GLOBAL", 1 if Failures == 0 else 2)
