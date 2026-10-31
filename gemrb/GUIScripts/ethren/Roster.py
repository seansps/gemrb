# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the party roster, a compact stand-in for the portrait bar that fits the level-0
# funnel's 16 characters (MaxPartySize). Two columns of eight name buttons on the right edge,
# each with its hit points; a click selects that character alone, a right click adds it to
# or takes it from the selection. Above them: select everyone, the formation (cycles through
# formatio.2da's rows) and the party settings (DCCParty.py). Built from code, no CHU; it
# refreshes itself on a timer (the demo GUI has no portrait window for the engine to update).

import GemRB
from GUIDefines import *
from ie_stats import *

Font = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 230}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
SelectedColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}
DeadColor = {'r': 40, 'g': 40, 'b': 40, 'a': 255}
HurtColor = {'r': 110, 'g': 40, 'b': 35, 'a': 255}

MAX_SLOTS = 16
COLUMNS = 2
ROWS = 8
BUTTON_W = 104
BUTTON_H = 26
GAP = 4
HEADER = 30
# formatio.2da's rows (ethren): the names the formation button shows
FORMATIONS = ["Block", "Column", "Line", "Threes", "Wedge"]

RosterWindow = None
TimerSet = False
Shown = {} # what each slot shows, for tests

def OpenRoster ():
	global RosterWindow
	if RosterWindow:
		return RosterWindow
	width = COLUMNS * BUTTON_W + (COLUMNS + 1) * GAP
	height = HEADER + ROWS * (BUTTON_H + GAP) + GAP
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': screen[0] - width, 'y': 4, 'w': width, 'h': height}
	win = GemRB.CreateView (95, 255, frame)
	win.AddAlias ("ROSTWIN")
	win.SetBackground (WindowColor)
	third = (width - 4 * GAP) // 3
	_Button (win, 100, GAP, GAP, third, HEADER - 2 * GAP, "All", SelectAll, "Select everyone")
	_Button (win, 101, 2 * GAP + third, GAP, third, HEADER - 2 * GAP, "", NextFormation, "Formation (click to change)")
	_Button (win, 102, 3 * GAP + 2 * third, GAP, third, HEADER - 2 * GAP, "Set", OpenSettings, "Party settings: Luck, spellburn, prompts")
	for i in range (MAX_SLOTS):
		column, row = divmod (i, ROWS)
		x = GAP + column * (BUTTON_W + GAP)
		y = HEADER + row * (BUTTON_H + GAP)
		button = _Button (win, i, x, y, BUTTON_W, BUTTON_H, "", lambda slot = i + 1: SelectOnly (slot))
		button.OnRightPress (lambda slot = i + 1: ToggleSelect (slot))
	RosterWindow = win
	_StartTimer ()
	UpdateRoster ()
	return win

def _Button (win, id, x, y, w, h, text, action, tooltip = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	if tooltip:
		button.SetTooltip (tooltip)
	return button

def _StartTimer ():
	global TimerSet
	if not TimerSet:
		TimerSet = True
		GemRB.SetTimer (_Tick, 500, -1)

def _Tick ():
	global RosterWindow
	try:
		UpdateRoster ()
	except Exception:
		# the window went with the game (back to the menu, a load)
		RosterWindow = None

def SelectOnly (slot):
	if slot <= GemRB.GetPartySize ():
		GemRB.GameSelectPC (slot, 1, SELECT_REPLACE)
		UpdateRoster ()

def ToggleSelect (slot):
	if slot <= GemRB.GetPartySize ():
		GemRB.GameSelectPC (slot, 0 if GemRB.GameIsPCSelected (slot) else 1)
		UpdateRoster ()

def SelectAll ():
	GemRB.GameSelectPC (0, 1)
	UpdateRoster ()

def CurrentFormation ():
	"""formatio.2da's row the party walks in (the preset the game has picked)"""
	return GemRB.GameGetFormation (GemRB.GameGetFormation ())

def NextFormation ():
	which = GemRB.GameGetFormation ()
	GemRB.GameSetFormation ((CurrentFormation () + 1) % len (FORMATIONS), which)
	UpdateRoster ()

def OpenSettings ():
	import DCCParty
	DCCParty.OpenPartySettings ()

def UpdateRoster ():
	global RosterWindow
	win = RosterWindow
	if not win:
		return
	if not win.GetControl (100):
		RosterWindow = None
		return
	size = GemRB.GetPartySize ()
	formation = CurrentFormation ()
	win.GetControl (101).SetText (FORMATIONS[formation] if 0 <= formation < len (FORMATIONS) else "F%d" % formation)
	for i in range (MAX_SLOTS):
		button = win.GetControl (i)
		slot = i + 1
		if slot > size:
			button.SetVisible (False)
			Shown.pop (slot, None)
			continue
		button.SetVisible (True)
		name = GemRB.GetPlayerName (slot, 0)
		hp = GemRB.GetPlayerStat (slot, IE_HITPOINTS)
		hpMax = GemRB.GetPlayerStat (slot, IE_MAXHITPOINTS)
		state = GemRB.GetPlayerStat (slot, IE_STATE_ID)
		dead = (state & STATE_DEAD) or hp < 1
		selected = GemRB.GameIsPCSelected (slot)
		text = "%s %s" % (name[:8], "dead" if dead else "%d/%d" % (hp, hpMax))
		button.SetText (text)
		button.SetTooltip ("%d: %s, %d/%d hp%s" % (slot, name, hp, hpMax, " (selected)" if selected else ""))
		if dead:
			button.SetBackground (DeadColor)
		elif selected:
			button.SetBackground (SelectedColor)
		elif hpMax > 0 and hp * 2 < hpMax:
			button.SetBackground (HurtColor)
		else:
			button.SetBackground (ButtonColor)
		Shown[slot] = text
