# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The real game's smoke test (game/, scripts/run.sh ethren-auto): nothing runs unless the table ethauto.2da
# exists, which only the game's autotest variant ships (game/src/tables/autotest, a ModPath of the -auto run).
# Then:
#   1. the rolling screen opens as for a player; the second card's buttons are clicked (GemRB.TestClick);
#      the first card is made a dwarf man, its looks stepped in the paper doll (M3.5; his beard never steps
#      to none); both are screenshotted and the cards accepted by themselves (BeforeRoll);
#   2. the funnel starts in Delmord's town hall (DL0101) with sixteen peasants on walkable ground;
#   3. the party walks every way Delmord has, through the doors and along the road, and back to the hall:
#      hall -> western edge -> village east -> tavern -> east -> smithy -> east -> shrine -> east -> western
#      edge -> hall, each arrival checked (all sixteen in the new area);
#   4. the roster's tiles answer real mouse clicks (GemRB.TestClick): select, shift-click, right click;
#   5. the wooded trail's content (DL0300, the first visit; _Trail): the cart's bandits placed and hostile, the archer's cover,
#      their loot, their hoods (no hair under them, the beards whole), the carter and the cart; every info point's string, by a real click; the snare's data; a bandit's last words,
#      a broken one's run up the trail and off the map, one taken alive and his dialog; a Detect traps searcher finds the snare
#      (the notice line, the tip) and its handle comes on; the other ways past it, each on the snare set again (GemRB.DCCSetTrap):
#      a thrown stick and a pole carried near it spring it on nothing (the birch stands up), a hen hangs by one leg until cut
#      down, round through the thorns leaves it set, Jory's warning and a trapper call it out; a walker springs it (held, hung
#      head down under the birch in the hanging body, its weapon dropped, the coppers) and one with a knife cuts it down; the
#      gang's colours; the quiet approach (the hound's ears and nose by gait: a walk, a run, Stealth unheard and heard, and a
#      blow from hiding by surprise); the archer stepping back from melee; the herb patch's check; the trail's mouth; the
#      party walks up the trail;
#   6. screenshots in the save path: game_looks (the paper doll), game_roll (the rolling screen), game_<area> (each area as drawn, the
#      first visit), game_hall_screen (the hall with the GUI), game_trail_* (the cart, the archer, the hound alive and dead, the hooded captive, an info point, the snare sprung
#      empty, the hen in it, a peasant hanging in it).
# Every check logs "[ETH] ok ..." or "[ETH] FAIL ...", the end "[ETH] autotest done: N checks, F failed";
# ETH_AT_DONE becomes 1 if all passed, 2 if not (run.sh's AutotestQuit).

import GemRB
from GUIDefines import *
from ie_stats import *
from ie_restype import RES_BAM

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
	("DL0100", None, "ToTrail", "DL0300"),
	("DL0300", None, "ToDelmord", "DL0100"),
	("DL0100", "HallDoor", "HallDoorExit", "DL0101"),
)
NAMES = {"DL0100": "west", "DL0101": "hall", "DL0200": "east", "DL0201": "tavern", "DL0202": "smithy", "DL0203": "shrine",
	"DL0300": "trail"}

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
		clicks ()
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
		# a dwarf man always has a beard (what tells him from a halfling man): it steps through the dwarven ones
		_Check ("roll: a dwarf man's beard steps to another dwarven one, never none", Chosen["Beard"] in (4, 5, 6), True)
		Chosen["Major"] = RollScreen.StepLook (0, "Tunic", 1)
		RollScreen._Turn (-2)
		GemRB.SetTimer (look, 800, 0)
	def clicks ():
		"""the second card's buttons by real clicks: each button acts on its own card"""
		second = RollScreen.Cards[1]
		sex, race, alignment = second["Sex"], second["Race"], second["Alignment"]
		_Click (RollScreen.Window, RollScreen._Control (1, 7))
		_Check ("roll: a click on card 2's sex button changes its sex", second["Sex"], 2 if sex == 1 else 1)
		_Click (RollScreen.Window, RollScreen._Control (1, 6))
		_Check ("roll: a click on card 2's species button changes its species", second["Race"] != race, True)
		_Click (RollScreen.Window, RollScreen._Control (1, 5))
		_Check ("roll: a click on card 2's alignment button changes it", second["Alignment"], FunnelRoll.NextAlignment (alignment))
		_Click (RollScreen.Window, RollScreen._Control (1, 8))
		_Check ("roll: a click on card 2's Looks... opens its paper doll", [bool (RollScreen.LooksWindow), RollScreen.LooksIndex], [True, 1])
		hair = second["Looks"]["Hair"]
		_Click (RollScreen.LooksWindow, RollScreen.LooksWindow.GetControl (11))
		_Check ("roll: a click on the doll's hair arrow steps the hair", second["Looks"]["Hair"] != hair or len (FunnelRoll.LookOptions (RollScreen._Tables (), second["Race"], second["Sex"], "Hair")) < 2, True)
		RollScreen.CloseLooks ()
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
	yield from _RosterClicks ()
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
			if to == "DL0300":
				yield from _Trail ()

# --- the wooded trail (DL0300): docs/world/funnel-adventure.md, area 3 -------------------------------------------------

TRAIL_BANDITS = ("DlCut1", "DlCut2", "DlCut3", "DlArch")
TRAIL_INFO = ("DlTollBoard", "DlHeadMan", "DlHeadGranny", "DlHeadYoung", "DlBarricade", "DlCartInfo", "DlBirches", "DlHare", "DlHerbs")
LAST_WORDS = ("Tell Bess she owes me four silver.", "It's warm. Why is it warm?", "Not the face. I'm getting married.",
	"Ha! Missed m--", "I can hear it counting.", "Bury me face down. It can't find you face down.")
MS_NONE, MS_DETECTTRAPS, MS_STEALTH = 0, 2, 3
Hover = ["", 0] # what _ClickMap's hover found under the cursor

def _Logged (text):
	return any (text in line for line in GemRB.GetDCCCombatLog ())

