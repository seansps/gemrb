# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.3): the funnel's end screen (docs/architecture/ui-design.md, "Funnel screens"), in the IE
# end-of-chapter manner but functional, in the demo's plain colours: the party that goes on (class,
# level, god, HP, whose), the reserves waiting at camp (level-1 XP, class pick pending) and the
# graveyard (every funnel character who died, from the game's own record, the same for every co-op
# player). ChooseFour.Watch opens it once the class picks are made (DCC_FUNNEL 3); closing it on the
# host marks the funnel done (4). With nobody alive it is the game's end: Close goes back to the start.

import GemRB
import FunnelChoose
from GUIDefines import *

Font = "NORMAL"
TextFont = "INFOFONT"
WindowColor = {'r': 14, 'g': 12, 'b': 18, 'a': 255}
PanelColor = {'r': 40, 'g': 36, 'b': 32, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
TextColor = {'r': 235, 'g': 228, 'b': 210, 'a': 255}
WINDOW_ID = 83

Window = None
GameOver = False
Shown = {} # for tests: "Party", "Reserves", "Graves" (lines), "Title"

def _Log (text):
	print ("[FunnelEnd] " + text)

def _Tables ():
	import ChooseFour
	return ChooseFour.Tables ()

def PartyLines ():
	lines = []
	for slot in range (1, GemRB.GetPartySize () + 1):
		member = FunnelChoose.Survivor (slot, _Tables ())
		text = "%s, %s, %s" % (member["Name"], FunnelChoose.ClassLine (member), member["Occupation"] or "no occupation")
		text += "  HP %d/%d" % (max (0, member["HP"]), member["MaxHP"])
		if GemRB.NetGetMode () and member["Player"] >= 0:
			text += "  (P%d)" % member["Player"]
		if member["Dead"]:
			text += "  (dead)"
		lines.append (text)
	return lines

def ReserveLines ():
	lines = []
	for ident in GemRB.GetDCCReserves ():
		member = FunnelChoose.Survivor (ident, _Tables ())
		stats = GemRB.GetDCCStats (ident)
		text = "%s, %s, %d XP%s" % (member["Name"], member["Occupation"] or "no occupation", stats["XP"] if stats else 0,
			", class pick pending" if member["ClassPending"] else "")
		lines.append (text)
	return lines

def Graves ():
	"""the graveyard's lines: the game's record (the dead in the party and out of it), plus any dead
	party member a save from before M3.3 has only in its own funnel record"""
	import Roster
	return Roster.GraveLinesAll ()

def Open (gameOver = False):
	global Window, GameOver
	Close ()
	GameOver = gameOver
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': 0, 'y': 0, 'w': screen[0], 'h': screen[1]})
	win.AddAlias ("FUNNELENDWIN")
	win.SetBackground (WindowColor)
	title = "The funnel took them all" if gameOver else "The funnel's end"
	win.CreateLabel (0x10000000, 10, 8, screen[0] - 20, 34, Font, title, IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	party, reserves, graves = PartyLines (), ReserveLines (), Graves ()
	w = (screen[0] - 40) // 2
	h = (screen[1] - 140) // 2
	panels = (
		(1, 10, 50, w, h, "The party", party or ["Nobody goes on."]),
		(2, 30 + w, 50, w, h, "The reserves", reserves or ["Nobody waits at camp."]),
		(3, 10, 70 + h, screen[0] - 20, h, "The graveyard", graves or ["No one died. Remarkable."]),
	)
	for id, x, y, pw, ph, heading, lines in panels:
		win.CreateLabel (0x10000010 + id, x, y, pw, 22, Font, heading, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		area = win.CreateTextArea (id, x, y + 24, pw, ph - 24, TextFont)
		area.SetBackground (PanelColor)
		area.SetColor (TextColor, TA_COLOR_NORMAL)
		area.SetText ("\n".join (lines))
	close = _Button (win, 10, screen[0] - 150, screen[1] - 44, 140, 32, "To the start" if gameOver else "Onward", Leave)
	close.MakeEscape ()
	if not gameOver and GemRB.NetGetMode () != "client":
		_Button (win, 11, screen[0] - 300, screen[1] - 44, 140, 32, "Reserves", OpenReserves)
	Window = win
	Shown.clear ()
	Shown.update ({"Title": title, "Party": party, "Reserves": reserves, "Graves": graves})
	_Log ("%s: party %s | reserves %s | graves %s" % (title, party, reserves, graves))
	win.Focus ()
	return win

def _Button (win, id, x, y, w, h, text, action):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	return button

def OpenReserves ():
	import Reserves
	Close ()
	Reserves.Open ()

def Leave ():
	import ChooseFour
	Close ()
	if GameOver:
		_Log ("nobody survived: back to the start")
		GemRB.QuitGame ()
		GemRB.SetNextScript ("Start")
		return
	if GemRB.NetGetMode () != "client" and ChooseFour.State () == 3:
		ChooseFour.SetState (4)

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass # gone with the game already (a load, the host's new save in co-op)
		Window = None
