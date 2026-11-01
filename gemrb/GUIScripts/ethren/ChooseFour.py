# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.3): "Choose your four" (docs/architecture/ui-design.md, "Funnel screens"), and the end of
# the funnel as a whole. The funnel ends when a script sets the global DCC_FUNNEL to 1 (the adventure's
# last scene; funneltest's autotest does it with a script action). Then:
#   1  this screen: the survivors as cards (abilities, occupation, HP, alignment and the funnel's record:
#      kills and near deaths), sortable by any ability, Luck, HP, kills or near deaths. The player picks
#      up to four (fewer is allowed) and may change a card's alignment (the player's choice, the roll was
#      only a default); in co-op the host also says whose each one is. Accept ends the funnel
#      (GemRB.DCCEndFunnel): every survivor gets the XP for level 1 with its class pick pending, the picks
#      are the party, the others wait in the reserves (their pick when they join), the dead leave.
#   2  the level-1 class picks (ClassPick.py): each player picks a class for each of its own four, with
#      the spells (DCCLearn.py) for casters;
#   3  the end screen (FunnelEnd.py): the four, the reserves, the graveyard.
# Nobody alive at all: the screen says so and the end screen closes the game (Ethren decision).
# Watch () runs on the roster's timer, on the host and on co-op clients alike (the global replicates):
# clients see the cards read-only (their own alignment they may change), then pick their own classes.
# Built from code in the demo's plain colours, like the rolling screen; Phase 5 skins it.

import GemRB
import FunnelRoll
import FunnelChoose
from GUIDefines import *

FUNNEL_VAR = "DCC_FUNNEL" # 0 the funnel runs, 1 choose the four, 2 the class picks, 3 the end screen, 4 done
CAMP_VAR = "DCC_CAMP" # 1: the reserves' roster is open (a camp or tavern script sets it)

Font = "NORMAL"
CardFont = "INFOFONT"
WindowColor = {'r': 18, 'g': 16, 'b': 22, 'a': 255}
CardColor = {'r': 46, 'g': 41, 'b': 34, 'a': 255}
PickedColor = {'r': 92, 'g': 78, 'b': 40, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
LitColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}
WINDOW_ID = 80
COLUMNS = 4
GAP = 6
TOP = 76
CARD_BASE = 1000 # card i: 1000 + 10 i + 0 backdrop, 1 name, 2 pick, 3 player, 4 alignment, 5 text

Window = None
Survivors = []
Picks = [] # global IDs, in the order picked
SortKey = "Order"
Mode = None # "single", "host" or "client"
Shown = {} # for tests: "Cards" (ids in the order shown), id -> [name, lines...], "Status", "Picks"
TableCache = None
EndShown = False
CampShown = False

def _Log (text):
	print ("[Choose] " + text)

def Tables ():
	global TableCache
	if TableCache is None:
		TableCache = dict ((name, FunnelRoll.ReadGemRBTable (name)) for name in ("dccoccup", "dccoccit", "dccclass", "dccgods"))
	return TableCache

def State ():
	return GemRB.GetGameVar (FUNNEL_VAR)

def SetState (state):
	GemRB.SetGlobal (FUNNEL_VAR, "GLOBAL", state)

def NetMode ():
	mode = GemRB.NetGetMode ()
	return mode if mode else "single"

def IsHost ():
	return NetMode () != "client"

def MySlot ():
	lobby = GemRB.NetGetLobby () if GemRB.NetGetMode () else None
	return lobby["Slot"] if lobby else 0

def Players ():
	lobby = GemRB.NetGetLobby () if GemRB.NetGetMode () else None
	return [player["Slot"] for player in lobby["Players"]] if lobby else []

###################################################
# who is left

def LivingSurvivors ():
	"""the party members still alive in the funnel, as cards (by global ID)"""
	cards = []
	for pc in range (1, GemRB.GetPartySize () + 1):
		card = FunnelChoose.Survivor (pc, Tables ())
		if not card["Dead"]:
			cards.append (card)
	return cards

###################################################
# the flow (the roster's timer)

def Watch ():
	"""opens what the funnel's state asks for; on the host it moves the state on"""
	global EndShown, CampShown
	state = State ()
	# windows that went with the game (a load; in co-op the host's new save after the party changed)
	import ClassPick, Reserves, FunnelEnd, DCCLearn
	for module in (ClassPick, Reserves, FunnelEnd, DCCLearn):
		if module.Window and not _Valid (module.Window):
			module.Window = None
	if Window and not _Alive ():
		Close ()
	if state == 1 and not Window:
		Open ()
	elif state == 1:
		Refresh ()
	elif state != 1 and Window:
		Close ()
	if state == 2:
		import ClassPick
		ClassPick.Next ()
		if IsHost () and not ClassPick.AnyPending () and not ClassPick.Busy ():
			_Log ("every class pick is made: the funnel's end")
			SetState (3)
	if state == 3 and not EndShown:
		import FunnelEnd
		EndShown = True
		FunnelEnd.Open ()
	elif state < 3:
		EndShown = False
	if state >= 2:
		# later swaps: a reserve that joins at level 0 picks its class too
		import ClassPick
		ClassPick.Next ()
	camp = GemRB.GetGameVar (CAMP_VAR)
	if camp and not CampShown:
		import Reserves
		CampShown = True
		Reserves.Open ()
	elif not camp and CampShown:
		import Reserves
		CampShown = False
		Reserves.Close ()