def _Items (actor):
	"""the resrefs (upper case) and counts (1 for an item that isn't a stack) of what a creature carries, fists aside"""
	items = []
	for slot in GemRB.GetSlots (actor, 0, 1):
		item = GemRB.GetSlotItem (actor, slot)
		if item and item["ItemResRef"].upper () != "FIST":
			items.append ((item["ItemResRef"].upper (), item["Usages0"] or 1))
	return items

def _Pile (name):
	"""a named container's items, as _Items"""
	trap = GemRB.GetDCCTrap (name)
	return [(ref.upper (), count or 1) for ref, count in trap["Items"]] if trap else None

def _GroundAt (pc):
	"""the ground pile at a party member's feet, as _Items"""
	pile = GemRB.GetContainer (pc, 1)
	items = []
	for i in range (pile["ItemCount"] if pile else 0):
		item = GemRB.GetContainerItem (pc, i)
		if item:
			items.append ((item["ItemResRef"].upper (), item["Usages0"] or 1))
	return items

def _AreaVar (name):
	"""a MYAREA variable of the current area (0-20)"""
	for value in range (21):
		if GemRB.EvaluateString ('Global("%s","MYAREA",%d)' % (name, value), True):
			return value
	return -1

def _Unpause ():
	if GemRB.GamePause (3, 1):
		GemRB.GamePause (0, 1)

def _ClickMap (x, y, pc, name = None):
	"""a real click on the area at (x, y), with pc selected: the view moves there first. With name, the hover is
	tried again (the view may still be settling) until the game view finds that region or container under the cursor"""
	GemRB.GameSelectPC (pc, 1, SELECT_REPLACE)
	GemRB.ClearActions (pc)
	GemRB.ExecuteString ("MoveViewPoint([%d.%d],0)" % (x, y), pc)
	for i in range (3):
		yield
	for i in range (5):
		sx, sy = GemRB.GameControlMapToScreen (x, y)
		GemRB.TestClick (sx, sy, 0) # the hover first: the game view finds what is under the cursor as it draws
		yield
		Hover[:] = GemRB.GameControlGetHover ()
		if not name or Hover[0] == name:
			break
	sx, sy = GemRB.GameControlMapToScreen (x, y)
	GemRB.TestClick (sx, sy)
	yield

def _Jump (actor, x, y):
	GemRB.ClearActions (actor)
	GemRB.ExecuteString ("JumpToPoint([%d.%d])" % (x, y), actor)

def _Said (pc):
	"""a party member's overhead text (GetOverheadText takes a global ID)"""
	said = GemRB.GetOverheadText (GemRB.GetDCCFunnel (pc)["GlobalID"])
	return said["Text"] if said else ""

def _QuietReset (hound):
	for name in TRAIL_BANDITS:
		actor = GemRB.DCCFindActor (name)
		if actor:
			GemRB.ClearActions (actor)
	GemRB.ClearActions (hound)
	GemRB.ExecuteString ('SetGlobal("DL_CART_ALERT","MYAREA",0)', 1)
	GemRB.ExecuteString ('SetGlobal("DL_CART_NOSE","MYAREA",0)', 1)

def _Quiet (hound, quiet, pc):
	"""the quiet approach (docs/world/funnel-adventure.md, "The first bandit encounter"), on the hound led off down the
	road (screen 1395,1046: the band is 24' and more away, and every spot below is more than 30' from them)"""
	home = GemRB.GetActorPosition (hound)
	party = _Party ()
	modes = dict ((p, GemRB.GetDCCStats (p)["PartyAI"]) for p in party)
	for p in party:  # the party AI stays out of it (it would go for the hound)
		GemRB.DCCSetPartyAI (p, 2)
	# trained in Outdoors (hide and sneak in the wild roll a d20, not the familiar d10 + 2: hidden in daylight needs it)
	GemRB.ExecuteString ('DCCAddSkill(Myself,"OUTDOORS")', quiet)
	_Jump (hound, 1395, 1046)
	yield
	yield
	# walking past at 33' (its walk range is 20'): not noticed
	_Jump (quiet, 1035, 1514)
	yield
	GemRB.ExecuteString ("MoveToPoint([1131.1389])", quiet)
	yield from _Wait (lambda: abs (GemRB.GetActorPosition (quiet)["X"] - 1131) + abs (GemRB.GetActorPosition (quiet)["Y"] - 1389) < 20, 60)
	for i in range (4):
		yield
	_Check ("trail, the quiet approach: walking by at 33', the hound doesn't notice", _AreaVar ("DL_CART_ALERT"), 0)
	# running the same way: heard at 40'
	_Jump (quiet, 1035, 1514)
	yield
	GemRB.ExecuteString ("RunToPoint([1131.1389])", quiet)
	yield from _Wait (lambda: _AreaVar ("DL_CART_ALERT") == 1, 60)
	said = GemRB.GetOverheadText (hound)
	_Check ("trail, the quiet approach: running, it hears at 40' and bays", [_AreaVar ("DL_CART_ALERT"), _AreaVar ("DL_CART_NOSE"), bool (said and "bays" in said["Text"])], [1, 1, True])
	_QuietReset (hound)
	GemRB.ClearActions (quiet)
	yield
	# in Stealth, hidden and unheard (every roll a 20), brought to 15': not noticed (a sneak at 10'). Into Stealth
	# far off first (standing at 15' out of it, it would be heard at once). The hound is led to the wood's shade in the
	# south-west (screen 440,1422: hiding there is DC 15, the road is broad daylight, DC 20)
	_Jump (hound, 440, 1422)
	_Jump (quiet, 1035, 1514)
	GemRB.DCCForceDice (quiet, [20] * 8)
	GemRB.SetModalState (quiet, MS_STEALTH)
	for i in range (4):
		yield
	GemRB.ExecuteString ("JumpToPoint([680.1422])", quiet)  # (a ClearActions would end Stealth)
	for i in range (10):
		yield
	GemRB.DCCForceDice (quiet, [])
	stealth = GemRB.GetDCCStealth (quiet)
	_Check ("trail, the quiet approach: sneaking unheard at 15', the hound doesn't notice", [stealth["Hidden"], stealth["Silent"], _AreaVar ("DL_CART_ALERT")], [1, 1, 0])
	# the blow from hiding: caught by surprise (the engine's DCC rule; held to a miss), and the band turns
	GemRB.DCCForceDice (quiet, [2])
	GemRB.DCCAttack (quiet, hound)
	GemRB.DCCForceDice (quiet, [])
	yield from _Wait (lambda: _AreaVar ("DL_CART_ALERT") == 1, 20)
	_Check ("trail, the quiet approach: a blow from hiding catches the hound by surprise, and the band turns", [_Logged ("caught by surprise"), _AreaVar ("DL_CART_ALERT")], [True, 1])
	GemRB.SetModalState (quiet, MS_NONE)
	GemRB.ClearActions (quiet)
	_Jump (quiet, 1035, 1514)
	_Jump (hound, 440, 1422)  # (back where it was: struck, it went for the one who struck it)
	for i in range (4):
		yield
	_QuietReset (hound)
	# in Stealth but heard (the hide a 20, the sneak a 1), brought to 15': noticed (a walk at 20')
	GemRB.DCCForceDice (quiet, [20, 1] * 4)
	GemRB.SetModalState (quiet, MS_STEALTH)
	for i in range (4):
		yield
	_Check ("trail, the quiet approach: (still unnoticed, far off in Stealth)", _AreaVar ("DL_CART_ALERT"), 0)
	GemRB.ExecuteString ("JumpToPoint([680.1422])", quiet)
	yield from _Wait (lambda: _AreaVar ("DL_CART_ALERT") == 1, 20)
	GemRB.DCCForceDice (quiet, [])
	stealth = GemRB.GetDCCStealth (quiet)
	_Check ("trail, the quiet approach: in Stealth but heard at 15', the hound notices", [stealth["Silent"], _AreaVar ("DL_CART_ALERT"), _AreaVar ("DL_CART_NOSE")], [0, 1, 1])
	_QuietReset (hound)
	GemRB.SetModalState (quiet, MS_NONE)
	_Jump (quiet, 1785, 1339)
	_Jump (hound, home["X"], home["Y"])
	for i in range (4):
		yield
	_QuietReset (hound)
	_Check ("trail, the quiet approach: back as it was, unnoticed", _AreaVar ("DL_CART_ALERT"), 0)
	for p in party:
		GemRB.DCCSetPartyAI (p, modes[p])

