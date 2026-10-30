# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

import GemRB
from GUIDefines import *

Name = "Jora the Seeker"

def OnLoad():
	# co-op clients wait for the host's game instead of starting their own
	if GemRB.NetGetMode () == "client":
		JoinWindow()
		return

	# shortcircuting support for easier development
	if GemRB.GetVar ("SkipIntroVideos"):
		NewGame()
		return

	StartWindow = GemRB.LoadWindow(0, "START")
	
	def NewGamePress():
		global Name
		Name = StartWindow.GetControl(3).QueryText()
		NewGame()
		
	NewGameButton = StartWindow.GetControl(0)
	QuitGameButton = StartWindow.GetControl(1)
	NewGameButton.SetText("New demo")
	QuitGameButton.SetText("Quit")
	NewGameButton.OnPress (NewGamePress)
	QuitGameButton.OnPress (lambda: GemRB.Quit())
	QuitGameButton.MakeEscape ()

	VersionLabel = StartWindow.GetControl(0x10000002)
	VersionLabel.SetText(GemRB.Version)

	NameEdit = StartWindow.GetControl(3)
	NameEdit.SetText(Name)
	NameEdit.Focus()

	GemRB.LoadMusicPL ("theme.mus")

def NewGame():
	GemRB.LoadGame(None)
	# this is needed, so the game loop runs and the load happens
	# before other code (eg. CreatePlayer) depending on it is run
	GemRB.SetNextScript("SetupGame")

def JoinWindow():
	# the engine loads the host's game once it arrives, which closes this window
	StartWindow = GemRB.LoadWindow(0, "START")

	JoinButton = StartWindow.GetControl(0)
	JoinButton.SetText("Joining host...")
	JoinButton.SetState(IE_GUI_BUTTON_DISABLED)
	QuitGameButton = StartWindow.GetControl(1)
	QuitGameButton.SetText("Quit")
	QuitGameButton.OnPress (lambda: GemRB.Quit())
	QuitGameButton.MakeEscape ()

	VersionLabel = StartWindow.GetControl(0x10000002)
	VersionLabel.SetText(GemRB.Version)
	StartWindow.GetControl(3).SetVisible(False)
