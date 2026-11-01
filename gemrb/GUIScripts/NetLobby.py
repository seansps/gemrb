# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Co-op multiplayer menu, lobby and permissions screen, modelled on the BG1/BG2/IWD
# multiplayer screens: host or join, a player list, party slots the host hands out,
# ready toggles, a start button for the host and a chat line.
# The windows are built from code, so every game type can use them without a CHU of its
# own; a game type sets Font (and Cursor) to fit its art and passes its way to start a game.
import GemRB
from GUIDefines import *

Font = "NORMAL"
Cursor = "CAROT" # the text edit caret BAM, if the game has one

# colours of the plain button and window backgrounds
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
OffColor = {'r': 45, 'g': 40, 'b': 40, 'a': 255}

MenuWindow = None
LobbyWindow = None
PermissionsWindow = None
StartGame = None # the game type's new game function, run on the host once everyone is ready
# a game type's step between the lobby's Start and the game (ethren: the funnel's rolling screen, M3.1);
# it gets the function that starts the game (False if it can't: someone isn't ready any more)
BeforeStart = None
Ready = False
LastRevision = -1
TimerSet = False
Fields = {}
Shown = {} # what the lobby window shows, for the test log

def Available ():
	return GemRB.NetGetConfig ()["Available"]

def _Window (id, w, h):
	# windows outside any CHU, centred on the screen
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - w) // 2, 'y': (screen[1] - h) // 2, 'w': w, 'h': h}
	win = GemRB.CreateView (id, 255, frame)
	win.SetBackground (WindowColor)
	return win

def _Label (win, id, x, y, w, h, text, align = IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE):
	return win.CreateLabel (id, x, y, w, h, Font, text, align)

def _Button (win, id, x, y, w, h, text, action):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	return button

def _Edit (win, id, x, y, w, h, text):
	edit = win.CreateSubview (id, IE_GUI_EDIT, {'x': x, 'y': y, 'w': w, 'h': h}, (Font, 64, Cursor))
	edit.SetBackground (OffColor)
	edit.SetText (text)
	return edit

def _Enable (button, enabled):
	button.SetState (IE_GUI_BUTTON_ENABLED if enabled else IE_GUI_BUTTON_DISABLED)
	button.SetBackground (ButtonColor if enabled else OffColor)

def _Log (text):
	print ("[NetLobby] " + text)

###################################################
# main menu -> Multiplayer: host or join

