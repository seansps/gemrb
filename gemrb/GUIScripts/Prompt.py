# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# "Ask the owning player": the prompt window. The game (rules code, scripts, co-op travel)
# asks the player who owns a party member a question through GemRB.AskPlayer or the engine's
# PromptManager, and the game waits, paused, until it is answered or times out.
#
# The asked player gets a small modal window: yes/no, a list of choices or one or more numbers,
# and the time left. Number keys pick a choice (they don't reach the dialog's replies), Up/Down
# move the selection, +/- (or Left/Right) change a number, digits type one, Enter takes the
# selection, Esc the default. Everyone else sees a read only "Waiting for X to decide" line.
# The outcome goes to everyone's message window (the engine writes it).
#
# Shared by all game types and single player alike; built from code, so no CHU is needed. The
# engine calls OnChange () whenever the open prompt changes; nothing here runs without a prompt.
import GemRB
from GUIDefines import *

Font = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 235}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
SelectedColor = {'r': 150, 'g': 120, 'b': 60, 'a': 255}
WaitColor = {'r': 24, 'g': 22, 'b': 30, 'a': 200}

PromptWindow = None
WaitWindow = None
Shown = None # what the windows show: (id, ours to answer)
Prompt = None # GemRB.GetPrompt () as shown
Selected = 0 # yes/no and choices: what Enter takes
Values = [] # integer prompts
Field = 0 # the number +/- and digits change
TimerSet = False

GEM_UP = chr(0x83)
GEM_DOWN = chr(0x84)

def _Log (text):
	print ("[Prompt] " + text)

def _Call (function, *args):
	# a button handler without parameters (handlers that take one get the button)
	return lambda: function (*args)

def _Valid (win):
	if not win:
		return False
	try:
		win.GetFrame ()
		return True
	except RuntimeError:
		return False # gone with the game

def _Close ():
	global PromptWindow, WaitWindow
	for win in (PromptWindow, WaitWindow):
		if _Valid (win):
			win.Close ()
	PromptWindow = WaitWindow = None

def _Seconds ():
	prompt = GemRB.GetPrompt ()
	ms = prompt["Remaining"] if prompt else 0
	return (ms + 999) // 1000

def OnChange ():
	"""the engine: the open prompt changed, closed or a new one opened"""
	global Shown, Prompt
	prompt = GemRB.GetPrompt ()
	mine = bool (prompt and prompt["Mine"] and not prompt["Sent"])
	key = (prompt["ID"], mine) if prompt else None
	if key == Shown and (_Valid (PromptWindow) or _Valid (WaitWindow)):
		# the same window, eg. another voter answered: only its text changes
		Prompt = prompt
		_Tick ()
		return
	_Close ()
	Shown = key
	Prompt = prompt
	if not prompt:
		_Log ("closed")
		return
	if mine:
		_OpenPrompt (prompt)
	else:
		_OpenWaiting (prompt)
	_StartTimer ()

def _StartTimer ():
	global TimerSet
	if not TimerSet:
		TimerSet = True
		GemRB.SetTimer (_Tick, 250, -1)

def _Tick ():
	# the time left; and a window whose prompt went away without a word closes
	if not Shown:
		return
	prompt = GemRB.GetPrompt ()
	if not prompt or prompt["ID"] != Shown[0]:
		OnChange ()
		return
	if _Valid (PromptWindow):
		PromptWindow.GetControl (0x10000002).SetText ("%d s" % _Seconds ())
	elif _Valid (WaitWindow):
		WaitWindow.GetControl (0x10000000).SetText (_WaitText (prompt))

###################################################
# read only: someone else decides

def _WaitText (prompt):
	who = prompt["Waiting"] or "the host"
	what = prompt["Title"] or prompt["Text"]
	if prompt["Sent"]:
		return "Answer sent: %s (%d s)" % (what, _Seconds ())
	extra = " for %s" % prompt["For"] if prompt["For"] else ""
	return "Waiting for %s to decide%s: %s (%d s)" % (who, extra, what, _Seconds ())