def _ShotAt (name, x, y, wait = 30):
	"""a screenshot with the view on an area point, once the floating text has gone"""
	for i in range (wait):
		yield
	GemRB.ExecuteString ("MoveViewPoint([%d.%d],0)" % (x, y), 1)
	for i in range (3):
		yield
	_Shot (name)

def _SnareSet (detected):
	"""the snare set again for the next way past it (GemRB.DCCSetTrap), its scripts' state back to before"""
	GemRB.DCCSetTrap ("DlSnare", 1, detected)
	for p in _Party ():
		GemRB.ExecuteString ('ApplySpellRES("DLSNCUT",Myself)', p)
	for var in ("DL_SNARE", "DL_SPRUNG", "DL_SNARE_POLE", "DL_SNARE_STICK", "DL_SNARE_BIRD", "DL_SNARE_WAY", "DL_SNARE_CALLED", "DL_SNARE_CP",
			"DL_SNARE_BODY"):
		GemRB.SetGlobal (var, "GLOBAL", 0)
	GemRB.SetGlobal ("DL_SNARE_FOUND", "GLOBAL", detected)
	for action in ('SetGlobal("DL_SPRUNG_SHOWN","MYAREA",0)', 'AmbientActivate("DlSprungDay",FALSE)', 'AmbientActivate("DlSprungNight",FALSE)',
			'Deactivate("DlSnareRope")', 'Activate("DlSnareHandle")' if detected else 'Deactivate("DlSnareHandle")'):
		GemRB.ExecuteString (action, 1)

def _Handle (who, way):
	"""a click on the found snare's handle and a reply (the dialog's transitions: 0 pole, 1 stick, 2 hen, 3 duck,
	4 goose, 5 disarm, 6 round through the thorns, 7 leave it)"""
	_Jump (who, 1450, 270)
	for i in range (3):
		yield
	yield from _ClickMap (1362, 186, who, "DlSnareHandle")
	yield from _Wait (_InDialog, 30)
	opened = _InDialog ()
	if opened:
		_Reply (way)
		yield from _Wait (lambda: not _InDialog (), 20)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE_WAY") == 0, 20)
	for i in range (3):
		yield
	return opened

