# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the bottom bar (docs/architecture/ui-design.md, "Proposed: the party column and the slim bar"): the
# message log and two rows of small buttons, left of the party column (Roster.py) and as slim as the log allows.
# It is the demo's message window (GUIW window 0: its text area and that one's scroll bar), resized and with its
# buttons replaced. The buttons: Map (M), Inventory (I), Record (C), Spells (W), Journal (J, not built yet),
# Rest, Pause (space), Centre; All, Mine (co-op), Block, Column, the thief's T, S and P (DCCThief.py), the party
# settings. A dialog makes the log taller while it lasts.
# The window ignores events itself (its buttons still take clicks), as the stock games' side windows do, so it
# never takes the keyboard from the game view: the keymap's keys (keymap.ini: M, I, C, W, R, space...) only reach
# the game view while it has the focus.
# Also the autotest hooks.

import GemRB
import Tests
import GUICommon
import CommonWindow
import GUIClasses
import GUIINV
import GUIMA
import GUIREC
import DCCThief
import DCCAutotest
import FunnelAutotest
import GameAutotest
import Roster
import NetChat
from GUIDefines import *

BarColor = {'r': 24, 'g': 22, 'b': 30, 'a': 235}
LogColor = {'r': 14, 'g': 13, 'b': 18, 'a': 255}
BAR_H = 72 # four lines of the log; two rows of buttons
DIALOG_H = 220 # the log while a dialog runs
BUTTON_W = 46
BUTTON_H = 30
GAP = 4
Window = None
Buttons = {} # label -> button (for tests)

def _Spec ():
	"""the bar's buttons, two rows: (label, tooltip, action); None leaves the place empty"""
	thief = dict ((label, (tip, action)) for label, tip, action in DCCThief.Specs ())
	return (
		(("Map", "Map (M)", GUIMA.OpenMapWindow),
		("Inv", "Inventory (I)", GUIINV.OpenInventoryWindow),
		("Rec", "Character record (C)", lambda: GUIREC.ToggleRecordsWindow ()),
		("Spells", "Spells (W)", lambda: _Spells ()),
		("Jrnl", "Journal (J): not built yet", None),
		("Rest", "Rest the night; at a camp: the camp, where the reserves wait", lambda: Roster.RestPress ()),
		("Pause", "Pause (space)", lambda: GemRB.GamePause (2, 0)),
		("Ctr", "Centre the view on the selected", lambda: GemRB.GameControlSetScreenFlags (SF_CENTERONACTOR, OP_OR))),
		(("All", "Select everyone", lambda: Roster.SelectAll ()),
		("Mine", "Select the characters you control", lambda: Roster.SelectMine ()),
		("Block", "Formation: block (four wide). Lines the selection up where its first one stands", lambda: Roster.Formation (0)),
		("Col", "Formation: column of twos. Lines the selection up where its first one stands", lambda: Roster.Formation (1)),
		("T",) + thief["T"], ("S",) + thief["S"], ("P",) + thief["P"],
		("Set", "Party settings: Luck, spellburn, prompts, the party AI", lambda: Roster.OpenSettings ())),
	)

def _Spells ():
	import DCCMagic
	DCCMagic.ToggleSpellsWindow ()

def Width ():
	return GemRB.GetSystemVariable (SV_WIDTH) - Roster.ColumnWidth ()

def OnLoad():
	global Window
	MessageWindow = Window = GemRB.LoadWindow(0, GUICommon.GetWindowPack(), WINDOW_BOTTOM|WINDOW_HCENTER)
	MessageWindow.SetFlags (WF_BORDERLESS|IE_GUI_VIEW_IGNORE_EVENTS, OP_OR)
	MessageWindow.AddAlias("MSGWIN")
	MessageWindow.AddAlias("HIDE_CUT", 0)
	MessageWindow.SetBackground (BarColor)

	# the demo's buttons go: the bar makes its own
	for cid in (2, 3, 4, 5):
		control = MessageWindow.GetControl (cid)
		if control:
			MessageWindow.RemoveSubview (control, True)

	# set up some *initial* text (UpdateControlStatus will get called several times)
	TMessageTA = MessageWindow.GetControl(0)
	TMessageTA.SetFlags (IE_GUI_TEXTAREA_AUTOSCROLL|IE_GUI_TEXTAREA_HISTORY)
	TMessageTA.AddAlias("MsgSys", 0)
	TMessageTA.AddAlias("MTA", 0)
	TMessageTA.SetColor(ColorRed, TA_COLOR_OPTIONS)
	TMessageTA.SetColor(ColorWhite, TA_COLOR_HOVER)
	TMessageTA.SetBackground (LogColor)

	# the demo's text tests still run, into the log file rather than the player's log
	print ("[MessageWindow] " + Tests.RunTests ().replace ("\n", " "))
	TMessageTA.SetText ("[color=d8c890]Dungeons of Ethren[/color]\n")

	Buttons.clear ()
	for row, specs in enumerate (_Spec ()):
		for col, (label, tip, action) in enumerate (specs):
			button = MessageWindow.CreateButton (10 + row * 10 + col, 0, 0, BUTTON_W, BUTTON_H)
			button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
			button.SetFont ("NORMAL")
			button.SetBackground (Roster.ButtonColor)
			button.SetText (label)
			button.SetTooltip (tip)
			if action:
				_OnPress (button, action)
			else:
				button.SetState (IE_GUI_BUTTON_DISABLED)
			Buttons[label] = button
	Roster.Backdrop (MessageWindow, Width (), BAR_H)
	# the thief's keys (DCCThief: global hot keys, as before)
	DCCThief.SetHotKeys (Buttons["T"], Buttons["S"], Buttons["P"])
	GUIREC.OnGameStart ()
	Layout (False)
	_WrapDialog ()

	# co-op: the recent chat (the lobby's too) goes back into the new message window
	NetChat.OnMessageWindow ()

	# the party column (Phase 3, reworked): a tile per party member, scrolling when they don't fit
	Roster.RosterWindow = None
	Roster.OpenRoster ()
	UpdateBar ()

	# dcctest-auto: checks the DCC numbers once the area script asks for it
	DCCAutotest.OnGameStart ()
	# funneltest-auto: the funnel readiness checks (M2.9), likewise
	FunnelAutotest.OnGameStart ()
	# ethren-auto: the real game's smoke test (game/), when its autotest table is there
	GameAutotest.OnGameStart ()