def _OpenWaiting (prompt):
	global WaitWindow
	width = min (520, GemRB.GetSystemVariable (SV_WIDTH) - 20)
	frame = {'x': (GemRB.GetSystemVariable (SV_WIDTH) - width) // 2, 'y': 8, 'w': width, 'h': 26}
	WaitWindow = win = GemRB.CreateView (95, 255, frame)
	win.SetBackground (WaitColor)
	win.CreateLabel (0x10000000, 6, 0, width - 12, 26, Font, _WaitText (prompt), IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	# shown on top, but the keys stay with the game (and the chat line)
	win.SetFlags (IE_GUI_VIEW_IGNORE_EVENTS, OP_OR)
	win.Focus ()
	_Log ("waiting for %s: prompt #%d '%s'" % (prompt["Waiting"], prompt["ID"], prompt["Title"]))

###################################################
# ours to answer

def _Button (win, id, x, y, w, h, text, action, hotkey = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	if hotkey:
		button.SetHotKey (hotkey)
	return button

def _Key (win, id, key, action):
	# a key of its own: a hidden button (each button takes one hot key)
	button = win.CreateButton (id, 0, 0, 1, 1)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.OnPress (action)
	button.SetHotKey (key)
	return button

def _OpenPrompt (prompt):
	global PromptWindow, Selected, Values, Field
	kind = prompt["Kind"]
	Selected = prompt["Default"]
	Values = [field["Value"] for field in prompt["Fields"]]
	Field = 0
	width = 420
	if kind == "choice":
		body = 28 * len (prompt["Choices"])
	elif kind == "int":
		body = 30 * len (prompt["Fields"]) + 34
	else:
		body = 34
	height = 24 + 50 + body + 30
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - width) // 2, 'y': max (0, (screen[1] - height) // 3), 'w': width, 'h': height}
	PromptWindow = win = GemRB.CreateView (94, 255, frame)
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, 10, 4, width - 80, 20, Font, prompt["Title"], IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000002, width - 70, 4, 60, 20, Font, "%d s" % _Seconds (), IE_FONT_ALIGN_RIGHT | IE_FONT_ALIGN_MIDDLE)
	text = prompt["Text"]
	if prompt["For"]:
		text = "(For %s, who is away.) %s" % (prompt["For"], text)
	win.CreateLabel (0x10000001, 10, 26, width - 20, 46, Font, text, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	y = 76

	if kind == "yesno":
		_Button (win, 1, 20, y, 180, 28, "1. Yes", lambda: _Answer (1), "y")
		_Button (win, 2, width - 200, y, 180, 28, "2. No", lambda: _Answer (0), "n")
		_Key (win, 11, "1", lambda: _Answer (1))
		_Key (win, 12, "2", lambda: _Answer (0))
		_Key (win, 23, GEM_LEFT, lambda: _Select (1))
		_Key (win, 24, GEM_RIGHT, lambda: _Select (0))
	elif kind == "choice":
		for i, choice in enumerate (prompt["Choices"]):
			_Button (win, i + 1, 20, y + 28 * i, width - 40, 24, "%d. %s" % (i + 1, choice), _Call (_Answer, i), str (i + 1))
		_Key (win, 23, GEM_UP, lambda: _Select (Selected - 1))
		_Key (win, 24, GEM_DOWN, lambda: _Select (Selected + 1))
	else:
		for i, field in enumerate (prompt["Fields"]):
			row = y + 30 * i
			label = "%s (%d-%d)" % (field["Label"], field["Min"], field["Max"])
			win.CreateLabel (0x10000010 + i, 20, row, 180, 26, Font, label, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
			_Button (win, 40 + i * 4, 210, row, 30, 26, "-", _Call (_Change, i, -1))
			win.CreateLabel (0x10000020 + i, 245, row, 50, 26, Font, str (Values[i]), IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
			_Button (win, 41 + i * 4, 300, row, 30, 26, "+", _Call (_Change, i, 1))
		ok = _Button (win, 20, width // 2 - 60, y + 30 * len (prompt["Fields"]) + 4, 120, 28, "OK", Accept)
		ok.MakeDefault ()
		for digit in range (10):
			_Key (win, 50 + digit, str (digit), _Call (_Type, digit))
		_Key (win, 25, GEM_LEFT, lambda: _Change (Field, -1))
		_Key (win, 26, GEM_RIGHT, lambda: _Change (Field, 1))
		_Key (win, 27, "+", lambda: _Change (Field, 1))
		_Key (win, 28, "=", lambda: _Change (Field, 1))
		_Key (win, 29, "-", lambda: _Change (Field, -1))
		_Key (win, 23, GEM_UP, lambda: _SelectField (Field - 1))
		_Key (win, 24, GEM_DOWN, lambda: _SelectField (Field + 1))
	if kind != "int":
		_Key (win, 21, GEM_RETURN, Accept)
	_Key (win, 22, GEM_ESCAPE, Default)
	_Highlight ()
	win.ShowModal (MODAL_SHADOW_NONE)
	win.Focus ()
	_Log ("ours to answer: prompt #%d '%s' (%s), %d s" % (prompt["ID"], prompt["Title"], kind, _Seconds ()))

def _Highlight ():
	if not _Valid (PromptWindow) or not Prompt:
		return
	kind = Prompt["Kind"]
	if kind == "yesno":
		PromptWindow.GetControl (1).SetBackground (SelectedColor if Selected == 1 else ButtonColor)
		PromptWindow.GetControl (2).SetBackground (SelectedColor if Selected == 0 else ButtonColor)
	elif kind == "choice":
		for i in range (len (Prompt["Choices"])):
			PromptWindow.GetControl (i + 1).SetBackground (SelectedColor if Selected == i else ButtonColor)
	else:
		for i in range (len (Prompt["Fields"])):
			PromptWindow.GetControl (0x10000020 + i).SetText (str (Values[i]))
			PromptWindow.GetControl (0x10000010 + i).SetColor ({'r': 255, 'g': 220, 'b': 140} if i == Field else {'r': 255, 'g': 255, 'b': 255})

def _Select (index):
	global Selected
	if not Prompt:
		return
	count = 2 if Prompt["Kind"] == "yesno" else len (Prompt["Choices"])
	if Prompt["Kind"] == "yesno":
		Selected = 1 if index >= 1 else 0
	else:
		Selected = max (0, min (count - 1, index))
	_Highlight ()

def _SelectField (index):
	global Field
	if Prompt:
		Field = max (0, min (len (Values) - 1, index))
		_Highlight ()

def _Change (index, delta):
	if not Prompt or index >= len (Values):
		return
	field = Prompt["Fields"][index]
	Values[index] = max (field["Min"], min (field["Max"], Values[index] + delta))
	_SelectField (index)

def _Type (digit):
	# a digit sets the selected number, if it is in range
	if not Prompt or not Values:
		return
	field = Prompt["Fields"][Field]
	if field["Min"] <= digit <= field["Max"]:
		Values[Field] = digit
		_Highlight ()

def _Answer (value):
	if not Prompt:
		return
	values = Values if Prompt["Kind"] == "int" else [value]
	_Log ("answering prompt #%d with %s" % (Prompt["ID"], values))
	if not GemRB.AnswerPrompt (Prompt["ID"], values):
		_Log ("the answer wasn't taken")
	# the engine says what happens next (OnChange)

def Accept ():
	"""Enter: the selection (or the numbers as they stand)"""
	if Prompt and Prompt["Kind"] == "int":
		_Answer (None)
	else:
		_Answer (Selected)

def Default ():
	"""Esc: the default answer"""
	global Values
	if not Prompt:
		return
	if Prompt["Kind"] == "int":
		Values = [field["Value"] for field in Prompt["Fields"]]
		_Answer (None)
	else:
		_Answer (Prompt["Default"])

def TestPress (n):
	"""test hook: what a click on the Nth button (0: OK or Enter) does"""
	if not Prompt:
		_Log ("test press %d: no prompt" % n)
		return
	_Log ("test press %d" % n)
	kind = Prompt["Kind"]
	if n == 0:
		Accept ()
	elif kind == "yesno":
		_Answer (1 if n == 1 else 0)
	elif kind == "choice":
		_Answer (n - 1)
	elif n > 0:
		_Change (Field, 1) # "+" of the selected number
	else:
		_Change (Field, -1)
