# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the reserves (docs/architecture/ui-design.md, "The reserves at an inn or camp"). Decided
# 2026-11-03: the reserves never follow the party; they join only in safe places, so this screen opens
# from an inn's window (the Reserves tab beside Rent room, GUISTORE.py, in a store that offers rooms) or
# from the rest button at a camp (Camp.py, while the global DCC_CAMP is 1), never on its own. A party that
# loses a hero plays short-handed until then.
#   - the party: four rows, one per party member, with Bench (a living hero goes to the reserves and
#     keeps its level, XP, class, gear and records; it earns no XP there) or Bury (the dead leave the
#     party; their graves stay in the graveyard); a free place is a row of its own, whose it is in co-op
#     (the player whose hero left it); a click picks the place the next Join fills;
#   - the reserves as cards in the choose-your-four style: the funnel's survivors (level 0, the class
#     pick pending) and benched heroes; Join takes one into the picked free place: it appears next to the
#     party, a level-0 one then picks its class (ClassPick.py), a benched hero comes back as it left.
# Co-op: a player benches its own heroes and fills its own free places (the host any: it picks for that
# place's player, and the hero is that player's); clients' presses go to the host (GemRB.DCCDismiss and
# DCCRecruit are routed), the new party comes back as a save. The host deals the reserves' player
# buttons as before.

import GemRB
import FunnelChoose
from GUIDefines import *

