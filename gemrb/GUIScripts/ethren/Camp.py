# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the rest button (the roster's Rest) and the camp (docs/architecture/ui-design.md, "The reserves at
# an inn or camp"). A camp is a safe place: its area script sets the global DCC_CAMP to 1 while the party is
# there (and back to 0 when it leaves). Then, once the funnel is over, Rest opens the camp's window: Rest for
# the night (camping: the area's rest checks, +1 HP), the reserves (Reserves.py: bench a hero, bury the dead,
# take a reserve into a free place) or Leave. Anywhere else Rest just rests, as the stock rest button does.
# Built from code in the demo's plain colours; Phase 5 skins it.

import GemRB
from GUIDefines import *

Font = "NORMAL"
TextFont = "INFOFONT"
WindowColor = {'r': 22, 'g': 19, 'b': 26, 'a': 250}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
WINDOW_ID = 85
W, H = 420, 150
CAMP_HP = 1 # a night's rest while adventuring (combat.md, "Healing")
REST_CHECKS = 1 | 2 | 4 | 8 # the area, the party together, in control, no enemies (the stock rest button's)

Window = None
Shown = {} # for tests: "Text", "Rested" (the last rest's outcome)

def _Log (text):
	print ("[Camp] " + text)

def AtCamp ():
	import ChooseFour
	return bool (GemRB.GetGameVar (ChooseFour.CAMP_VAR)) and bool (GemRB.GetGameVar ("DCC_PARTY_SIZE"))

def RestPress ():
	"""the rest button: the camp's window at a camp, else a rest"""
	if AtCamp ():
		return Open ()
	return Rest ()

def Open ():
	global Window
	Close ()
	if not AtCamp ():
		return None
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': (screen[0] - W) // 2, 'y': (screen[1] - H) // 2 - 60, 'w': W, 'h': H})
	win.AddAlias ("CAMPWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, 10, 8, W - 20, 28, Font, "The camp", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	text = "Rest here for the night, or see who waits at camp."
	win.CreateLabel (0x10000001, 10, 40, W - 20, 48, TextFont, text, IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_TOP)
	w = (W - 40) // 3
	_Button (win, 1, 10, H - 46, w, 32, "Rest", RestHere, "Sleep the night here (camping)")
	_Button (win, 2, 20 + w, H - 46, w, 32, "Reserves", OpenReserves, "Bench a hero, bury the dead, take a reserve into a free place")
	leave = _Button (win, 3, 30 + 2 * w, H - 46, w, 32, "Leave", Close, "Back to the game")
	leave.MakeEscape ()
	Window = win
	Shown["Text"] = text
	win.Focus ()
	_Log ("the camp's window")
	return win

def _Button (win, id, x, y, w, h, text, action, tooltip = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (lambda: action ())
	if tooltip:
		button.SetTooltip (tooltip)
	return button

def RestHere ():
	Close ()
	return Rest ()

def OpenReserves ():
	import Reserves
	Close ()
	return Reserves.Open ("camp", Open)

def Rest ():
	"""a night's rest with the stock checks (the area may interrupt it); the dream scripts first"""
	# the dream scripts first (bg2's pdialog.2da; Ethren has none yet: the curse's dreams will need a hook)
	from ie_restype import RES_2DA
	if GemRB.HasResource ("pdialog", RES_2DA) and GemRB.RunRestScripts ():
		Shown["Rested"] = "dream scripts"
		return True
	info = GemRB.RestParty (REST_CHECKS, -1, CAMP_HP)
	if info and info["Error"]:
		why = GemRB.GetString (info["ErrorMsg"]) if info["ErrorMsg"] >= 0 else "not now"
		GemRB.DisplayString (why, ColorRed)
		Shown["Rested"] = "no: " + why
		_Log ("no rest: %s" % why)
		return False
	Shown["Rested"] = "rested"
	_Log ("the party rested")
	return True

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass
		Window = None
