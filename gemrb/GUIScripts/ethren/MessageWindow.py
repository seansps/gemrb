# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the bottom bar (docs/architecture/ui-design.md, "The party column and the slim bar"): the
# message log and two rows of small buttons, left of the party column (Roster.py) and as slim as the log allows.
# It is the demo's message window (GUIW window 0: its text area and that one's scroll bar), resized and with its
# buttons replaced. The buttons, two rows with no gaps: Map (M), Inventory (I), Record (C), Spells (W), Rest,
# Pause (space), the party settings; All, Mine (co-op only), Block, Column, the thief's T, S and P (DCCThief.py).
# No Journal until it exists, no Centre (a double click on a tile centres). A dialog makes the log taller.
# The window ignores events itself (its buttons still take clicks), as the stock games' side windows do, so it
# never takes the keyboard from the game view: the keymap's keys (keymap.ini: M, I, C, W, R, space...) only reach
# the game view while it has the focus.
# The skin (Skin.py): the bar's backgrounds (ETB<w>, ETD<w> in a dialog), its buttons' BAMs, the log on the felt well
# in the skin's ink; in a dialog the Continue/End dialog button (GUIWORLD's ContinueWindow) moves into the button
# block, under the two rows. A game without the skin (dcctest) keeps the plain colours.
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
import Skin
from GUIDefines import *

BarColor = {'r': 24, 'g': 22, 'b': 30, 'a': 235}
LogColor = {'r': 14, 'g': 13, 'b': 18, 'a': 255}
BAR_H = 72 # four lines of the log; two rows of buttons
DIALOG_H = 220 # the log while a dialog runs
DIALOG_BUTTON = (346, 40) # the Continue/End dialog button (skinned): as wide as the button block, under its rows
BUTTON_W = 46
BUTTON_H = 30
GAP = 4
Window = None
Buttons = {} # label -> button (for tests)
Order = [] # the buttons' labels in order (first row, then second)

def _Spec ():
	"""the bar's buttons, in order: (label, tooltip, action); the two rows share them out (_Rows). Mine only in
	co-op; no Journal until the journal exists; no Centre (a double click on a tile centres the view)"""
	thief = dict ((label, (tip, action)) for label, tip, action in DCCThief.Specs ())
	specs = [("Map", "Map (M)", GUIMA.OpenMapWindow),
		("Inv", "Inventory (I)", GUIINV.OpenInventoryWindow),
		("Rec", "Character record (C)", lambda: GUIREC.ToggleRecordsWindow ()),
		("Spells", "Spells (W)", lambda: _Spells ()),
		("Rest", "Rest the night; at a camp: the camp, where the reserves wait", lambda: Roster.RestPress ()),
		("Pause", "Pause (space)", lambda: GemRB.GamePause (2, 0)),
		("Set", "Party settings: Luck, spellburn, prompts, the party AI", lambda: Roster.OpenSettings ()),
		("All", "Select everyone", lambda: Roster.SelectAll ())]
	if GemRB.NetGetMode ():
		specs.append (("Mine", "Select the characters you control", lambda: Roster.SelectMine ()))
	specs += [("Block", "Formation: block (four wide). Lines the selection up where its first one stands", lambda: Roster.Formation (0)),
		("Col", "Formation: column of twos. Lines the selection up where its first one stands", lambda: Roster.Formation (1)),
		("T",) + thief["T"], ("S",) + thief["S"], ("P",) + thief["P"]]
	return specs

