# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.1): the funnel's rolling screen, "Rolling the sixteen" (docs/architecture/ui-design.md,
# "Funnel screens"). A new game opens it before the game exists (Start.NewGame, dccstart.2da ROLL 1):
# sixteen cards, four by four, each a peasant rolled by FunnelRoll.py on the engine's dice: the six
# abilities with their modifiers, the occupation with its weapon and trade good, HP, copper, the birth
# augur with its Luck modifier. The name on a card can be changed; Reroll all is always there, the house
# rules (reroll one card, 4d6 drop the lowest) are off until the Options turn them on. Accept hands the
# cards to SetupGame, which makes the party.
#
# Co-op: the host opens it from the lobby's Start (NetLobby.BeforeStart), while everyone waits in the
# lobby, and deals the cards: a card's player button hands its party slot to the next player (the
# lobby's own slot split, NetAssign, M2.9). The cards go to everyone with the lobby (NetSetLobbyCards,
# protocol v14); clients see them read-only, theirs marked (OpenView, WatchLobby). Only the host rolls
# and accepts; accepting starts the game, and each peasant's funnel record keeps who it was dealt to.
# Built from code in the demo's plain colours; Phase 5 skins it.

import GemRB
import FunnelRoll
from GUIDefines import *

Font = "NORMAL"
CardFont = "INFOFONT"
Cursor = "CAROT"
WindowColor = {'r': 18, 'g': 16, 'b': 22, 'a': 255}
CardColor = {'r': 46, 'g': 41, 'b': 34, 'a': 255}
MineColor = {'r': 70, 'g': 62, 'b': 40, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
OffColor = {'r': 45, 'g': 40, 'b': 40, 'a': 255}
EditColor = {'r': 30, 'g': 27, 'b': 24, 'a': 255}
WINDOW_ID = 97
OPTIONS_ID = 98
COLUMNS = 4
GAP = 6
TOP = 44
CARD_BASE = 1000 # card i's controls: 1000 + 10 i + 0 backdrop, 1 name, 2 player, 3 reroll, 4 text

Cards = None # the current roll: FunnelRoll cards, in roster order
Options = FunnelRoll.Options ()
Tables = None
Window = None
OptionsWindow = None
Mode = None # "single", "host" or "client"
OnAccept = None # single: starts the game; host: NetLobby's start, False if it can't
OnBack = None
Accepted = None # the cards SetupGame makes the party from (None: not rolled)
AcceptedOwners = None # co-op: roster order -> the player it was dealt to
Published = None # what the lobby has (host)
Watching = False
NamesToShow = set () # cards whose name field must be filled again (a new roll, a rename)
Shown = {} # what the screen shows, for tests: order -> [name, lines...], "Status", "Owners"

def _Log (text):
	print ("[Roll] " + text)

###################################################
# dccstart.2da

StartTable = None

def StartValue (row, default = None):
	global StartTable
	if StartTable is None:
		try:
			StartTable = FunnelRoll.ReadGemRBTable ("dccstart")
		except Exception:
			StartTable = FunnelRoll.Table ([], [])
	return StartTable.Get (row, "VALUE", default)

def Rolling ():
	"""a new game rolls the funnel's peasants (dccstart.2da ROLL 1)"""
	return StartValue ("ROLL", "0") == "1"

def PartySize ():
	size = int (StartValue ("PARTY", "16"))
	most = GemRB.GetVar ("MaxPartySize") or size
	return max (1, min (size, most))

def _Tables ():
	global Tables
	if Tables is None:
		Tables = FunnelRoll.LoadTables ()
	return Tables

###################################################
# rolling

def RollAll ():
	global Cards
	Cards = FunnelRoll.RollAll (FunnelRoll.EngineDie, _Tables (), PartySize (), Options)
	NamesToShow.update (range (len (Cards)))
	_Log ("rolled %d (%s)" % (len (Cards), "4d6 drop the lowest" if Options.fourDice else "3d6 in order"))
	for card in Cards:
		_Log ("  " + FunnelRoll.Summary (card))
	_Changed ()
	return Cards

def RerollOne (index):
	"""the house rule (Options.rerollOne): a new peasant in one card's place"""
	if not Options.rerollOne or not Cards or not 0 <= index < len (Cards):
		return None
	_ReadNames ()
	card = FunnelRoll.Reroll (FunnelRoll.EngineDie, _Tables (), Cards, index, Options)
	NamesToShow.add (index)
	_Log ("rerolled card %d: %s" % (index + 1, FunnelRoll.Summary (card)))
	_Changed ()
	return card

def Rename (index, name):
	name = name.strip ()
	if not Cards or not name or not 0 <= index < len (Cards):
		return False
	Cards[index]["Name"] = name[:32]
	NamesToShow.add (index)
	_Changed ()
	return True

def SetOption (name, on):
	setattr (Options, name, bool (on))
	_Log ("house rule %s %s" % (name, "on" if on else "off"))
	_ShowOptions ()
	Refresh ()

###################################################
# co-op: the deal

def _Lobby ():
	return GemRB.NetGetLobby () if GemRB.NetGetMode () else None

def Owners ():
	"""roster order -> the player the card is dealt to (co-op), {} otherwise"""
	lobby = _Lobby ()
	if not lobby:
		return {}
	return dict ((slot["Slot"], slot["Owner"]) for slot in lobby["Party"])

def Deal (order, player):
	"""the host hands a card (its party slot) to a player"""
	if Mode != "host":
		return False
	ok = GemRB.NetAssign (order, player)
	_Log ("card %d dealt to player %d: %s" % (order, player, ok))
	Refresh ()
	return ok

def DealNext (order):
	lobby = _Lobby ()
	if not lobby or Mode != "host":
		return
	players = [player["Slot"] for player in lobby["Players"]]
	current = Owners ().get (order, 0)
	following = players[(players.index (current) + 1) % len (players)] if current in players else players[0]
	Deal (order, following)

def _Publish ():
	"""host: the cards to everyone in the lobby when they change"""
	global Published
	if Mode != "host" or not Cards:
		return
	texts = [FunnelRoll.CardText (card) for card in Cards]
	if texts != Published:
		Published = texts
		GemRB.NetSetLobbyCards (texts)

def _Changed ():
	_Publish ()
	Refresh ()

###################################################
# the window

def Open (onAccept, onBack = None):
	"""single player: roll and show; onAccept starts the game"""
	global Mode, OnAccept, OnBack
	Mode, OnAccept, OnBack = "single", onAccept, onBack
	if not Cards:
		RollAll ()
	return _Build ()

def OpenForLobby (startGame):
	"""co-op host (NetLobby.BeforeStart): roll, deal, then startGame () starts it"""
	global Mode, OnAccept, OnBack, Published
	Mode, OnAccept, OnBack = "host", startGame, _BackToLobby
	Published = None
	if not Cards:
		RollAll ()
	_Publish ()
	return _Build ()

def OpenView ():
	"""co-op client: the host's cards, read-only"""
	global Mode
	Mode = "client"
	return _Build ()

def _Frame (index, count):
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	rows = (count + COLUMNS - 1) // COLUMNS
	w = (screen[0] - (COLUMNS + 1) * GAP) // COLUMNS
	h = min (170, (screen[1] - TOP - (rows + 1) * GAP) // max (rows, 1))
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

def _CardCount ():
	if Mode == "client":
		lobby = _Lobby ()
		return len ([slot for slot in lobby["Party"] if slot["Card"]]) if lobby else 0
	return len (Cards or [])

def _Build ():
	global Window
	Close ()
	for key in list (Shown):
		del Shown[key]
	NamesToShow.update (range (len (Cards or [])))
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': 0, 'y': 0, 'w': screen[0], 'h': screen[1]})
	win.AddAlias ("ROLLWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, GAP, 6, 360, 30, Font, "Rolling the sixteen", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, 300, 6, screen[0] - 300 - 4 * 132, 30, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	if Mode != "client":
		right = screen[0] - GAP
		for id, text, action, tip in ((4, "Accept", Accept, "Make these the funnel's peasants"),
				(3, "Back", Back, None), (2, "Reroll all", RerollAll, "Roll all of them again"),
				(1, "Options", OpenOptions, "House rules: reroll one card, 4d6 drop the lowest")):
			_Button (win, id, right - 124, 8, 124, 28, text, action, tip)
			right -= 130
		win.GetControl (3).MakeEscape ()
	count = _CardCount ()
	for i in range (count):
		x, y, w, h = _Frame (i, count)
		base = CARD_BASE + 10 * i
		if Mode == "client":
			win.CreateLabel (base + 1, x + 6, y + 4, w - 70, 24, Font, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		else:
			edit = win.CreateSubview (base + 1, IE_GUI_EDIT, {'x': x + 6, 'y': y + 4, 'w': w - 130, 'h': 24}, (Font, 32, Cursor))
			edit.SetBackground (EditColor)
			edit.SetTooltip ("The peasant's name: type to change it")
		player = _Button (win, base + 2, x + w - 120, y + 4, 54, 24, "", lambda order = i + 1: DealNext (order), None, CardFont)
		if Mode != "host":
			player.SetState (IE_GUI_BUTTON_LOCKED)
		else:
			player.SetTooltip ("Who plays this peasant: press to deal it to the next player")
		reroll = _Button (win, base + 3, x + w - 60, y + 4, 54, 24, "Reroll", lambda index = i: RerollOne (index), "Roll this card again (house rule)", CardFont)
		reroll.SetVisible (False)
		win.CreateLabel (base + 4, x + 8, y + 32, w - 14, h - 36, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
		# the card itself, made last: a new view goes behind the ones already there
		backdrop = _Button (win, base, x, y, w, h, "", None)
		backdrop.SetState (IE_GUI_BUTTON_LOCKED)
		backdrop.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND) # its border (the owner's colour) needs it off
		backdrop.SetBackground (CardColor)
	Window = win
	Refresh ()
	win.Focus ()
	if Mode != "client":
		_StartTimer ()
	return win

def _Control (index, part):
	if not Window:
		return None
	try:
		return Window.GetControl (CARD_BASE + 10 * index + part)
	except Exception:
		return None

def _ClientCards ():
	"""client: the lobby's cards as [name, lines...] by roster order"""
	lobby = _Lobby ()
	if not lobby:
		return {}
	return dict ((slot["Slot"], slot["Card"].split ("\n")) for slot in lobby["Party"] if slot["Card"])

def Refresh ():
	if not Window:
		return
	try:
		if not Window.GetControl (0x10000001):
			return
	except Exception:
		return
	owners = Owners ()
	lobby = _Lobby ()
	me = lobby["Slot"] if lobby else 0
	coop = bool (lobby)
	if Mode == "client":
		cards = _ClientCards ()
	else:
		cards = dict ((card["Order"], [card["Name"]] + FunnelRoll.CardLines (card)) for card in Cards or [])
	for order, lines in cards.items ():
		index = order - 1
		name, body = _Control (index, 1), _Control (index, 4)
		if not name or not body:
			continue
		# the host's own name fields only when the card changed under them (typing is read by _ReadNames)
		if Mode == "client" or index in NamesToShow:
			name.SetText (lines[0])
			NamesToShow.discard (index)
		body.SetText ("\n".join (lines[1:]))
		owner = owners.get (order, 0)
		player, backdrop, reroll = _Control (index, 2), _Control (index, 0), _Control (index, 3)
		player.SetVisible (coop)
		if coop:
			import Roster
			colour = Roster.PLAYER_COLORS[owner % len (Roster.PLAYER_COLORS)]
			player.SetText ("P%d%s" % (owner, " you" if owner == me and Mode == "client" else ""))
			player.SetColor (colour)
			backdrop.SetBorder (0, colour, 1, 0)
			backdrop.SetBackground (MineColor if owner == me and Mode == "client" else CardColor)
		reroll.SetVisible (Mode != "client" and Options.rerollOne)
		Shown[order] = lines
	Shown["Owners"] = dict ((order, owners.get (order, 0)) for order in cards) if coop else {}
	status = Window.GetControl (0x10000001)
	if Mode == "client":
		mine = sorted (order for order in cards if owners.get (order) == me)
		text = "The host is rolling and dealing; yours: %s" % (", ".join (str (o) for o in mine) or "none yet")
	else:
		rules = [name for name, on in (("reroll one", Options.rerollOne), ("4d6 drop the lowest", Options.fourDice)) if on]
		text = "Rename a peasant on its card. House rules: %s." % (", ".join (rules) if rules else "none")
		if Mode == "host":
			counts = {}
			for order in cards:
				counts[owners.get (order, 0)] = counts.get (owners.get (order, 0), 0) + 1
			text = "Deal the cards: %s. " % ", ".join ("P%d %d" % item for item in sorted (counts.items ())) + text
	Shown["Status"] = text
	status.SetText (text)

def _ReadNames ():
	"""the names typed on the cards into the cards"""
	if not Cards or Mode == "client":
		return
	changed = False
	for index, card in enumerate (Cards):
		edit = _Control (index, 1)
		if not edit:
			continue
		name = (edit.QueryText () or "").strip ()
		if name and name != card["Name"]:
			card["Name"] = name[:32]
			changed = True
	if changed:
		_Publish ()

TimerSet = False

def _StartTimer ():
	global TimerSet
	if not TimerSet:
		TimerSet = True
		GemRB.SetTimer (_Tick, 500, -1)

def _Tick ():
	global Window
	if not Window or Mode == "client":
		return
	try:
		_ReadNames ()
		Refresh ()
	except Exception:
		Window = None

###################################################
# the buttons

def RerollAll ():
	_ReadNames () # a reroll makes new peasants; nothing to keep
	RollAll ()

def Accept ():
	"""the sixteen as they are: SetupGame makes them (the host starts the game)"""
	global Accepted, AcceptedOwners
	if not Cards or Mode == "client":
		return False
	_ReadNames ()
	Accepted = [dict (card) for card in Cards]
	AcceptedOwners = Owners () if Mode == "host" else {}
	_Log ("accepted: %s" % ", ".join ("%s%s" % (card["Name"], " (P%d)" % AcceptedOwners[card["Order"]] if card["Order"] in AcceptedOwners else "") for card in Accepted))
	if Mode == "host":
		lobby = _Lobby ()
		if not lobby or not lobby["CanStart"]:
			Accepted = None
			Shown["Status"] = "Not everyone is ready: back to the lobby, or wait."
			Window.GetControl (0x10000001).SetText (Shown["Status"])
			return False
	CloseOptions ()
	Close ()
	if OnAccept and OnAccept () is False:
		Accepted = None
		_Build ()
		return False
	return True

def Back ():
	global Cards
	CloseOptions ()
	Close ()
	Cards = None
	if OnBack:
		OnBack ()

def _BackToLobby ():
	global Published
	Published = None
	GemRB.NetSetLobbyCards ([])
	import NetLobby
	NetLobby.OpenLobby ()

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except RuntimeError:
			pass
		Window = None

def OpenOptions ():
	global OptionsWindow
	if OptionsWindow:
		CloseOptions ()
		return
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	w, h = 420, 170
	win = GemRB.CreateView (OPTIONS_ID, 255, {'x': (screen[0] - w) // 2, 'y': (screen[1] - h) // 2, 'w': w, 'h': h})
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, 10, 6, w - 20, 24, Font, "House rules (off by default)", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	_Button (win, 1, 10, 40, w - 20, 30, "", lambda: SetOption ("rerollOne", not Options.rerollOne), "A Reroll button on each card")
	_Button (win, 2, 10, 80, w - 20, 30, "", lambda: SetOption ("fourDice", not Options.fourDice), "Each ability rolls 4d6 and drops the lowest die (from the next roll on)")
	close = _Button (win, 3, w // 2 - 60, h - 40, 120, 30, "Close", CloseOptions)
	close.MakeEscape ()
	OptionsWindow = win
	_ShowOptions ()
	win.Focus ()

def _ShowOptions ():
	if not OptionsWindow:
		return
	OptionsWindow.GetControl (1).SetText ("Reroll one card: %s" % ("on" if Options.rerollOne else "off"))
	OptionsWindow.GetControl (2).SetText ("4d6, drop the lowest: %s" % ("on" if Options.fourDice else "off"))

def CloseOptions ():
	global OptionsWindow
	if OptionsWindow:
		try:
			OptionsWindow.Close ()
		except RuntimeError:
			pass
		OptionsWindow = None

###################################################
# co-op clients: the host's cards while they wait in the lobby

def WatchLobby ():
	global Watching
	if not Watching:
		Watching = True
		GemRB.SetTimer (_Watch, 250, -1)

def _Watch ():
	global Window, Mode
	lobby = _Lobby ()
	if not lobby or GemRB.NetGetMode () != "client":
		return
	has = any (slot["Card"] for slot in lobby["Party"])
	try:
		if has and lobby["Phase"] == "lobby":
			if not Window or Mode != "client" or not Window.GetControl (0x10000001):
				_Log ("the host is rolling: showing the cards")
				OpenView ()
			elif len (_ClientCards ()) != _CardCount_Shown ():
				OpenView ()
			else:
				Refresh ()
		elif Window and Mode == "client":
			_Log ("the cards are gone (%s)" % ("the game starts" if lobby["Phase"] != "lobby" else "the host went back"))
			Close ()
	except Exception:
		Window = None

def _CardCount_Shown ():
	return len ([key for key in Shown if isinstance (key, int)])
