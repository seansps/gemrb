# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The ethren record screen: the DCC character sheet of the selected party member
# as plain text (DCCSheet.Lines). Built from code like Prompt.py, so the demo's GUI
# needs no CHU for it. Opened with the "R" button of the message window.
# "Luck" and "Deeds" open the M2.4 settings panels (DCCLuck), "Combat" the M2.3b one
# (charge, grapple, subdual blows, battle rage), and "Roll body over" lets a living
# party member roll a fallen one's body over (GemRB.DCCRecoverBody). The art and the
# level-up button come later (M2.8).
import GemRB
import DCCLuck
import DCCSheet
from GUIDefines import *

Font = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 240}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
Width = 460
Height = 560
Shown = 1 # party slot on show
Window = None
Text = None
DeedsButton = None
BodyButton = None

def _Button (win, id, x, y, w, h, text, action, hotkey = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	if hotkey:
		button.SetHotKey (hotkey)
	return button

def OpenRecordsWindow (pc = None):
	"""opens on pc, the selected party member by default; a second press closes it"""
	global Shown, Window, Text, DeedsButton, BodyButton
	if Window:
		CloseRecordsWindow ()
		return

	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - Height) // 4), 'w': Width, 'h': Height}
	win = Window = GemRB.CreateView (96, 255, frame)
	win.AddAlias ("WIN_REC")
	win.SetBackground (WindowColor)
	Text = win.CreateTextArea (1, 12, 10, Width - 24, Height - 90, Font)
	_Button (win, 2, 12, Height - 38, 50, 28, "<", lambda: _Step (-1))
	_Button (win, 3, 68, Height - 38, 50, 28, ">", lambda: _Step (1))
	# M2.4: the Luck burn policy, and a warrior's or dwarf's Mighty Deeds
	_Button (win, 5, 130, Height - 38, 90, 28, "Luck", lambda: DCCLuck.OpenLuckWindow (Shown))
	DeedsButton = _Button (win, 6, 226, Height - 38, 90, 28, "Deeds", lambda: DCCLuck.OpenDeedsWindow (Shown))
	# M2.3b: the combat options, and rolling a fallen character's body over
	_Button (win, 7, 130, Height - 72, 90, 28, "Combat", lambda: DCCLuck.OpenCombatWindow (Shown))
	BodyButton = _Button (win, 8, 226, Height - 72, 190, 28, "Roll body over", lambda: RecoverBody (Shown))
	close = _Button (win, 4, Width - 112, Height - 38, 100, 28, "Close", CloseRecordsWindow)
	close.MakeEscape ()
	win.Focus ()
	UpdateRecordsWindow ()

def CloseRecordsWindow ():
	global Window, Text, DeedsButton, BodyButton
	DCCLuck.Close ()
	if Window:
		Window.Close ()
	Window = Text = DeedsButton = BodyButton = None

def OnGameStart ():
	"""a new or loaded game closed every window, ours included"""
	global Window, Text, DeedsButton, BodyButton
	Window = Text = DeedsButton = BodyButton = None
	DCCLuck.OnGameStart ()

def BodyHelper (dead):
	"""the party member who rolls the body over: the selected one if it can, else the first who can"""
	party = range (1, GemRB.GetPartySize () + 1)
	selected = GemRB.GameGetSelectedPCSingle ()
	def able (pc):
		if pc == dead or not pc:
			return False
		stats = GemRB.GetDCCStats (pc)
		return stats and stats["HP"] > 0 and not (stats["States"] & 23) and not stats["Subdued"] # held or dying: 1|2|4|16
	if able (selected):
		return selected
	for pc in party:
		if able (pc):
			return pc
	return 0

def RecoverBody (dead):
	"""a living party member rolls the fallen one's body over (party slots: co-op sends it to the host)"""
	helper = BodyHelper (dead)
	if not helper:
		return -1
	result = GemRB.DCCRecoverBody (dead, helper)
	UpdateRecordsWindow ()
	return result

def _Step (step):
	global Shown
	count = GemRB.GetPartySize ()
	if count:
		Shown = (Shown - 1 + step) % count + 1
	DCCLuck.Close ()
	UpdateRecordsWindow ()

def UpdateRecordsWindow ():
	if not Text:
		return
	if Shown > GemRB.GetPartySize ():
		Text.SetText ("")
		return
	Text.SetText ("\n".join (DCCSheet.Lines (Shown)))
	# only warriors and dwarves have deeds
	stats = GemRB.GetDCCStats (Shown)
	if DeedsButton:
		DeedsButton.SetState (IE_GUI_BUTTON_ENABLED if stats and stats["DeedDie"] else IE_GUI_BUTTON_DISABLED)
	# a level 1+ character who died within the hour, not rolled over yet, and someone to do it
	if BodyButton:
		can = stats and stats["BodyRecoverable"] and BodyHelper (Shown)
		BodyButton.SetState (IE_GUI_BUTTON_ENABLED if can else IE_GUI_BUTTON_DISABLED)
