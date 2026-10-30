# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Co-op chat during play, BG style: Enter (when no text field has the focus) opens a one
# line input over the message window, Enter sends, Esc cancels. Lines (chat, whispers and
# "X joined/left" notes) are shown by the engine in the message window, names coloured by
# player slot. '/w NAME text' whispers to one player, '/r text' answers the last whisper.
#
# Shared by all game types. A game type's message window setup calls OnMessageWindow ();
# the engine's GameControl calls OpenInput () on Enter, only in a co-op session. Without a
# session nothing here does anything.
import GemRB
import NetLobby
from GUIDefines import *

InputWindow = None
InputHeight = 24
MaxLength = 240

def _Log (text):
	print ("[NetChat] " + text)

def OnMessageWindow ():
	"""the message window was (re)made, eg. a game was loaded: the recent chat goes back in"""
	if not GemRB.NetGetMode ():
		return
	shown = GemRB.NetChatReplay ()
	_Log ("message window ready, %d recent line(s) shown again" % shown)

def _Frame ():
	# right above the message window, as wide as it is; else at the bottom of the screen
	width, height = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': 0, 'y': height - InputHeight, 'w': width, 'h': InputHeight}
	try:
		msg = GemRB.GetView ("MSGWIN")
		if msg and msg.IsVisible ():
			mf = msg.GetFrame ()
			frame = {'x': mf['x'], 'y': max (0, mf['y'] - InputHeight), 'w': mf['w'], 'h': InputHeight}
	except Exception:
		pass
	return frame

def _Valid ():
	global InputWindow
	if not InputWindow:
		return False
	try:
		InputWindow.GetFrame ()
		return True
	except RuntimeError:
		InputWindow = None # gone with the game
		return False

def OpenInput ():
	"""Enter in the game: open the chat line (or give it the focus back)"""
	global InputWindow
	if not GemRB.NetGetMode ():
		return
	if _Valid ():
		InputWindow.Focus ()
		InputWindow.GetControl (1).Focus ()
		return
	frame = _Frame ()
	InputWindow = win = GemRB.CreateView (93, 255, frame) # 255: a window
	win.SetBackground (NetLobby.OffColor)
	win.CreateLabel (0x10000000, 4, 0, 46, frame['h'], NetLobby.Font, "Say:", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	edit = win.CreateSubview (1, IE_GUI_EDIT, {'x': 50, 'y': 2, 'w': frame['w'] - 54, 'h': frame['h'] - 4}, (NetLobby.Font, MaxLength, NetLobby.Cursor))
	edit.OnDone (SendInput)
	# Esc: a hidden button, so it reaches us before the window's own Esc handling
	cancel = win.CreateButton (2, 0, 0, 1, 1)
	cancel.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	cancel.OnPress (CancelInput)
	cancel.MakeEscape ()
	win.Focus ()
	edit.Focus ()
	_Log ("input opened")

def _Close ():
	global InputWindow
	if _Valid ():
		InputWindow.Close ()
	InputWindow = None

def SendInput ():
	if not _Valid ():
		return
	text = InputWindow.GetControl (1).QueryText ()
	_Close ()
	if text.strip ():
		_Log ("sending '%s'" % text)
		GemRB.NetChat (text)
	else:
		_Log ("input closed, nothing typed")

def CancelInput ():
	_Log ("input cancelled")
	_Close ()
