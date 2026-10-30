# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the demo's message window (GUIScripts/demo/MessageWindow.py) plus an "R"
# button for the DCC record screen and the dcctest autotest hook.

import GemRB
import Tests
import GUICommon
import CommonWindow
import GUIClasses
import GUIINV
import GUIMA
import GUIREC
import DCCAutotest
import NetChat
from GUIDefines import *

def OnLoad():
	MessageWindow = GemRB.LoadWindow(0, GUICommon.GetWindowPack(), WINDOW_BOTTOM|WINDOW_HCENTER)
	MessageWindow.SetFlags (WF_BORDERLESS|IE_GUI_VIEW_IGNORE_EVENTS, OP_OR)
	MessageWindow.AddAlias("MSGWIN")
	MessageWindow.AddAlias("HIDE_CUT", 0)

	# set up some *initial* text (UpdateControlStatus will get called several times)
	TMessageTA = MessageWindow.GetControl(0)
	TMessageTA.SetFlags (IE_GUI_TEXTAREA_AUTOSCROLL|IE_GUI_TEXTAREA_HISTORY)
	TMessageTA.SetResizeFlags(IE_GUI_VIEW_RESIZE_ALL)
	TMessageTA.AddAlias("MsgSys", 0)
	TMessageTA.AddAlias("MTA", 0)
	TMessageTA.SetColor(ColorRed, TA_COLOR_OPTIONS)
	TMessageTA.SetColor(ColorWhite, TA_COLOR_HOVER)
	
	results = Tests.RunTests ()
	TMessageTA.SetText ("[cap]D[/cap]emo " + "DEMO "*40 + "\n" + results)

	PauseButton = MessageWindow.GetControl (2)
	PauseButton.OnPress (lambda: GemRB.GamePause (2, 0))
	PauseButton.SetAnimation ("loading", 0, A_ANI_GAMEANIM)
	PauseButton.SetFlags (IE_GUI_BUTTON_PICTURE|IE_GUI_BUTTON_NORMAL, OP_SET)

	MapButton = MessageWindow.GetControl (3)
	MapButton.SetText ("M")
	MapButton.OnPress (GUIMA.OpenMapWindow)
	MapButton.OnRightPress (lambda: GemRB.MoveToArea ("ar0110"))

	CenterButton = MessageWindow.GetControl (4)
	CenterButton.SetText ("C")
	CenterButton.OnPress (lambda: GemRB.GameControlSetScreenFlags (SF_CENTERONACTOR, OP_OR))

	InventoryButton = MessageWindow.GetControl (5)
	InventoryButton.SetText ("I")
	InventoryButton.OnPress (GUIINV.OpenInventoryWindow)

	# the DCC record screen, below the inventory button
	frame = InventoryButton.GetFrame ()
	RecordButton = MessageWindow.CreateButton (6, frame["x"], frame["y"] + 55, frame["w"], frame["h"])
	RecordButton.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	RecordButton.SetBackground ({'r': 70, 'g': 60, 'b': 45, 'a': 255})
	RecordButton.SetText ("R")
	RecordButton.SetTooltip ("Character record")
	RecordButton.OnPress (lambda: GUIREC.OpenRecordsWindow ())
	GUIREC.OnGameStart ()

	# co-op: the recent chat (the lobby's too) goes back into the new message window
	NetChat.OnMessageWindow ()

	# dcctest-auto: checks the DCC numbers once the area script asks for it
	DCCAutotest.OnGameStart ()

def UpdateControlStatus():
	pass

def ToggleMenuWindow():
	pass
