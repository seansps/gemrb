# SPDX-FileCopyrightText: 2012 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the demo's start screen (GUIScripts/demo/Start.py) with the funnel (M3.1): a new game opens
# the rolling screen (RollScreen.py) first when dccstart.2da says ROLL 1, and SetupGame makes the
# sixteen peasants from the accepted cards. In co-op the host rolls between the lobby's Start and the
# game (NetLobby.BeforeStart), and clients watch the cards from their lobby (RollScreen.WatchLobby).

import GemRB
import NetLobby
import RollScreen
from GUIDefines import *

Name = "Jora the Seeker"
StartWindow = None

def OnLoad():
	RollScreen.Accepted = None
	# a co-op client (from the config or the Multiplayer menu) sees the host's cards while it waits
	RollScreen.WatchLobby ()
	# co-op clients wait in the lobby for the host's game instead of starting their own
	if GemRB.NetGetMode () == "client":
		NetLobby.OpenLobby (NewGame)
		return

	# the host rolls the sixteen once everyone is ready, before the game starts (M3.1)
	NetLobby.BeforeStart = RollScreen.OpenForLobby if RollScreen.Rolling () else None

	# shortcircuting support for easier development
	if GemRB.GetVar ("SkipIntroVideos") and not GemRB.NetGetConfig ()["Test"]:
		NewGame()
		return
	ShowStartWindow ()

def ShowStartWindow ():
	global StartWindow
	StartWindow = GemRB.LoadWindow(0, "START")

	def NewGamePress():
		global Name
		Name = StartWindow.GetControl(3).QueryText()
		NewGame()

	NewGameButton = StartWindow.GetControl(0)
	QuitGameButton = StartWindow.GetControl(1)
	NewGameButton.SetText("New game" if RollScreen.Rolling () else "New demo")
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
		# below New game and Quit
		MultiButton = StartWindow.CreateButton (10, frame["x"], frame["y"] + 2 * (frame["h"] + 10), frame["w"], frame["h"])
		MultiButton.SetText ("Multiplayer")
		MultiButton.OnPress (lambda: NetLobby.OpenMultiplayer (NewGame))
		NetLobby.StartTest (NewGame)

	GemRB.LoadMusicPL ("theme.mus")

def NewGame():
	# the funnel: roll the sixteen first (single player; the co-op host has rolled them already)
	if RollScreen.Rolling () and RollScreen.Accepted is None and GemRB.NetGetMode () != "host":
		RollScreen.Open (StartGame, BackToStart)
		return
	StartGame ()

def StartGame():
	GemRB.LoadGame(None)
	# this is needed, so the game loop runs and the load happens
	# before other code (eg. CreatePlayer) depending on it is run
	GemRB.SetNextScript("SetupGame")

def BackToStart ():
	if not StartWindow:
		ShowStartWindow ()
