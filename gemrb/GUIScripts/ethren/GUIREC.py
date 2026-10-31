# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The ethren record screen: the DCC character sheet of the selected party member
# as plain text (DCCSheet.Lines). Built from code like Prompt.py, so the demo's GUI
# needs no CHU for it. Opened with the "R" button of the message window.
# "Luck" and "Deeds" open the M2.4 settings panels (DCCLuck). The art and the
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
	global Shown, Window, Text, DeedsButton
	if Window:
		CloseRecordsWindow ()
		return

	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - Height) // 4), 'w': Width, 'h': Height}
	win = Window = GemRB.CreateView (96, 255, frame)
	win.AddAlias ("WIN_REC")
	win.SetBackground (WindowColor)
	Text = win.CreateTextArea (1, 12, 10, Width - 24, Height - 56, Font)
	_Button (win, 2, 12, Height - 38, 50, 28, "<", lambda: _Step (-1))
	_Button (win, 3, 68, Height - 38, 50, 28, ">", lambda: _Step (1))
	# M2.4: the Luck burn policy, and a warrior's or dwarf's Mighty Deeds
	_Button (win, 5, 130, Height - 38, 90, 28, "Luck", lambda: DCCLuck.OpenLuckWindow (Shown))
	DeedsButton = _Button (win, 6, 226, Height - 38, 90, 28, "Deeds", lambda: DCCLuck.OpenDeedsWindow (Shown))
	close = _Button (win, 4, Width - 112, Height - 38, 100, 28, "Close", CloseRecordsWindow)
	close.MakeEscape ()
	win.Focus ()
	UpdateRecordsWindow ()

def CloseRecordsWindow ():
	global Window, Text, DeedsButton
	DCCLuck.Close ()
	if Window:
		Window.Close ()
	Window = Text = DeedsButton = None

def OnGameStart ():
	"""a new or loaded game closed every window, ours included"""
	global Window, Text, DeedsButton
	Window = Text = DeedsButton = None
	DCCLuck.OnGameStart ()

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