def _OnPress (button, action):
	# the engine passes the button to a handler that takes arguments (a lambda's defaults too): the action is
	# called without any
	button.OnPress (lambda: action ())

def Layout (dialog):
	"""the bar along the bottom, left of the party column: the log, its scroll bar, the buttons on the right"""
	win = Window
	if not win:
		return
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	width = Width ()
	height = DIALOG_H if dialog else BAR_H
	cols = max (len (row) for row in _Spec ())
	block = cols * BUTTON_W + (cols - 1) * GAP
	bx = width - block - GAP
	for row in range (2):
		for col in range (cols):
			button = win.GetControl (10 + row * 10 + col)
			if button:
				button.SetFrame ({'x': bx + col * (BUTTON_W + GAP), 'y': GAP + row * (BUTTON_H + GAP), 'w': BUTTON_W, 'h': BUTTON_H})
	scroll = win.GetControl (1)
	sw = scroll.GetFrame ()['w'] if scroll else 0
	log = {'x': GAP, 'y': GAP, 'w': bx - 3 * GAP - sw, 'h': height - 2 * GAP}
	win.GetControl (0).SetFrame (log)
	if scroll:
		scroll.SetFrame ({'x': log['x'] + log['w'] + GAP, 'y': GAP, 'w': sw, 'h': log['h']})
	# the backdrop as big as the window (a control beyond it would take clicks outside it)
	win.GetControl (Roster.BACKDROP_ID).SetFrame ({'x': 0, 'y': 0, 'w': width, 'h': height})
	# the window last, its controls inside it already: a window is a scroll view, and one shrunk around
	# controls that stick out scrolls its content away (the CHU's are laid out for 800 by 240)
	win.SetFrame ({'x': 0, 'y': screen[1] - height, 'w': width, 'h': height})
	win.ScrollTo (0, 0)

def _WrapDialog ():
	"""GUIWORLD's dialog hooks, with the log made taller first and slim again after"""
	import GUIWORLD
	if getattr (GUIWORLD, "EthrenBar", False):
		return
	GUIWORLD.EthrenBar = True
	started, ended = GUIWORLD.DialogStarted, GUIWORLD.DialogEnded
	def DialogStarted ():
		Layout (True)
		started ()
	def DialogEnded ():
		ended ()
		Layout (False)
	GUIWORLD.DialogStarted, GUIWORLD.DialogEnded = DialogStarted, DialogEnded

def UpdateBar ():
	"""what the buttons show now (the roster's timer calls it): pause, the formation, Mine in co-op only"""
	if not Window or not Buttons:
		return
	try:
		paused = GemRB.GamePause (3, 1)
		Buttons["Pause"].SetBackground (Roster.SelectedColor if paused else Roster.ButtonColor)
		formation = Roster.CurrentFormation ()
		Buttons["Block"].SetBackground (Roster.SelectedColor if formation == 0 else Roster.ButtonColor)
		Buttons["Col"].SetBackground (Roster.SelectedColor if formation == 1 else Roster.ButtonColor)
		Buttons["Mine"].SetState (IE_GUI_BUTTON_ENABLED if GemRB.NetGetMode () else IE_GUI_BUTTON_DISABLED)
	except RuntimeError:
		pass # the window went with the game

def ToggleAlwaysRun():
	# the keymap's Toggle_Always_Run (unhardcoded/ethren/keymap.2da): every move is a run while it is on
	# (combat.md "Movement"), so say which it is now
	GemRB.GameControlToggleAlwaysRun ()
	state = "on" if GemRB.GetVar ("Always Run") else "off"
	GemRB.GetView ("MsgSys", 0).Append ("[color=d8c890]Always run: " + state + "[/color]\n")

def UpdateControlStatus():
	pass

def ToggleMenuWindow():
	pass