def _SnareWays (party, cutter):
	"""the snare's other ways past (docs/world/funnel-adventure.md, "The teaching trap"), each on the snare set again"""
	thrower, henner, poler, rounder, jory, trapper = party[10], party[11], party[12], party[13], party[14], party[15]
	# a stick thrown into the leaves: it springs on nothing, the birch stands up empty
	opened = yield from _Handle (thrower, 1)
	_Check ("trail, the snare: a click on the handle opens its talk", opened, True)
	yield from _Wait (lambda: _AreaVar ("DL_SPRUNG_SHOWN") > 0, 20)
	snare = GemRB.GetDCCTrap ("DlSnare")
	_Check ("trail, the snare: a thrown stick springs it on nothing; the birch stands up", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetGameVar ("DL_SNARE_STICK"),
		snare["Flags"] & 0x100, GemRB.GetPlayerStat (thrower, IE_HELD), _AreaVar ("DL_SPRUNG_SHOWN") in (1, 2), "empty noose" in _Said (thrower)],
		[5, 1, 0x100, 0, True, True])
	_Shot ("trail_snare_empty", thrower)
	_SnareSet (1)
	yield
	# a hen sent across: up she goes by one leg, until a blade cuts her down and she goes back on her lead
	GemRB.CreateItem (henner, "HEN")
	yield
	yield from _Handle (henner, 2)
	_Check ("trail, the snare: the handle saw the hen", GemRB.GetGameVar ("DL_HAND_HEN"), 1)
	_Check ("trail, the snare: the hen hangs by one leg", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetGameVar ("DL_SNARE_BIRD"), ("HEN", 1) in _Items (henner),
		GemRB.GetDCCTrap ("DlSnareRope")["Flags"] & 0x100], [8, 1, False, 0])
	for i in range (3):
		yield
	_Shot ("trail_snare_hen", henner)
	yield from _ShotAt ("trail_snare_hen_quiet", 1344, 179)
	GemRB.CreateItem (cutter, "KNIFE")
	_Jump (cutter, 1450, 270)
	for i in range (3):
		yield
	yield from _ClickMap (1420, 195, cutter, "DlSnareRope")
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE") == 9, 30)
	_Check ("trail, the snare: cut down, the hen goes back to the one who cut her", [GemRB.GetGameVar ("DL_SNARE"), ("HEN", 1) in _Items (cutter)], [9, True])
	_SnareSet (0)
	yield
	# the 10' pole, before the snare is found: carried near it, it prods the leaves and springs it on nothing
	GemRB.CreateItem (poler, "POLE")
	_Jump (poler, 1035, 470)
	for i in range (3):
		yield
	GemRB.ExecuteString ("MoveToPoint([1330.300])", poler)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE") == 5, 60)
	_Check ("trail, the snare: a pole carried near it springs it on nothing, and it is found", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetGameVar ("DL_SNARE_POLE"),
		GemRB.GetDCCTrap ("DlSnare")["Detected"], GemRB.GetPlayerStat (poler, IE_HELD)], [5, 1, 1, 0])
	GemRB.ClearActions (poler)
	for i in range (3):
		GemRB.ExecuteString ('DestroyItem("POLE")', poler)
	_Jump (poler, 1785, 1339)
	_SnareSet (1)
	yield
	# round through the thorns: a minute, and the snare left set behind the party (off for it)
	hour = GemRB.GetGameTime ()
	yield from _Handle (rounder, 6)
	_Check ("trail, the snare: round through the thorns, a minute; it stays set, the party past it", [GemRB.GetGameVar ("DL_SNARE"),
		GemRB.GetDCCTrap ("DlSnare")["Trapped"], GemRB.GetDCCTrap ("DlSnare")["Flags"] & 0x100, GemRB.GetGameTime () - hour >= 60], [6, 1, 0x100, True])
	_Jump (rounder, 1362, 186)
	for i in range (6):
		yield
	_Check ("trail, the snare: walked over after going round, it doesn't catch", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetPlayerStat (rounder, IE_HELD)], [6, 0])
	_Jump (rounder, 1785, 1339)
	_SnareSet (0)
	yield
	# Jory's warning (DL_JORY_WARNED: his talk in Delmord, when he lived): the first near the snare remembers, and it is found
	for p in (party[1], thrower, henner, cutter):  # nobody else near it
		_Jump (p, 1785, 1339)
	yield
	GemRB.SetGlobal ("DL_JORY_WARNED", "GLOBAL", 1)
	_Jump (jory, 1250, 330)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_JORY_SAID") == 1 and GemRB.GetDCCTrap ("DlSnare")["Detected"], 20)
	_Check ("trail, the snare: Jory's warning calls it out", [GemRB.GetGameVar ("DL_JORY_SAID"), GemRB.GetDCCTrap ("DlSnare")["Detected"],
		"Jory said" in _Said (jory)], [1, 1, True])
	_Jump (jory, 1785, 1339)
	_SnareSet (0)
	yield
	# a trapper or hunter (trained in Outdoors and Butchery) knows it on sight
	for tag in ("OUTDOORS", "BUTCHERY"):
		GemRB.ExecuteString ('DCCAddSkill(Myself,"%s")' % tag, trapper)
	GemRB.SetGlobal ("DL_TRAPPER_SAID", "GLOBAL", 0)
	yield
	_Jump (trapper, 1250, 330)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_TRAPPER_SAID") == 1 and GemRB.GetDCCTrap ("DlSnare")["Detected"], 20)
	_Check ("trail, the snare: a trapper calls it out on sight", [GemRB.GetGameVar ("DL_TRAPPER_SAID"), GemRB.GetDCCTrap ("DlSnare")["Detected"],
		"Spring snare" in _Said (trapper)], [1, 1, True])
	_Jump (trapper, 1785, 1339)
	# set and found again, for the walker
	_SnareSet (1)
	for i in range (3):
		yield

