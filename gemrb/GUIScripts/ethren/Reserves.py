# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.3): the reserves at a camp or tavern (docs/architecture/ui-design.md, "Funnel screens"): the
# funnel's survivors who weren't chosen wait off the map (GemRB.GetDCCReserves; they are in the save) and
# one takes the place of a party member who died or was dismissed. A camp or tavern script opens it by
# setting the global DCC_CAMP to 1 (ChooseFour.Watch); the funnel's end screen has a button for it too.
#   - the party: one row each, with Dismiss (a living one goes to the reserves) or Bury (the dead leave
#     the party; their graves stay in the graveyard);
#   - the reserves as cards, in the choose-your-four style: Join takes one into the party while it has
#     room (a party of four); a level-0 one then picks its class (ClassPick.py). In co-op the host also
#     says whose each one is.
# The host (or single player) swaps; co-op clients see it read-only, the party changes reach them with
# the host's next save.

import GemRB
import FunnelRoll
import FunnelChoose
from GUIDefines import *

Font = "NORMAL"
CardFont = "INFOFONT"
WindowColor = {'r': 18, 'g': 16, 'b': 22, 'a': 255}
CardColor = {'r': 46, 'g': 41, 'b': 34, 'a': 255}
DeadColor = {'r': 34, 'g': 34, 'b': 34, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
WINDOW_ID = 82
GAP = 6
TOP = 44
PARTY_H = 34
COLUMNS = 4
PARTY_BASE = 100 # party row i: 100 + 4 i + 0 text, 1 dismiss
CARD_BASE = 1000 # reserve card i: 1000 + 10 i + 0 backdrop, 1 name, 2 join, 3 player, 5 text

Window = None
Mode = None
Shown = {} # for tests: "Party" [lines], "Reserves" [ids], id -> lines, "Status"

def _Log (text):
	print ("[Reserves] " + text)

def _Tables ():
	import ChooseFour
	return ChooseFour.Tables ()

def Party ():
	return [FunnelChoose.Survivor (slot, _Tables ()) for slot in range (1, GemRB.GetPartySize () + 1)]

def ReserveCards ():
	return [FunnelChoose.Survivor (ident, _Tables ()) for ident in GemRB.GetDCCReserves ()]

def Room ():
	"""places free in the party (four after the funnel)"""
	size = GemRB.GetGameVar ("DCC_PARTY_SIZE") or FunnelChoose.PARTY
	return max (0, size - GemRB.GetPartySize ())

def Open ():
	global Window, Mode
	import ChooseFour
	Close ()
	Mode = ChooseFour.NetMode ()
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': 0, 'y': 0, 'w': screen[0], 'h': screen[1]})
	win.AddAlias ("RESERVEWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, GAP, 6, 300, 30, Font, "The reserves", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, 220, 6, screen[0] - 220 - 150, 30, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	close = _Button (win, 1, screen[0] - GAP - 134, 8, 134, 28, "Close", Leave, "Back to the game", Font)
	close.MakeEscape ()
	# the party: up to four rows
	for i in range (FunnelChoose.PARTY):
		y = TOP + i * (PARTY_H + 4)
		win.CreateLabel (PARTY_BASE + 4 * i, GAP + 6, y, screen[0] - 2 * GAP - 120, PARTY_H, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, PARTY_BASE + 4 * i + 1, screen[0] - GAP - 110, y + 4, 104, PARTY_H - 8, "", lambda i = i: DismissRow (i), None, CardFont)
	reserves = GemRB.GetDCCReserves ()
	top = TOP + FunnelChoose.PARTY * (PARTY_H + 4) + 10
	for i in range (len (reserves)):
		x, y, w, h = _Frame (i, len (reserves), top)
		base = CARD_BASE + 10 * i
		win.CreateLabel (base + 1, x + 6, y + 4, w - 130, 24, Font, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, base + 2, x + w - 120, y + 4, 70, 24, "Join", lambda i = i: JoinCard (i), "Take this one into the party", CardFont)
		_Button (win, base + 3, x + w - 46, y + 4, 40, 24, "", lambda i = i: DealCard (i), "Whose this one is: press for the next player", CardFont)
		win.CreateLabel (base + 5, x + 8, y + 32, w - 14, h - 36, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
		backdrop = _Button (win, base, x, y, w, h, "", None)
		backdrop.SetState (IE_GUI_BUTTON_LOCKED)
		backdrop.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND)
		backdrop.SetBackground (CardColor)
	Window = win
	Refresh ()
	win.Focus ()
	return win

def _Frame (index, count, top):
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	rows = (count + COLUMNS - 1) // COLUMNS
	w = (screen[0] - (COLUMNS + 1) * GAP) // COLUMNS
	h = min (140, (screen[1] - top - (rows + 1) * GAP) // max (rows, 1))
	column, row = index % COLUMNS, index // COLUMNS
	return GAP + column * (w + GAP), top + GAP + row * (h + GAP), w, h

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

def _Alive ():
	try:
		return bool (Window and Window.GetControl (0x10000001))
	except Exception:
		return False

def PartyLine (member):
	who = "%s, %s" % (member["Name"], FunnelChoose.ClassLine (member))
	if member["Dead"]:
		return who + "  (dead)"
	return "%s   HP %d/%d   %s" % (who, max (0, member["HP"]), member["MaxHP"], member["Occupation"])

def Refresh ():
	if not _Alive ():
		return
	host = Mode != "client"
	party = Party ()
	reserves = ReserveCards ()
	Shown.clear ()
	Shown["Party"] = []
	for i in range (FunnelChoose.PARTY):
		text, button = Window.GetControl (PARTY_BASE + 4 * i), Window.GetControl (PARTY_BASE + 4 * i + 1)
		if i < len (party):
			line = PartyLine (party[i])
			text.SetText (line)
			button.SetText ("Bury" if party[i]["Dead"] else "Dismiss")
			button.SetVisible (host)
			Shown["Party"].append (line)
		else:
			text.SetText ("(a free place)")
			button.SetVisible (False)
	coop = Mode != "single"
	Shown["Reserves"] = [r["Id"] for r in reserves]
	room = Room ()
	for i, reserve in enumerate (reserves):
		base = CARD_BASE + 10 * i
		try:
			name = Window.GetControl (base + 1)
		except Exception:
			name = None
		if not name:
			continue # a reserve that came after the window opened: the next Open shows it
		lines = FunnelChoose.CardLines (reserve)
		name.SetText (reserve["Name"])
		Window.GetControl (base + 5).SetText ("\n".join (lines))
		join = Window.GetControl (base + 2)
		join.SetVisible (host)
		join.SetState (IE_GUI_BUTTON_ENABLED if room else IE_GUI_BUTTON_DISABLED)
		player = Window.GetControl (base + 3)
		player.SetVisible (coop)
		if coop:
			import Roster
			owner = reserve["Player"] if reserve["Player"] >= 0 else 0
			colour = Roster.PLAYER_COLORS[owner % len (Roster.PLAYER_COLORS)]
			player.SetText ("P%d" % owner)
			player.SetColor (colour)
			player.SetState (IE_GUI_BUTTON_ENABLED if Mode == "host" else IE_GUI_BUTTON_LOCKED)
			Window.GetControl (base).SetBorder (0, colour, 1, 0)
		Shown[reserve["Id"]] = [reserve["Name"]] + lines
	text = "%d in the party, %d in the reserves." % (len (party), len (reserves))
	if host:
		text += " Dismiss or bury one to make room, then pick who joins." if not room else " Room for %d: pick who joins." % room
	else:
		text += " The host swaps them."
	Shown["Status"] = text
	Window.GetControl (0x10000001).SetText (text)

def DismissRow (index):
	if Mode == "client" or index >= GemRB.GetPartySize ():
		return -1
	return Dismiss (index + 1)

def Dismiss (pc):
	"""a living party member to the reserves, a dead one buried"""
	name = GemRB.GetPlayerName (pc, 0)
	result = GemRB.DCCDismiss (pc)
	_Log ("%s leaves the party: %d" % (name, result))
	_Reopen ()
	return result

def JoinCard (index):
	reserves = GemRB.GetDCCReserves ()
	if Mode == "client" or not 0 <= index < len (reserves):
		return -1
	return Join (reserves[index])

def Join (ident):
	"""a reserve into the party (while there is room); a level-0 one picks its class next"""
	name = GemRB.GetPlayerName (ident, 0)
	result = GemRB.DCCRecruit (ident)
	_Log ("%s joins the party: %d" % (name, result))
	_Reopen ()
	return result

def Swap (pc, ident):
	"""a party member (party slot) out, a reserve (global ID) in"""
	if Dismiss (pc) != 0:
		return -1
	return Join (ident)

def DealCard (index):
	import ChooseFour
	reserves = ReserveCards ()
	players = ChooseFour.Players ()
	if Mode != "host" or not 0 <= index < len (reserves) or not players:
		return False
	current = reserves[index]["Player"] if reserves[index]["Player"] >= 0 else 0
	following = players[(players.index (current) + 1) % len (players)] if current in players else players[0]
	ok = GemRB.DCCSetPlayer (reserves[index]["Id"], following)
	Refresh ()
	return ok

def _Reopen ():
	"""the reserves changed: the cards again"""
	if Window:
		Open ()

def Leave ():
	import ChooseFour
	Close ()
	if Mode != "client" and GemRB.GetGameVar (ChooseFour.CAMP_VAR):
		GemRB.SetGlobal (ChooseFour.CAMP_VAR, "GLOBAL", 0)
	ChooseFour.CampShown = False

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except RuntimeError:
			pass
		Window = None