def _Rows (count):
	"""the buttons' two rows: how many on the first, how many on the second (the second as wide as the first, its
	buttons a little wider when it has one fewer, so neither row leaves a gap)"""
	first = (count + 1) // 2
	return first, count - first

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
	if Skin.On ():
		# on the bar's felt well, in the skin's ink
		TMessageTA.SetColor (Skin.INK, TA_COLOR_NORMAL)
		TMessageTA.SetColor (Skin.INK_OPTION, TA_COLOR_OPTIONS)
		TMessageTA.SetColor (Skin.INK_HOVER, TA_COLOR_HOVER)
	else:
		TMessageTA.SetBackground (LogColor)

	# the demo's text tests still run, into the log file rather than the player's log
	print ("[MessageWindow] " + Tests.RunTests ().replace ("\n", " "))
	TMessageTA.SetText ("[color=d8c890]Dungeons of Ethren[/color]\n")

	Buttons.clear ()
	Order[:] = []
	for i, (label, tip, action) in enumerate (_Spec ()):
		button = MessageWindow.CreateButton (10 + i, 0, 0, BUTTON_W, BUTTON_H)
		button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
		button.SetFont (Skin.Font (Skin.BUTTON_FONT))
		button.SetBackground (Roster.ButtonColor)
		button.SetText (label)
		button.SetTooltip (tip)
		_OnPress (button, action)
		Buttons[label] = button
		Order.append (label)
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
	first, second = _Rows (len (Order))
	block = first * BUTTON_W + (first - 1) * GAP
	bx = width - block - GAP
	for i, label in enumerate (Order):
		row, col = (0, i) if i < first else (1, i - first)
		count = first if row == 0 else second
		w = (block - (count - 1) * GAP) // count
		x = bx + col * (w + GAP)
		if col == count - 1:
			w = bx + block - x # the last one ends where the other row does
		Buttons[label].SetFrame ({'x': x, 'y': GAP + row * (BUTTON_H + GAP), 'w': w, 'h': BUTTON_H})
	scroll = win.GetControl (1)
	sw = scroll.GetFrame ()['w'] if scroll else 0
	log = {'x': GAP, 'y': GAP, 'w': bx - 3 * GAP - sw, 'h': height - 2 * GAP}
	win.GetControl (0).SetFrame (log)
	if scroll:
		scroll.SetFrame ({'x': log['x'] + log['w'] + GAP, 'y': GAP, 'w': sw, 'h': log['h']})
	# the backdrop as big as the window (a control beyond it would take clicks outside it)
	win.GetControl (Roster.BACKDROP_ID).SetFrame ({'x': 0, 'y': 0, 'w': width, 'h': height})
	for label in Order:
		Skin.Button (Buttons[label], label)
	if not Skin.Background (win, Skin.BarBackground (width, dialog)):
		win.SetBackground (BarColor)
	# the window last, its controls inside it already: a window is a scroll view, and one shrunk around
	# controls that stick out scrolls its content away (the CHU's are laid out for 800 by 240)
	win.SetFrame ({'x': 0, 'y': screen[1] - height, 'w': width, 'h': height})
	win.ScrollTo (0, 0)

def DialogButtonFrame ():
	"""where the skin puts the Continue/End dialog button: the button block's width, at the bottom of the dialog's bar
	(under the two rows of buttons), in screen coordinates"""
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	first = _Rows (len (Order))[0]
	block = first * BUTTON_W + (first - 1) * GAP
	w, h = DIALOG_BUTTON
	return {'x': Width () - block - GAP, 'y': screen[1] - GAP - h, 'w': block, 'h': h}

def _PlaceDialogButton ():
	"""the skin's Continue/End dialog button: GUIWORLD's ContinueWindow (the demo's 800 px strip, which covered the log's
	last lines) shrunk to the button, in the bar's button block; the window stays GUIWORLD's, so its focus and its
	clicks work as before"""
	import GUIWORLD
	win = GUIWORLD.ContinueWindow
	if not win or not Skin.On ():
		return
	frame = DialogButtonFrame ()
	button = win.GetControl (0)
	if not button or not Skin.ButtonBam (frame['w'], frame['h']):
		return
	win.SetBackground (None)
	win.SetBackground (Skin.CLEAR)
	# the button first, then the window around it (a window shrunk round a control that sticks out scrolls it away)
	button.SetFrame ({'x': 0, 'y': 0, 'w': frame['w'], 'h': frame['h']})
	button.SetFont (Skin.TITLE_FONT)
	Skin.Button (button, "dialog", False)
	win.SetFrame (frame)
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
		_PlaceDialogButton ()
	def DialogEnded ():
		ended ()
		Layout (False)
	GUIWORLD.DialogStarted, GUIWORLD.DialogEnded = DialogStarted, DialogEnded
	# the End dialog button (unlike Continue) never brought its window to the front: NextDialogState focuses the
	# bar, whose backdrop then took the click, so a conversation that ends without a reply couldn't be left
	opened = GUIWORLD.OpenEndMessageWindow
	def OpenEndMessageWindow ():
		opened ()
		if GUIWORLD.ContinueWindow:
			GUIWORLD.ContinueWindow.Focus ()
	GUIWORLD.OpenEndMessageWindow = OpenEndMessageWindow

def UpdateBar ():
	"""what the buttons show now (the roster's timer calls it): pause, the formation"""
	if not Window or not Buttons:
		return
	try:
		paused = GemRB.GamePause (3, 1)
		formation = Roster.CurrentFormation ()
		for label, lit in (("Pause", paused), ("Block", formation == 0), ("Col", formation == 1)):
			if not Skin.SetLit (label, lit):
				Buttons[label].SetBackground (Roster.SelectedColor if lit else Roster.ButtonColor)
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