###################################################
# the window

def Open ():
	global Window, Mode, Survivors, Picks
	Close ()
	Mode = NetMode ()
	Survivors = LivingSurvivors ()
	Picks = [p for p in Picks if p in [s["Id"] for s in Survivors]]
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': 0, 'y': 0, 'w': screen[0], 'h': screen[1]})
	win.AddAlias ("CHOOSEWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, GAP, 6, 300, 30, Font, "Choose your four", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, 260, 6, screen[0] - 260 - 2 * 140, 30, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	if Mode != "client":
		accept = _Button (win, 1, screen[0] - GAP - 134, 8, 134, 28, "Accept", Accept, "These go on; the rest go to the reserves")
		accept.MakeDefault ()
	# the sort buttons
	x = GAP
	win.CreateLabel (0x10000002, x, 42, 50, 26, CardFont, "Sort:", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	x += 52
	for i, (key, label) in enumerate (FunnelChoose.SORTS):
		w = 46 if len (label) < 6 else 96
		_Button (win, 20 + i, x, 42, w, 26, label, lambda key = key: SortBy (key), "Sort by %s" % label.lower (), CardFont)
		x += w + 4
	count = len (Survivors)
	if not count:
		win.CreateLabel (0x10000003, GAP, TOP + 40, screen[0] - 2 * GAP, 60, Font, "Nobody survived the funnel.", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	for i in range (count):
		x, y, w, h = _Frame (i, count)
		base = CARD_BASE + 10 * i
		win.CreateLabel (base + 1, x + 6, y + 4, w - 190, 24, Font, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, base + 2, x + w - 182, y + 4, 60, 24, "", lambda i = i: TogglePick (i), "Pick this one to go on", CardFont)
		_Button (win, base + 3, x + w - 118, y + 4, 36, 24, "", lambda i = i: DealNext (i), "Whose this one is: press for the next player", CardFont)
		_Button (win, base + 4, x + w - 78, y + 4, 72, 24, "", lambda i = i: NextAlignment (i), "Alignment: the player's choice", CardFont)
		win.CreateLabel (base + 5, x + 8, y + 32, w - 14, h - 36, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
		backdrop = _Button (win, base, x, y, w, h, "", None)
		backdrop.SetState (IE_GUI_BUTTON_LOCKED)
		backdrop.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND)
		backdrop.SetBackground (CardColor)
	Window = win
	_Log ("%d survivor(s): %s" % (count, ", ".join (s["Name"] for s in Survivors)))
	Refresh ()
	win.Focus ()
	return win

def _Frame (index, count):
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	rows = (count + COLUMNS - 1) // COLUMNS
	w = (screen[0] - (COLUMNS + 1) * GAP) // COLUMNS
	h = min (150, (screen[1] - TOP - (rows + 1) * GAP) // max (rows, 1))
	column, row = index % COLUMNS, index // COLUMNS
	return GAP + column * (w + GAP), TOP + GAP + row * (h + GAP), w, h

def _Button (win, id, x, y, w, h, text, action, tooltip = None, font = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (font or Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	if action:
		button.OnPress (action)
	if tooltip:
		button.SetTooltip (tooltip)
	return button

def _Control (index, part):
	try:
		return Window.GetControl (CARD_BASE + 10 * index + part) if Window else None
	except Exception:
		return None

def _Valid (window):
	try:
		window.GetFrame ()
		return True
	except Exception:
		return False

def _Alive ():
	try:
		return bool (Window and Window.GetControl (0x10000001))
	except Exception:
		return False

def _Owner (survivor):
	"""co-op: whose the character is (its funnel record's player)"""
	return survivor["Player"] if survivor["Player"] >= 0 else 0

def Refresh ():
	global Survivors
	if not _Alive ():
		return
	# the cards follow the characters (an alignment changed, the host dealt one)
	fresh = dict ((s["Id"], s) for s in LivingSurvivors ())
	Survivors = [fresh[s["Id"]] for s in Survivors if s["Id"] in fresh]
	ordered = FunnelChoose.Sorted (Survivors, SortKey)
	coop = Mode != "single"
	me = MySlot ()
	for key in list (Shown):
		del Shown[key]
	Shown["Cards"] = [s["Id"] for s in ordered]
	for index, survivor in enumerate (ordered):
		lines = FunnelChoose.CardLines (survivor)
		_Control (index, 1).SetText (survivor["Name"])
		_Control (index, 5).SetText ("\n".join (lines))
		picked = survivor["Id"] in Picks
		pick = _Control (index, 2)
		pick.SetText ("#%d" % (Picks.index (survivor["Id"]) + 1) if picked else "Pick")
		pick.SetBackground (LitColor if picked else ButtonColor)
		pick.SetVisible (Mode != "client")
		owner = _Owner (survivor)
		player = _Control (index, 3)
		player.SetVisible (coop)
		backdrop = _Control (index, 0)
		if coop:
			import Roster
			colour = Roster.PLAYER_COLORS[owner % len (Roster.PLAYER_COLORS)]
			player.SetText ("P%d" % owner)
			player.SetColor (colour)
			player.SetState (IE_GUI_BUTTON_ENABLED if Mode == "host" else IE_GUI_BUTTON_LOCKED)
			backdrop.SetBorder (0, colour, 1, 0)
		backdrop.SetBackground (PickedColor if picked else CardColor)
		align = _Control (index, 4)
		align.SetText (FunnelRoll.ALIGNMENTS.get (survivor["Alignment"], "?"))
		mine = Mode != "client" or owner == me
		align.SetState (IE_GUI_BUTTON_ENABLED if mine else IE_GUI_BUTTON_LOCKED)
		Shown[survivor["Id"]] = [survivor["Name"]] + lines
	for i in range (len (FunnelChoose.SORTS)):
		Window.GetControl (20 + i).SetBackground (LitColor if FunnelChoose.SORTS[i][0] == SortKey else ButtonColor)
	limit = FunnelChoose.Limit (Survivors)
	if not Survivors:
		text = "The funnel took them all."
	elif Mode == "client":
		text = "The host is choosing who goes on; you may change your own characters' alignment."
	else:
		text = "Every survivor reaches level 1. Pick up to %d to go on now (%d picked)%s; the others wait in the reserves." % (limit, len (Picks), ", as fewer than four survived" if limit < FunnelChoose.PARTY else "")
	Shown["Status"] = text
	Shown["Picks"] = list (Picks)
	Window.GetControl (0x10000001).SetText (text)
	if Mode != "client":
		accept = Window.GetControl (1)
		accept.SetText ("Accept" if Survivors else "The end")
		accept.SetState (IE_GUI_BUTTON_ENABLED if FunnelChoose.Ready (Picks, Survivors) or not Survivors else IE_GUI_BUTTON_DISABLED)

def _Shown (index):
	"""the survivor on card index, as it is sorted now"""
	ordered = FunnelChoose.Sorted (Survivors, SortKey)
	return ordered[index] if 0 <= index < len (ordered) else None

def SortBy (key):
	global SortKey
	SortKey = key
	Refresh ()

def TogglePick (index):
	global Picks
	survivor = _Shown (index)
	if not survivor or Mode == "client":
		return False
	Picks = FunnelChoose.Toggle (Picks, survivor["Id"], Survivors)
	Refresh ()
	return survivor["Id"] in Picks

def Pick (ident):
	"""for tests and scripts: picks a survivor by global ID"""
	global Picks
	Picks = FunnelChoose.Toggle (Picks, ident, Survivors) if ident not in Picks else Picks
	Refresh ()
	return ident in Picks

def NextAlignment (index):
	survivor = _Shown (index)
	if not survivor:
		return False
	return SetAlignment (survivor["Id"], FunnelRoll.NextAlignment (survivor["Alignment"]))

def SetAlignment (ident, code):
	"""the player's choice (routed to the host in co-op, its owner only); the god follows"""
	ok = GemRB.DCCSetAlignment (ident, code)
	_Log ("%s: alignment %s: %s" % (GemRB.GetPlayerName (ident, 0), FunnelRoll.ALIGNMENTS.get (code), ok))
	Refresh ()
	return ok

def DealNext (index):
	survivor = _Shown (index)
	players = Players ()
	if not survivor or Mode != "host" or not players:
		return False
	current = _Owner (survivor)
	following = players[(players.index (current) + 1) % len (players)] if current in players else players[0]
	return Deal (survivor["Id"], following)

def Deal (ident, player):
	"""the host says whose a character is (its funnel record; the party slot follows it)"""
	ok = GemRB.DCCSetPlayer (ident, player)
	_Log ("%s dealt to player %d: %s" % (GemRB.GetPlayerName (ident, 0), player, ok))
	Refresh ()
	return ok

def Accept ():
	"""the four go on (the funnel's end in the engine), or with nobody left the game ends"""
	if Mode == "client":
		return False
	if not Survivors:
		_Log ("nobody survived: the end")
		Close ()
		SetState (3)
		import FunnelEnd
		FunnelEnd.Open (gameOver = True)
		return True
	if not FunnelChoose.Ready (Picks, Survivors):
		return False
	names = [GemRB.GetPlayerName (p, 0) for p in Picks]
	result = GemRB.DCCEndFunnel (list (Picks))
	_Log ("accepted: %s -> %d" % (", ".join (names), result))
	if result != 0:
		return False
	SetState (2)
	Close ()
	return True

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass # gone with the game already (a load, the host's new save in co-op)
		Window = None
