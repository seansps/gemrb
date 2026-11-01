# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The real game's smoke test (game/, scripts/run.sh ethren-auto): nothing runs unless the table ethauto.2da
# exists, which only the game's autotest variant ships (game/src/tables/autotest, a ModPath of the -auto run).
# Then:
#   1. the rolling screen opens as for a player, is screenshotted and accepted by itself (BeforeRoll);
#   2. the funnel starts in Delmord's town hall (DL0101) with sixteen peasants on walkable ground;
#   3. the party walks every way Delmord has, through the doors and along the road, and back to the hall:
#      hall -> western edge -> village east -> tavern -> east -> smithy -> east -> shrine -> east -> western
#      edge -> hall, each arrival checked (all sixteen in the new area);
#   4. screenshots in the save path: game_roll (the rolling screen), game_<area> (each area as drawn, the
#      first visit), game_hall_screen (the hall with the GUI).
# Every check logs "[ETH] ok ..." or "[ETH] FAIL ...", the end "[ETH] autotest done: N checks, F failed";
# ETH_AT_DONE becomes 1 if all passed, 2 if not (run.sh's AutotestQuit).

import GemRB
from GUIDefines import *
from ie_stats import *

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
	def accept ():
		import RollScreen
		_Shot ("roll", 0, 1)
		_Log ("rolling screen: accept")
		RollScreen.Accept ()
	GemRB.SetTimer (accept, 1500, 0)

def _Party ():
	return list (range (1, GemRB.GetPartySize () + 1))

def _Areas ():
	return sorted (set (GemRB.GetActorPosition (pc)["Area"].upper () for pc in _Party ()))

def _Run ():
	party = _Party ()
	_Check ("the funnel's sixteen", len (party), 16)
	_Check ("they start in the town hall", _Areas (), ["DL0101"])
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