def OpenMultiplayer (startGame):
	"""startGame: what the host runs once everyone is ready (eg. the demo's NewGame)"""
	global MenuWindow, StartGame
	StartGame = startGame
	config = GemRB.NetGetConfig ()
	MenuWindow = win = _Window (90, 400, 260)
	_Label (win, 0x10000000, 10, 10, 380, 24, "Multiplayer", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	_Label (win, 0x10000001, 20, 50, 110, 24, "Your name")
	Fields["Name"] = _Edit (win, 1, 140, 50, 240, 24, config["Name"])
	_Label (win, 0x10000002, 20, 85, 110, 24, "Host address")
	Fields["Host"] = _Edit (win, 2, 140, 85, 240, 24, config["Host"])
	_Label (win, 0x10000003, 20, 120, 110, 24, "Port")
	Fields["Port"] = _Edit (win, 3, 140, 120, 100, 24, str (config["Port"]))
	_Label (win, 0x10000004, 20, 150, 360, 40, "Host a game on this port, or join the host at that address.", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	_Button (win, 4, 20, 210, 110, 30, "Host", HostPress)
	_Button (win, 5, 145, 210, 110, 30, "Join", JoinPress)
	back = _Button (win, 6, 270, 210, 110, 30, "Back", BackPress)
	back.MakeEscape ()
	win.Focus ()
	return win

def _Fields ():
	name = Fields["Name"].QueryText () or "Player"
	try:
		port = int (Fields["Port"].QueryText ())
	except ValueError:
		port = GemRB.NetGetConfig ()["Port"]
	return name, Fields["Host"].QueryText (), port

def HostPress ():
	name, host, port = _Fields ()
	_Log ("Host pressed: '%s' on port %d" % (name, port))
	if not GemRB.NetHost (port, name):
		Fields["Port"].SetText (str (port)) # stays open, the log says why
		return
	_CloseMenu ()
	OpenLobby (StartGame)

def JoinPress ():
	name, host, port = _Fields ()
	_Log ("Join pressed: '%s' to %s:%d" % (name, host, port))
	if not GemRB.NetJoin (host, port, name):
		return
	_CloseMenu ()
	OpenLobby (StartGame)

def BackPress ():
	_CloseMenu ()

def _CloseMenu ():
	global MenuWindow
	if MenuWindow:
		MenuWindow.Close ()
		MenuWindow = None

###################################################
# the lobby

# party slots the lobby can show: six in stock games, up to 16 in DCC games (MaxPartySize)
MAX_SLOTS = 16

def _SlotFrame (i, compact = False):
	"""x, y, label width, button width, height of party slot i (0-based)"""
	if not compact:
		return 310, 76 + i * 24, 150, 145, 22
	column, row = divmod (i, 8)
	return 310 + column * 152, 76 + row * 18, 94, 52, 17

def OpenLobby (startGame = None):
	global LobbyWindow, LastRevision, Ready, StartGame
	if startGame:
		StartGame = startGame
	Ready = False
	LastRevision = -1
	lobby = GemRB.NetGetLobby ()
	host = lobby and lobby["Mode"] == "host"
	LobbyWindow = win = _Window (91, 620, 460)
	_Label (win, 0x10000000, 10, 6, 600, 22, "Co-op lobby", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	_Label (win, 0x10000001, 10, 28, 600, 20, "", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE) # status

	# players, as in BG's party screen: slot, name, ready, ping
	_Label (win, 0x10000002, 10, 54, 290, 20, "Players")
	for i in range (4):
		_Label (win, 0x10000010 + i, 10, 76 + i * 22, 290, 20, "")

	# party slots: whose character each one is; the host hands them out. Six as in BG; DCC games
	# have up to 16 (the level-0 funnel), shown in two compact columns of eight
	_Label (win, 0x10000003, 310, 54, 300, 20, "Party")
	for i in range (MAX_SLOTS):
		x, y, lw, bw, h = _SlotFrame (i)
		_Label (win, 0x10000020 + i, x, y, lw, h, "")
		owner = _Button (win, 20 + i, x + lw + 3, y, bw, h, "", lambda slot = i + 1: AssignPress (slot))
		_Enable (owner, host)

	_Button (win, 10, 10, 200, 140, 28, "Ready", ReadyPress)
	perms = _Button (win, 11, 160, 200, 140, 28, "Permissions", PermissionsPress)
	start = _Button (win, 12, 310, 230, 145, 28, "Start game", StartPress)
	leave = _Button (win, 13, 465, 230, 145, 28, "Leave", LeavePress)
	leave.MakeEscape ()
	perms.SetVisible (host)
	start.SetVisible (host)

	# chat: the lobby's share of the chat message, the in-game chat window comes later
	_Label (win, 0x10000004, 10, 266, 600, 20, "Chat")
	chat = win.CreateTextArea (14, 10, 288, 600, 124, Font)
	chat.SetBackground (OffColor)
	Fields["Chat"] = _Edit (win, 15, 10, 420, 500, 26, "")
	send = _Button (win, 16, 520, 420, 90, 26, "Send", SendPress)
	send.MakeDefault ()

	win.Focus ()
	_StartTimer ()
	Refresh (True)
	return win

def _StartTimer ():
	global TimerSet
	if not TimerSet:
		TimerSet = True
		GemRB.SetTimer (_Tick, 250, -1)

def _Tick ():
	global LobbyWindow
	try:
		Refresh ()
	except Exception:
		# the window went away, eg. with the host's game being loaded
		LobbyWindow = None
	_RunTest ()

def _Player (lobby, slot):
	for player in lobby["Players"]:
		if player["Slot"] == slot:
			return player
	return None

def Refresh (force = False):
	global LastRevision, LobbyWindow
	if not LobbyWindow:
		return
	lobby = GemRB.NetGetLobby ()
	if not lobby:
		return
	if lobby["Revision"] == LastRevision and not force:
		return
	LastRevision = lobby["Revision"]
	win = LobbyWindow
	if not win.GetControl (0x10000001):
		LobbyWindow = None # gone with the game being loaded
		return
	host = lobby["Mode"] == "host"
	status = lobby["Status"]
	if host:
		status = "Hosting: waiting for everyone to be ready" if lobby["Phase"] == "lobby" else "Hosting: the game runs"
	Shown["Status"] = "%s (you are player %d)" % (status, lobby["Slot"])
	win.GetControl (0x10000001).SetText (Shown["Status"])

	for i in range (4):
		label = win.GetControl (0x10000010 + i)
		player = lobby["Players"][i] if i < len (lobby["Players"]) else None
		if not player:
			label.SetText ("")
			continue
		text = "%d: %s%s" % (player["Slot"], player["Name"], " (host)" if player["Slot"] == 0 else "")
		if not player["Connected"]:
			text += " - away"
		elif player["Ready"]:
			text += " - ready"
		if player["Ping"]:
			text += " %d ms" % player["Ping"]
		label.SetText (text)

	compact = len (lobby["Party"]) > 6
	for i in range (MAX_SLOTS):
		label = win.GetControl (0x10000020 + i)
		button = win.GetControl (20 + i)
		slot = lobby["Party"][i] if i < len (lobby["Party"]) else None
		x, y, lw, bw, h = _SlotFrame (i, compact)
		label.SetFrame ({"x": x, "y": y, "w": lw, "h": h})
		button.SetFrame ({"x": x + lw + 3, "y": y, "w": bw, "h": h})
		label.SetVisible (slot is not None)
		button.SetVisible (slot is not None)
		if not slot:
			continue
		name = slot["Name"] or "(empty)"
		owner = _Player (lobby, slot["Owner"])
		text = "Player %d %s" % (slot["Owner"], owner["Name"] if owner else "")
		if slot["Controller"] != slot["Owner"]:
			text += " (host now)"
		if compact:
			label.SetText ("%d %s" % (slot["Slot"], name[:9]))
			button.SetText ("P%d%s" % (slot["Owner"], "*" if slot["Controller"] != slot["Owner"] else ""))
			button.SetTooltip (text)
		else:
			label.SetText ("PC %d: %s" % (slot["Slot"], name))
			button.SetText (text)
		Shown["PC %d" % slot["Slot"]] = text

	me = _Player (lobby, lobby["Slot"])
	Shown["Ready"] = "Ready: yes" if me and me["Ready"] else "Ready: no"
	win.GetControl (10).SetText (Shown["Ready"])
	if host:
		_Enable (win.GetControl (12), lobby["CanStart"])
	chat = win.GetControl (14)
	chat.SetText ("\n".join (lobby["Chat"][-8:]))
	if PermissionsWindow:
		RefreshPermissions (lobby)

def AssignPress (partySlot):
	# each press hands the slot to the next player the host knows of
	lobby = GemRB.NetGetLobby ()
	if not lobby or lobby["Mode"] != "host":
		return
	slots = [player["Slot"] for player in lobby["Players"]]
	current = [slot["Owner"] for slot in lobby["Party"] if slot["Slot"] == partySlot]
	current = current[0] if current else 0
	nextOwner = slots[(slots.index (current) + 1) % len (slots)] if current in slots else slots[0]
	_Log ("Party slot %d -> player %d" % (partySlot, nextOwner))
	GemRB.NetAssign (partySlot, nextOwner)
	Refresh (True)

def ReadyPress ():
	lobby = GemRB.NetGetLobby ()
	me = lobby and _Player (lobby, lobby["Slot"])
	ready = not (me and me["Ready"])
	_Log ("Ready pressed: %s" % ready)
	GemRB.NetSetReady (ready)

def StartPress ():
	if BeforeStart:
		lobby = GemRB.NetGetLobby ()
		if not lobby or not lobby["CanStart"]:
			_Log ("Start pressed, but not everyone is ready")
			return
		_Log ("Start pressed: the game type's step before the game")
		CloseLobby ()
		BeforeStart (_StartNow)
		return
	_StartNow ()

def _StartNow ():
	if not GemRB.NetStartGame ():
		_Log ("Start pressed, but not everyone is ready")
		return False
	_Log ("Start pressed: starting the game")
	CloseLobby ()
	if StartGame:
		StartGame ()
	return True

def LeavePress ():
	_Log ("Leave pressed")
	GemRB.NetLeave ()
	CloseLobby ()

def SendPress ():
	text = Fields["Chat"].QueryText ()
	if not text:
		return
	GemRB.NetChat (text)
	Fields["Chat"].SetText ("")

def CloseLobby ():
	global LobbyWindow
	ClosePermissions ()
	if LobbyWindow:
		try:
			LobbyWindow.Close ()
		except RuntimeError:
			pass
		LobbyWindow = None

###################################################
# the host's permissions screen: per player, what BG's permissions screen offers

def PermissionsPress ():
	global PermissionsWindow
	lobby = GemRB.NetGetLobby ()
	if not lobby or lobby["Mode"] != "host":
		return
	names = lobby["PermissionNames"]
	PermissionsWindow = win = _Window (92, 620, 220)
	_Label (win, 0x10000000, 10, 6, 600, 22, "Permissions", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	for col, name in enumerate (names):
		_Label (win, 0x10000010 + col, 130 + col * 80, 34, 78, 34, name.title (), IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	for row in range (3):
		_Label (win, 0x10000020 + row, 10, 72 + row * 30, 115, 26, "")
		for col, name in enumerate (names):
			_Button (win, 100 + row * 10 + col, 130 + col * 80, 72 + row * 30, 76, 26, "", lambda r = row, n = name: TogglePermission (r, n))
	done = _Button (win, 1, 250, 175, 120, 28, "Done", ClosePermissions)
	done.MakeEscape ()
	win.Focus ()
	RefreshPermissions (lobby)

def _PermissionRows (lobby):
	return [player for player in lobby["Players"] if player["Slot"] != 0]

def RefreshPermissions (lobby):
	win = PermissionsWindow
	rows = _PermissionRows (lobby)
	for row in range (3):
		player = rows[row] if row < len (rows) else None
		win.GetControl (0x10000020 + row).SetText ("%d: %s" % (player["Slot"], player["Name"]) if player else "")
		for col, name in enumerate (lobby["PermissionNames"]):
			button = win.GetControl (100 + row * 10 + col)
			button.SetVisible (player is not None)
			if player:
				allowed = player["Permissions"][name]
				button.SetText ("Yes" if allowed else "No")
				button.SetBackground (ButtonColor if allowed else OffColor)

def TogglePermission (row, name):
	lobby = GemRB.NetGetLobby ()
	rows = _PermissionRows (lobby)
	if row >= len (rows):
		return
	player = rows[row]
	allowed = not player["Permissions"][name]
	_Log ("Permission %s for player %d -> %s" % (name, player["Slot"], allowed))
	GemRB.NetSetPermission (player["Slot"], name, allowed)
	RefreshPermissions (GemRB.NetGetLobby ())

def ClosePermissions ():
	global PermissionsWindow
	if PermissionsWindow:
		try:
			PermissionsWindow.Close ()
		except RuntimeError:
			pass
		PermissionsWindow = None

###################################################
# NetTestLobby=T:step;T:step... drives these windows the way a player would (T in seconds
# since the test started): it types into the fields and calls the buttons' own handlers.
# Steps: host NAME PORT | join NAME HOST PORT | ready 0|1 | assign PC PLAYER |
# perm PLAYER NAME 0|1 (NAME with _ for spaces) | chat TEXT | start | leave | dump |
# py CODE (GUI script code, eg. a game type's screen between Start and the game; | for line breaks)

TestSteps = []
TestClock = 0

def StartTest (startGame):
	"""returns True if NetTestLobby is set; it then runs from the multiplayer menu"""
	global TestSteps
	script = GemRB.NetGetConfig ()["Test"]
	if not script:
		return False
	for step in script.split (";"):
		when, _, what = step.partition (":")
		TestSteps.append ((float (when), what.strip ()))
	OpenMultiplayer (startGame)
	_StartTimer ()
	return True

def _RunTest ():
	global TestClock
	if not TestSteps:
		return
	TestClock += 0.25
	while TestSteps and TestSteps[0][0] <= TestClock:
		when, step = TestSteps.pop (0)
		_Log ("test step at %.2f s: %s" % (TestClock, step))
		try:
			_TestStep (step.split (" "))
		except Exception as e:
			_Log ("test step failed: %r" % e)

def _TestStep (words):
	verb = words[0]
	lobby = GemRB.NetGetLobby ()
	if verb in ("host", "join"):
		Fields["Name"].SetText (words[1])
		if verb == "join":
			Fields["Host"].SetText (words[2])
		Fields["Port"].SetText (words[-1])
		HostPress () if verb == "host" else JoinPress ()
	elif verb == "ready":
		me = _Player (lobby, lobby["Slot"])
		if bool (int (words[1])) != bool (me and me["Ready"]):
			ReadyPress ()
	elif verb == "assign":
		partySlot, player = int (words[1]), int (words[2])
		for i in range (4):
			lobby = GemRB.NetGetLobby ()
			owner = [slot["Owner"] for slot in lobby["Party"] if slot["Slot"] == partySlot]
			if owner and owner[0] == player:
				break
			AssignPress (partySlot)
	elif verb == "perm":
		slot, name, allowed = int (words[1]), words[2].replace ("_", " "), bool (int (words[3]))
		if not PermissionsWindow:
			PermissionsPress ()
		rows = _PermissionRows (lobby)
		for row, player in enumerate (rows):
			if player["Slot"] == slot and player["Permissions"][name] != allowed:
				TogglePermission (row, name)
	elif verb == "closeperms":
		ClosePermissions ()
	elif verb == "chat":
		Fields["Chat"].SetText (" ".join (words[1:]))
		SendPress ()
	elif verb == "start":
		StartPress ()
	elif verb == "leave":
		LeavePress ()
	elif verb == "py":
		exec (" ".join (words[1:]).replace ("|", "\n"), globals ())
	elif verb == "dump":
		if not lobby:
			_Log ("no session")
			return
		_Log ("lobby: %s, slot %d, status '%s', can start %s" % (lobby["Phase"], lobby["Slot"], lobby["Status"], lobby["CanStart"]))
		for player in lobby["Players"]:
			perms = " ".join (("+" if allowed else "-") + name.replace (" ", "_") for name, allowed in sorted (player["Permissions"].items ()))
			_Log ("  player %d '%s' connected %s ready %s ping %d: %s" % (player["Slot"], player["Name"], player["Connected"], player["Ready"], player["Ping"], perms))
		for slot in lobby["Party"]:
			card = slot["Card"].split ("\n")[0] if slot["Card"] else ""
			_Log ("  PC %d '%s' owner %d controller %d%s" % (slot["Slot"], slot["Name"], slot["Owner"], slot["Controller"], " card '%s'" % card if card else ""))
		_Log ("  chat: %s" % " | ".join (lobby["Chat"]))
		if LobbyWindow:
			_Log ("  window shows: %s" % " | ".join ("%s: %s" % item for item in sorted (Shown.items ())))
	else:
		_Log ("unknown test step '%s'" % verb)