Font = "NORMAL"
CardFont = "INFOFONT"
WindowColor = {'r': 18, 'g': 16, 'b': 22, 'a': 255}
CardColor = {'r': 46, 'g': 41, 'b': 34, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
PlaceColor = {'r': 30, 'g': 28, 'b': 34, 'a': 255}
PickedColor = {'r': 110, 'g': 95, 'b': 55, 'a': 255}
WINDOW_ID = 82
GAP = 6
TOP = 44
PARTY_H = 34
COLUMNS = 4
PARTY_BASE = 100 # party row i: 100 + 4 i + 0 text, 1 bench/bury, 2 the free place's button
CARD_BASE = 1000 # reserve card i: 1000 + 10 i + 0 backdrop, 1 name, 2 join, 3 player, 5 text
PLACES = {"inn": "the inn", "camp": "the camp"}

Window = None
Mode = None
Where = None # "inn" or "camp"
Back = None # what Close returns to (the inn's window, the camp's)
Place = -1 # the free place the next Join fills (an index into GetDCCFreePlaces), -1 the first one may
Shown = {} # for tests: "Party" [lines], "Places" [lines], "Reserves" [ids], id -> lines, "Status", "Where", "Place"

def _Log (text):
	print ("[Reserves] " + text)

def _Tables ():
	import ChooseFour
	return ChooseFour.Tables ()

def Party ():
	return [FunnelChoose.Survivor (slot, _Tables ()) for slot in range (1, GemRB.GetPartySize () + 1)]

def ReserveCards ():
	return [FunnelChoose.Survivor (ident, _Tables ()) for ident in GemRB.GetDCCReserves ()]

def FreePlaces ():
	return GemRB.GetDCCFreePlaces ()

def Room ():
	"""places free in the party (four after the funnel)"""
	return len (FreePlaces ())

def _Me ():
	import ChooseFour
	return ChooseFour.MySlot ()

def _Host ():
	return Mode != "client"

def FunnelOver ():
	return bool (GemRB.GetGameVar ("DCC_PARTY_SIZE"))

def SafePlace (where):
	"""an inn's window that offers rooms is open, or the party is at a camp (DCC_CAMP)"""
	if not FunnelOver ():
		return False
	if where == "camp":
		import ChooseFour
		return bool (GemRB.GetGameVar (ChooseFour.CAMP_VAR))
	if where == "inn":
		import GUISTORE
		return GUISTORE.IsInn ()
	return False

def Open (where, back = None):
	"""the reserves' screen at an inn or camp (where), None anywhere else"""
	global Window, Mode, Where, Back, Place
	import ChooseFour
	Close ()
	if not SafePlace (where):
		_Log ("not at an inn or camp (%s): the reserves wait there" % where)
		return None
	Mode = ChooseFour.NetMode ()
	Where, Back, Place = where, back, -1
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': 0, 'y': 0, 'w': screen[0], 'h': screen[1]})
	win.AddAlias ("RESERVEWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, GAP, 6, 300, 30, Font, "The reserves at " + PLACES[where], IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, 310, 6, screen[0] - 310 - 150, 30, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	close = _Button (win, 1, screen[0] - GAP - 134, 8, 134, 28, "Back to " + PLACES[where], Leave, "Back to " + PLACES[where], Font)
	close.MakeEscape ()
	# the party: four rows (party members, then the free places)
	for i in range (FunnelChoose.PARTY):
		y = TOP + i * (PARTY_H + 4)
		place = _Button (win, PARTY_BASE + 4 * i + 2, GAP, y, screen[0] - 2 * GAP - 116, PARTY_H, "", lambda i = i: PickRow (i), "Pick this free place: the next to join fills it", CardFont)
		place.SetFlags (IE_GUI_BUTTON_ALIGN_LEFT, OP_OR)
		place.SetBackground (PlaceColor)
		win.CreateLabel (PARTY_BASE + 4 * i, GAP + 6, y, screen[0] - 2 * GAP - 120, PARTY_H, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, PARTY_BASE + 4 * i + 1, screen[0] - GAP - 110, y + 4, 104, PARTY_H - 8, "", lambda i = i: DismissRow (i), None, CardFont)
	reserves = GemRB.GetDCCReserves ()
	top = TOP + FunnelChoose.PARTY * (PARTY_H + 4) + 10
	for i in range (len (reserves)):
		x, y, w, h = _Frame (i, len (reserves), top)
		base = CARD_BASE + 10 * i
		win.CreateLabel (base + 1, x + 6, y + 4, w - 130, 24, Font, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, base + 2, x + w - 120, y + 4, 70, 24, "Join", lambda i = i: JoinCard (i), "Take this one into the picked free place", CardFont)
		_Button (win, base + 3, x + w - 46, y + 4, 40, 24, "", lambda i = i: DealCard (i), "Whose this one is: press for the next player", CardFont)
		win.CreateLabel (base + 5, x + 8, y + 32, w - 14, h - 36, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
		backdrop = _Button (win, base, x, y, w, h, "", None)
		backdrop.SetState (IE_GUI_BUTTON_LOCKED)
		backdrop.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND)
		backdrop.SetBackground (CardColor)
	Window = win
	Refresh ()
	win.Focus ()
	_Log ("opened at %s: %d in the party, %d free places, %d reserves" % (PLACES[where], GemRB.GetPartySize (), Room (), len (reserves)))
	return win

def _Frame (index, count, top):
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	rows = (count + COLUMNS - 1) // COLUMNS
	w = (screen[0] - (COLUMNS + 1) * GAP) // COLUMNS
	h = min (150, (screen[1] - top - (rows + 1) * GAP) // max (rows, 1))
	column, row = index % COLUMNS, index // COLUMNS
	return GAP + column * (w + GAP), top + GAP + row * (h + GAP), w, h

def _Button (win, id, x, y, w, h, text, action, tooltip = None, font = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (font or Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	if action:
		# the engine passes the button to a handler that takes arguments, which would fill a lambda's
		# defaults (lambda i = i: ...): the action is called without any
		button.OnPress (lambda: action ())
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

def PickedPlace ():
	"""the free place a Join fills: the one picked, else the first this player may fill (-1 none)"""
	places = FreePlaces ()
	if 0 <= Place < len (places):
		return Place
	return FunnelChoose.FirstFillable (places, _Me (), _Host ())

def Refresh ():
	if not _Alive ():
		return
	host = _Host ()
	me = _Me ()
	coop = Mode != "single"
	party = Party ()
	places = FreePlaces ()
	picked = PickedPlace ()
	reserves = ReserveCards ()
	Shown.clear ()
	Shown["Where"] = Where
	Shown["Party"] = []
	Shown["Places"] = []
	for i in range (FunnelChoose.PARTY):
		text, button = Window.GetControl (PARTY_BASE + 4 * i), Window.GetControl (PARTY_BASE + 4 * i + 1)
		place = Window.GetControl (PARTY_BASE + 4 * i + 2)
		if i < len (party):
			line = PartyLine (party[i])
			if coop:
				line += "   P%d" % max (0, party[i]["Player"])
			text.SetText (line)
			button.SetText ("Bury" if party[i]["Dead"] else "Bench")
			button.SetTooltip ("Bury: the dead leave the party" if party[i]["Dead"] else "Bench: to the reserves, with everything (no XP while benched)")
			button.SetVisible (FunnelChoose.MayBench (party[i]["Player"], me, host))
			place.SetVisible (False)
			Shown["Party"].append (line)
		elif i - len (party) < len (places):
			index = i - len (party)
			line = FunnelChoose.PlaceLine (places[index], me, coop)
			text.SetText ("")
			button.SetVisible (False)
			place.SetVisible (True)
			place.SetText ("   " + line + ("   (Join fills this one)" if index == picked else ""))
			place.SetBackground (PickedColor if index == picked else PlaceColor)
			place.SetState (IE_GUI_BUTTON_ENABLED if FunnelChoose.MayFill (places[index], me, host) else IE_GUI_BUTTON_DISABLED)
			Shown["Places"].append (line)
		else:
			text.SetText ("")
			button.SetVisible (False)
			place.SetVisible (False)
	Shown["Place"] = picked
	Shown["Reserves"] = [r["Id"] for r in reserves]
	fill = picked >= 0
	for i, reserve in enumerate (reserves):
		base = CARD_BASE + 10 * i
		try:
			name = Window.GetControl (base + 1)
		except Exception:
			name = None
		if not name:
			continue # a reserve that came after the window opened: the next Open shows it
		lines = FunnelChoose.ReserveLines (reserve)
		name.SetText (FunnelChoose.ReserveTitle (reserve))
		Window.GetControl (base + 5).SetText ("\n".join (lines))
		join = Window.GetControl (base + 2)
		join.SetState (IE_GUI_BUTTON_ENABLED if fill else IE_GUI_BUTTON_DISABLED)
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
		Shown[reserve["Id"]] = [FunnelChoose.ReserveTitle (reserve)] + lines
	text = "%d in the party, %d in the reserves." % (len (party), len (reserves))
	if not places:
		text += " Bench or bury one to make room, then pick who joins."
	elif fill:
		text += " Room for %d: pick who joins." % len (places)
	else:
		text += " Room for %d, but not yours to fill." % len (places)
	Shown["Status"] = text
	Window.GetControl (0x10000001).SetText (text)

def PickRow (index):
	"""a click on a free place's row: the next Join fills it"""
	global Place
	places = FreePlaces ()
	place = index - GemRB.GetPartySize ()
	if not 0 <= place < len (places) or not FunnelChoose.MayFill (places[place], _Me (), _Host ()):
		return False
	Place = place
	Refresh ()
	return True

def DismissRow (index):
	if index >= GemRB.GetPartySize ():
		return -1
	return Dismiss (index + 1)

def Dismiss (pc):
	"""a living party member benched (to the reserves, keeping everything), a dead one buried"""
	global Place
	name = GemRB.GetPlayerName (pc, 0)
	if not FunnelChoose.MayBench (GemRB.GetDCCFunnel (pc)["Funnel"]["Player"], _Me (), _Host ()):
		_Log ("%s is another player's" % name)
		return -1
	result = GemRB.DCCDismiss (pc)
	_Log ("%s leaves the party: %d" % (name, result))
	Place = -1
	_Reopen ()
	return result

def JoinCard (index):
	reserves = GemRB.GetDCCReserves ()
	if not 0 <= index < len (reserves):
		return -1
	return Join (reserves[index])

def Join (ident, place = None):
	"""a reserve into a free place (the picked one by default); a level-0 one picks its class next"""
	global Place
	if place is None:
		place = PickedPlace ()
	name = GemRB.GetPlayerName (ident, 0)
	if place < 0:
		_Log ("%s can't join: no free place to fill" % name)
		return -1
	result = GemRB.DCCRecruit (ident, place)
	_Log ("%s joins the party (free place %d): %d" % (name, place, result))
	Place = -1
	_Reopen ()
	return result

def Swap (pc, ident):
	"""a party member (party slot) out, a reserve (global ID) into the place it left"""
	if Dismiss (pc) != 0:
		return -1
	return Join (ident, len (FreePlaces ()) - 1)

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
	"""the reserves changed: the cards again, and the roster's tiles for the new party"""
	import Roster
	Roster.UpdateRoster ()
	if Window:
		Open (Where, Back)

def Leave ():
	"""back to the inn's window or the camp's"""
	back = Back
	Close ()
	if back:
		back ()

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass # gone with the game already (a load, the host's new save in co-op)
		Window = None
