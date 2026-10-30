# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

import GemRB
import NetLobby
from GUIDefines import *

Name = "Jora the Seeker"

def OnLoad():
	# co-op clients wait in the lobby for the host's game instead of starting their own
	if GemRB.NetGetMode () == "client":
		NetLobby.OpenLobby (NewGame)
		return

	# shortcircuting support for easier development
	if GemRB.GetVar ("SkipIntroVideos") and not GemRB.NetGetConfig ()["Test"]:
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

	# co-op, only where the build can network
	if NetLobby.Available ():
		frame = NewGameButton.GetFrame ()
		# below New demo and Quit
		MultiButton = StartWindow.CreateButton (10, frame["x"], frame["y"] + 2 * (frame["h"] + 10), frame["w"], frame["h"])
		MultiButton.SetText ("Multiplayer")
		MultiButton.OnPress (lambda: NetLobby.OpenMultiplayer (NewGame))
		NetLobby.StartTest (NewGame)

	GemRB.LoadMusicPL ("theme.mus")

def NewGame():
	GemRB.LoadGame(None)
	# this is needed, so the game loop runs and the load happens
	# before other code (eg. CreatePlayer) depending on it is run
	GemRB.SetNextScript("SetupGame")