def _Trail ():
	"""the wooded trail's content, on the party's first visit"""
	party = _Party ()
	# the test looks everywhere at once: the whole map explored (the clicks need it, the screenshots too)
	GemRB.ExploreArea (True)
	# no pole or staff in the party for now (the snare's pole probe springs it on anyone carrying one near it), and
	# no trapper's call-out yet (the party's occupations are the roll's): both are tried on their own below
	for p in party:
		for ref in ("POLE", "STAFF", "POLEW"):
			for i in range (3):
				GemRB.ExecuteString ('DestroyItem("%s")' % ref, p)
	GemRB.SetGlobal ("DL_TRAPPER_SAID", "GLOBAL", 1)
	yield
	ids = dict ((name, GemRB.DCCFindActor (name)) for name in TRAIL_BANDITS)
	_Check ("trail: the cart's four bandits are placed", all (ids.values ()), True)
	if not all (ids.values ()):
		return
	cut1, cut2, cut3, arch = [ids[name] for name in TRAIL_BANDITS]
	_Check ("trail: the bandits are hostile (EA enemy)", [GemRB.GetPlayerStat (i, IE_EA) for i in ids.values ()], [255] * 4)
	_Check ("trail: they haven't noticed the party yet", _AreaVar ("DL_CART_ALERT"), 0)
	for i in range (4):
		yield
	# AC: leather 12, padded 11 and the barricade's cover +2 while the archer keeps his post
	_Check ("trail: AC cutthroats 12, the archer at his post 11 +2 cover", [GemRB.GetDCCStats (i)["AC"] for i in ids.values ()], [12, 12, 12, 13])
	loot = [sorted (ref for ref, n in _Items (i)) for i in ids.values ()]
	_Check ("trail: each bandit's weapon, armour, hood and silver", loot, [["HOOD", "LEATHR", "SILVER", "SSWORD"], ["HOOD", "LEATHR", "SILVER", "SSWORD"],
		["HOOD", "LEATHR", "SILVER", "SPEAR"], ["ARROW", "DAGGER", "HOOD", "PADDED", "SBOW", "SILVER"]])
	# S7: the hood (RH, dcchelm.2da CLOSED 2): no hair drawn under it, each one's beard whole (mutton chops, moustache,
	# full beard, moustache), and the hood's overlay files on the male rig (WQL)
	looks = [GemRB.GetDCCStats (i) for i in ids.values ()]
	_Check ("trail: the bandits wear the hood (RH)", [d["HelmetOverlay"] for d in looks], ["RH"] * 4)
	_Check ("trail: no hair under the hood, the beard whole", [(d["HairOverlay"], d["BeardOverlay"]) for d in looks],
		[("", "BC"), ("", "BM"), ("", "BD"), ("", "BM")])
	_Check ("trail: the hood's overlay file is there (WQLRHG1)", GemRB.HasResource ("WQLRHG1", RES_BAM), True)
	# the gang's colours (CRE pal16 rows): the forest green tunic under the hood (major 17; the hood itself is the HOOD
	# item's forest green, the helmet's major range, colour byte 3), faded black breeches (minor 27), oxblood leather (49);
	# each one's own skin and hair
	gang = [(GemRB.GetPlayerStat (i, IE_MAJOR_COLOR) & 0xff, GemRB.GetPlayerStat (i, IE_MINOR_COLOR) & 0xff, GemRB.GetPlayerStat (i, IE_LEATHER_COLOR) & 0xff,
		GemRB.GetPlayerStat (i, IE_MAJOR_COLOR) >> 24) for i in ids.values ()]
	_Check ("trail: one gang: green tunic, black breeches, oxblood leather, the green hood", gang, [(17, 27, 49, 17)] * 4)
	looks = [(GemRB.GetPlayerStat (i, IE_SKIN_COLOR) & 0xff, GemRB.GetPlayerStat (i, IE_HAIR_COLOR) & 0xff) for i in ids.values ()]
	_Check ("trail: each with his own skin and hair", [len (set (k for k, h in looks)), len (set (h for k, h in looks))], [4, 4])
	silver = [dict (_Items (i)).get ("SILVER", 0) for i in ids.values ()]
	_Check ("trail: 2d6 sp each", all (2 <= n <= 12 for n in silver), True)
	_Check ("trail: the archer's arrows (1d10)", 1 <= dict (_Items (arch)).get ("ARROW", 0) <= 10, True)
	# the bandits' hound by the cart's tail: its own sprite (avatars.2da 0x6600, the IWD monster layout MHND*)
	hound = GemRB.DCCFindActor ("DlHound")
	_Check ("trail: the bandits' hound is placed, its sprite 0x6600", bool (hound) and GemRB.GetPlayerStat (hound, IE_ANIMATION_ID), 0x6600)
	carter = GemRB.DCCFindActor ("DlCarter")
	_Check ("trail: the carter lies dead by his cart", bool (carter and GemRB.GetPlayerStat (carter, IE_STATE_ID) & STATE_DEAD), True)
	_Check ("trail: the carter's 14 cp and knife", _Pile ("DlCarter"), [("COINS", 14), ("KNIFE", 1)])
	_Check ("trail: the cart's loot", _Pile ("DlCart"), [("FLOUR", 1), ("FLOUR", 1), ("BARREL", 1), ("ROPE", 1), ("TORCH", 2), ("FLINT", 1), ("CROWBR", 1)])
	_Check ("trail: the dropped waterskin at the trail's mouth", _Pile ("DlWaterskin"), [("WSKIN", 1)])
	# the screenshots: the hostile are drawn only in the party's sight, and nobody has come near them yet (they would
	# notice), so the game view draws the unseen for these two (its debug flag 0x1000)
	fog = GemRB.GameControlDebugFlags ()
	GemRB.GameControlDebugFlags (fog | 0x1000)
	yield
	_Shot ("trail_cart", cut1)
	_Shot ("trail_archer", arch)
	if hound:
		_Shot ("trail_hound", hound)
	GemRB.GameControlDebugFlags (fog)
	_Check ("trail: still unnoticed", _AreaVar ("DL_CART_ALERT"), 0)

	# the quiet approach: the hound's nose and ears by how one comes (DCCNoticesParty: a run at 40', a walk at 20', a sneak
	# unheard at 10'), tried with the hound led off down the road, out of the band's sight, and one party member
	if hound:
		yield from _Quiet (hound, party[8], party[0])

	# the info points: each one's description, shown by a real click (the examine cursor's click)
	pc = party[0]
	for name in TRAIL_INFO:
		region = GemRB.GetDCCTrap (name)
		text = GemRB.GetOverheadText (name)
		_Check ("trail: info point %s with its string" % name, bool (region and region["Type"] == "region" and text and text["Text"]), True)
	_Check ("trail: the toll board's prices", "TOLL. CART 5 SP. MULE 2 SP. GRANNY 1 SP. ARGUING 1 HEAD." in GemRB.GetOverheadText ("DlTollBoard")["Text"], True)

	# the snare's data (dcctrap.2da SNARE, the ARE's DCs)
	snare = GemRB.GetDCCTrap ("DlSnare")
	_Check ("trail: the snare, woodland, find DC 8, disarm DC 8 by Agi, not deadly, armed", snare and [snare["Kind"], snare["FindDC"], snare["DisarmDC"],
		snare["DisarmAbility"], snare["Deadly"], snare["Trapped"], snare["Detected"]], ["WOODLAND", 8, 8, "AGI", 0, 1, 0])
	_Check ("trail: the snare's rope is off until it springs", GemRB.GetDCCTrap ("DlSnareRope")["Flags"] & 0x100, 0x100)

	# a death: last words over its head (cutthroat 2); the first of their side to fall, so the others near check
	# morale: cutthroat 1 breaks (a forced 1) and runs up the trail and off the map, cutthroat 3 and the hound stand (a 20)
	GemRB.DCCForceDice (cut1, [1])
	GemRB.DCCForceDice (cut3, [20])
	if hound: # S7: the hound checks too; held to a 20, so its own flight (the dice stream moves with any loot change) can't
		# crowd the runaway's path up the trail
		GemRB.DCCForceDice (hound, [20])
	GemRB.ClearActions (pc)
	GemRB.ExecuteString ('Kill("DlCut2")', pc)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_WORDS") >= 1, 20)
	said = GemRB.GetOverheadText (cut2)
	first = said["Text"] if said else ""
	_Check ("trail: a dying bandit barks its last words", [first in LAST_WORDS, GemRB.GetGameVar ("DL_WORDS")], [True, 1])
	_Check ("trail: the word is marked said", first in LAST_WORDS and GemRB.GetGameVar ("DL_WORD%d" % (LAST_WORDS.index (first) + 1)), 1)
	_Check ("trail: the dead drop their things", _Items (cut2), [])
	yield from _Wait (lambda: _AreaVar ("DL_CART_RAN") == 1, 20)
	_Check ("trail: cutthroat 1's morale breaks: he runs for the trail", [_AreaVar ("DL_CART_RAN"), _Logged ("checks morale")], [1, True])
	# he runs (RunToPoint, IE_ANI_RUN 15) with the run's own frames (G10, the run frames found), not the walk's fallback
	yield from _Wait (lambda: GemRB.GetDCCStats (cut1)["Stance"] == 15, 20)
	ran = GemRB.GetDCCStats (cut1)
	_Check ("trail: the runaway runs with the run's frames (stance 15, RunFrames, the G10 file)",
		[ran["Stance"], ran["RunFrames"], ran["StanceFiles"]["Run"][0][-3:].upper ()], [15, 1, "G10"])
	_Check ("trail: at twice the walk (half the step time)", ran["WalkSpeed"] * 2 <= 1500 // 9 + 1, True)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_CART_FLED") == 1, 120)
	if GemRB.DCCFindActor ("DlCut1"):
		_Log ("the runaway: %s" % GemRB.GetActorPosition (cut1))
	_Check ("trail: up the trail and off the map, counted for the camp", [GemRB.GetGameVar ("DL_CART_FLED"), bool (GemRB.DCCFindActor ("DlCut1"))], [1, False])
	_Check ("trail: the runaway strode over the snare", GemRB.GetDCCTrap ("DlSnare")["Trapped"], 1)
	GemRB.DCCForceDice (cut3, [])

	# taken alive: cutthroat 3 knocked out (the subdual blow's state), then neutral, a captive
	GemRB.DCCForceDice (arch, [20])
	GemRB.DCCForceDice (cut3, [20]) # hurt and alone, he checks morale too: he stands
	# (from where the party stands: DCCAttack needs no reach, and nobody comes within the 30' that would warn them)
	boxer = party[6]
	GemRB.ExecuteString ("DCCDropHands(Myself)", boxer)
	GemRB.ApplyEffect (cut3, "CurrentHPModifier", -3, 0)
	yield
	yield
	GemRB.DCCSetCombat (boxer, DCC_OPT_SUBDUAL)
	GemRB.DCCForceDice (boxer, [19, 1])
	rolls = GemRB.DCCAttack (boxer, cut3)
	GemRB.DCCForceDice (boxer, [])
	GemRB.DCCSetCombat (boxer, 0)
	_Check ("trail: a subdual blow with fists knocks cutthroat 3 out", [rolls[0]["Hit"], rolls[0]["Subdual"], GemRB.GetDCCStats (cut3)["Subdued"]], [1, 1, 1])
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_CART_TAKEN") == 1, 20)
	_Check ("trail: knocked out, cutthroat 3 is taken alive (neutral)", [GemRB.GetGameVar ("DL_CART_TAKEN"), GemRB.GetPlayerStat (cut3, IE_EA)], [1, 128])
	# the archer steps back from melee (the band is fighting): a peasant at his elbow, its free swing held to a miss
	GemRB.ExecuteString ('SetGlobal("DL_CART_ALERT","MYAREA",1)', pc)
	near = party[9]
	where = GemRB.GetActorPosition (arch)
	_Jump (near, where["X"] + 48, where["Y"])
	GemRB.DCCForceDice (near, [2] * 6)
	yield from _Wait (lambda: _AreaVar ("DL_ARCH_BACKOFF") >= 1, 30)
	for i in range (12):
		yield
	moved = GemRB.GetActorPosition (arch)
	said = GemRB.GetOverheadText (arch)
	went = ((moved["X"] - where["X"]) / 16.0) ** 2 + ((moved["Y"] - where["Y"]) / 12.0) ** 2
	_Log ("the archer from %d,%d to %d,%d, the peasant at %s" % (where["X"], where["Y"], moved["X"], moved["Y"], GemRB.GetActorPosition (near)))
	_Check ("trail: the archer steps back from the peasant at his elbow (3' and more), and says so",
		[_AreaVar ("DL_ARCH_BACKOFF") >= 1, bool (said and "give me room" in said["Text"]), went >= 9], [True, True, True])
	GemRB.DCCForceDice (near, [])
	GemRB.ClearActions (near)
	# the archer dies: a second, different line
	GemRB.ClearActions (pc)
	GemRB.ExecuteString ('Kill("DlArch")', pc)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_WORDS") >= 2, 20)
	said = GemRB.GetOverheadText (arch)
	second = said["Text"] if said else ""
	_Check ("trail: the archer's last words, not the first's", [second in LAST_WORDS, second != first, GemRB.GetGameVar ("DL_WORDS")], [True, True, 2])
	GemRB.DCCForceDice (arch, [])
	yield from _Wait (lambda: _AreaVar ("DL_CART_OVER") == 1, 20)
	_Check ("trail: the fight is over (dead, run off, taken)", [_AreaVar ("DL_CART_OUT"), _AreaVar ("DL_CART_OVER")], [4, 1])
	_Check ("trail: one ran, so no tracks check", _AreaVar ("DL_TRACKS"), 0)
	# the hound goes down with its band (its fall, DE, then lying dead, TW); alive it would chase the party through
	# the rest of the visit
	if hound:
		GemRB.ClearActions (pc)
		GemRB.ExecuteString ('Kill("DlHound")', pc)
		yield from _Wait (lambda: GemRB.GetPlayerStat (hound, IE_STATE_ID) & STATE_DEAD, 20)
		for i in range (30):  # the 18 frames of its fall, played out
			yield
		_Check ("trail: the hound dies", bool (GemRB.GetPlayerStat (hound, IE_STATE_ID) & STATE_DEAD), True)
		_Shot ("trail_hound_dead", hound)

	# the captive comes to and talks (DLCAPT): the camp for free, the watch by a Per check, then he is left tied
	where = GemRB.GetActorPosition (cut3)
	_Jump (pc, where["X"] + 30, where["Y"] + 20)
	GemRB.ApplyEffect (cut3, "CurrentHPModifier", 2, 0)
	yield from _Wait (_InDialog, 30)
	_Check ("trail: the captive starts talking", _InDialog (), True)
	if _InDialog ():
		_Reply (0) # Where's your camp?
		yield
		yield
		_Reply (GemRB.GetVar ("DialogOption")) # his answer, then on
		yield
		yield
		# (a transition's actions run once the conversation lets the clock go on: checked at its end)
		GemRB.DCCForceDice (pc, [10])
		_Reply (1) # [Military or Deception, Per 10] How is it guarded?
		yield
		yield
		_Reply (GemRB.GetVar ("DialogOption")) # the check: the passing branch
		yield
		yield
		GemRB.DCCForceDice (pc, [])
		_Reply (GemRB.GetVar ("DialogOption")) # his answer, then on
		yield
		yield
		_Reply (4) # Leave him tied.
		yield
		yield
		_Reply (GemRB.GetVar ("DialogOption"))
		yield from _Wait (lambda: not _InDialog () and GemRB.GetGameVar ("DL_CAPTIVE_TIED") == 1, 20)
		_Check ("trail: left tied", [GemRB.GetGameVar ("DL_CAPTIVE_TIED"), _InDialog ()], [1, False])
		_Check ("trail: he gave up the camp for free", GemRB.GetGameVar ("DL_KNOW_CAMP"), 1)
		_Check ("trail: and its watch, to a Per check (a 10, the most an untrained d10 gives)", GemRB.GetGameVar ("DL_KNOW_CAMPWATCH"), 1)
	# S7: the captive in his hood (no hair under it, his full beard), with the one who questioned him beside him
	for i in range (4):
		yield
	_Check ("trail: the captive's hood, no hair, his full beard", [GemRB.GetDCCStats (cut3)[k] for k in ("HelmetOverlay", "HairOverlay", "BeardOverlay")], ["RH", "", "BD"])
	_Shot ("trail_captive", cut3)

	# the info points, once the fight is over: each one's description, shown by a real click (the examine cursor's)
	# the hare in the second snare, by the trail: first, from a spot clear of the snare (it springs only on the party)
	_Jump (pc, 1245, 165)
	for i in range (3):
		yield
	yield from _ClickMap (1284, 98, pc, "DlHare")
	_Check ("trail: a click on DlHare shows its string", [Hover[0], GemRB.GetOverheadText ("DlHare")["Displaying"]], ["DlHare", True])
	clicks = {"DlTollBoard": (617, 160), "DlHeadMan": (110, 300), "DlHeadGranny": (270, 170), "DlHeadYoung": (425, 52), "DlBarricade": (380, 220),
		"DlCartInfo": (1032, 772), "DlBirches": (1100, 1300)}
	for name, (x, y) in clicks.items ():
		# the one who looks stands near enough to see it (a click where the party can't see does nothing)
		# (and off to one side and below: its own sprite would take the click)
		_Jump (pc, x + 60, y + 150)
		for i in range (3):
			yield
		yield from _ClickMap (x, y, pc, name)
		_Check ("trail: a click on %s shows its string" % name, [Hover[0], GemRB.GetOverheadText (name)["Displaying"]], [name, True])
		if name == "DlTollBoard":
			_Shot ("trail_toll")


	# Detect traps: a searcher by the snare finds it (a 20), with the notice line and the game's first tell's tip
	searcher, walker, cutter = party[1], party[2], party[3]
	_Jump (searcher, 1300, 300)
	yield
	yield
	GemRB.DCCForceDice (searcher, [20])
	GemRB.SetModalState (searcher, MS_DETECTTRAPS)
	yield from _Wait (lambda: GemRB.GetDCCTrap ("DlSnare")["Detected"], 20)
	GemRB.DCCForceDice (searcher, [])
	_Unpause ()
	name = GemRB.GetPlayerName (searcher, 0)
	_Check ("trail: a Detect traps searcher finds the snare", [GemRB.GetDCCTrap ("DlSnare")["Detected"], _Logged (name + " finds a trap: 20")], [1, True])
	_Check ("trail: the notice line, once", [GemRB.GetDCCTrap ("DlSnare")["Notice"], _Logged ("That birch is bent over the trail like a fishing rod.")], [1, True])
	yield
	_Check ("trail: the snare's script got Detected; the tip was given", [GemRB.GetGameVar ("DL_SNARE_FOUND"), GemRB.GetGameVar ("DCC_TRAP_TIP")], [1, 1])
	GemRB.SetModalState (searcher, MS_NONE)
	_Check ("trail: found, the snare's handle is on", GemRB.GetDCCTrap ("DlSnareHandle")["Flags"] & 0x100, 0)

	# the ways past it (the handle's dialog DLSNHAND, the pole probe, the call-outs), each tried on the snare set again
	yield from _SnareWays (party, cutter)

	# sprung on a walker: held by one ankle, what it holds falls, 1d10 cp out of the purse onto the ground
	gold = GemRB.GameGetPartyGold ()
	held = _Items (walker)
	anim = GemRB.GetPlayerStat (walker, IE_ANIMATION_ID)
	_Jump (walker, 1362, 186)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE") == 1 and GemRB.GetGameVar ("DL_SNARE_CP") > 0, 20)
	cp = GemRB.GetGameVar ("DL_SNARE_CP")
	_Check ("trail: the walker springs the snare and hangs, held", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetPlayerStat (walker, IE_HELD), GemRB.GetDCCTrap ("DlSnare")["Trapped"]], [1, 1, 0])
	ground = _GroundAt (walker)
	_Check ("trail: 1d10 cp fall from the purse onto the ground", [1 <= cp <= 10, gold - GemRB.GameGetPartyGold (), ("COINS", cp) in ground], [True, cp, True])
	dropped = [item for item in held if item not in _Items (walker)]
	_Check ("trail: it drops what it held, there too", bool (dropped) and all (item in ground for item in dropped) or not held, True)
	_Check ("trail: the rope can be clicked now", GemRB.GetDCCTrap ("DlSnareRope")["Flags"] & 0x100, 0)
	# hung under the birch's tip: the hanging body by sex (avatars.2da 0x6502, 0x6512), the birch stood up (the patch)
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE_BODY") > 0 and _AreaVar ("DL_SPRUNG_SHOWN") > 0, 20)
	for i in range (3):
		yield
	female = GemRB.GetPlayerStat (walker, IE_SEX) == 2
	race = GemRB.GetPlayerStat (walker, IE_RACE)
	body = {4: 0x6522, 5: 0x6522, 2: 0x6542}.get (race, 0x6502) + (0x10 if female else 0)  # the dwarves' bodies for halflings too
	at = GemRB.GetActorPosition (walker)
	_Check ("trail: it hangs head down in its race's and sex's hanging body, at the spot under the birch's tip, the birch stood up",
		[GemRB.GetPlayerStat (walker, IE_ANIMATION_ID), abs (at["X"] - 1344) + abs (at["Y"] - 179) < 24, _AreaVar ("DL_SPRUNG_SHOWN") in (1, 2)],
		[body, True, True])
	# what it held lies on the ground under it (the pile's icon), not in its hand: the hanging body draws no overlays
	_Check ("trail: hanging, it holds nothing (no weapon overlay)", GemRB.GetDCCStats (walker)["WeaponOverlay"], "")
	_Shot ("trail_snare", walker)
	yield from _ShotAt ("trail_snare_quiet", 1344, 179)
	# by night: the night tiles and the sprung birch's night patch (DayNight: to 23:00, then back to the next morning)
	GemRB.ExecuteString ("DayNight(23)", 1)
	yield from _Wait (lambda: _AreaVar ("DL_SPRUNG_SHOWN") == 2, 30)
	_Check ("trail: by night, the night patch", _AreaVar ("DL_SPRUNG_SHOWN"), 2)
	yield from _ShotAt ("trail_snare_night", 1344, 179, 6)
	GemRB.ExecuteString ("DayNight(10)", 1)
	yield from _Wait (lambda: _AreaVar ("DL_SPRUNG_SHOWN") == 1, 30)
	_Check ("trail: and back by day, the day patch", _AreaVar ("DL_SPRUNG_SHOWN"), 1)
	# cut down by one with a knife: a click on the rope, a round's sawing
	GemRB.CreateItem (cutter, "KNIFE")
	_Jump (cutter, 1450, 270)
	for i in range (3):
		yield
	yield from _ClickMap (1420, 195, cutter, "DlSnareRope")
	yield from _Wait (lambda: GemRB.GetGameVar ("DL_SNARE") == 4, 60)
	yield
	_Check ("trail: cut down with a knife", [GemRB.GetGameVar ("DL_SNARE"), GemRB.GetPlayerStat (walker, IE_HELD)], [4, 0])
	_Check ("trail: cut down, its own body again", GemRB.GetPlayerStat (walker, IE_ANIMATION_ID), anim)

	# the herb patch: a click is the party's Herbalism check (Int DC 10); a pass leaves two poultices
	_Jump (pc, 910, 1450)
	for i in range (3):
		yield
	yield from _ClickMap (900, 1370, pc, "DlHerbs")
	yield from _Wait (lambda: _AreaVar ("DL_HERBS") in (1, 2), 20)
	herbs = _AreaVar ("DL_HERBS")
	_Check ("trail: the herb patch's Herbalism check", [herbs in (1, 2), _Logged ("Herbalism check for the party")], [True, True])
	yield
	yield
	_Check ("trail: the poultices to the one who looked, on a pass", ("POULTC", 2) in _Items (pc), herbs == 1)

	# the trail's mouth: the prints, then the deer path check (Outdoors, Per DC 12)
	_Jump (party[4], 953, 513)
	yield from _Wait (lambda: _AreaVar ("DL_GAP") == 2 and _AreaVar ("DL_DEERPATH_ROLL") in (1, 2), 20)
	_Check ("trail: the prints at the trail's mouth, the deer path check", [_AreaVar ("DL_GAP"), _AreaVar ("DL_DEERPATH_ROLL") in (1, 2)], [2, True])

	# the party walks up the trail, past the sprung snare, to its top
	GemRB.GameSelectPC (0, 1)
	for p in party:
		GemRB.ClearActions (p)
	GemRB.GameControlMoveSelected (1480, 90)
	near = lambda: any (abs (GemRB.GetActorPosition (p)["X"] - 1480) + abs (GemRB.GetActorPosition (p)["Y"] - 90) < 120 for p in party)
	yield from _Wait (near, 80)
	_Check ("trail: the party walks up the trail", near (), True)
	_Shot ("trail_top", party[0])

def _InDialog ():
	import GUIWORLD
	return GUIWORLD.ContinueWindow is not None

def _Reply (transition):
	"""picks a transition of the dialog's current state, as a click on a reply (or Continue) does"""
	GemRB.SetVar ("DialogChoose", transition)

def _Click (win, control, button = 1, mod = 0):
	"""a real click on a control (GemRB.TestClick: through the windows as a player's would go)"""
	outer, frame = win.GetFrame (), control.GetFrame ()
	GemRB.TestClick (outer['x'] + frame['x'] + frame['w'] // 2, outer['y'] + frame['y'] + frame['h'] // 2, button, mod)

def _ClickTile (slot, button = 1, mod = 0):
	import Roster
	_Click (Roster.RosterWindow, Roster.TileControl (slot), button, mod) # the party column's tile, scrolled to it

def _RosterClicks ():
	"""the roster's tiles by the mouse (U2): a click selects that one, shift-click and a right click add or
	take one away"""
	import Roster
	_Check ("the roster is open", bool (Roster.RosterWindow), True)
	GemRB.GameSelectPC (0, 1)
	_ClickTile (5)
	yield
	_Check ("roster: a click on tile 5 selects only it", Roster.Selected (), [5])
	_ClickTile (6, 1, GEM_MOD_SHIFT)
	yield
	_Check ("roster: shift-click adds tile 6", Roster.Selected (), [5, 6])
	_ClickTile (5, GEM_MB_MENU)
	yield
	_Check ("roster: a right click takes tile 5 away", Roster.Selected (), [6])
	_Check ("roster: tile 6 shows selected", Roster.Tiles[6]["Selected"], True)

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
