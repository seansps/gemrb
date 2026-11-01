# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the party settings, so a funnel of 16 doesn't stall on prompts. Sets the per-character
# policies in bulk, for every character this player controls (the whole party in single player):
# when Luck burns on its own (DCCSetLuckPolicy), whether key rolls pause and ask, spellburn
# (DCCSetSpellburn: wizards and elves only) and when Luck cancels a corruption. The host also
# sets the game's prompt options (DCCSetPromptOptions): auto-resolve after N seconds with the
# character's policy, and whether level-0 characters ask at all. M2.10: what each character does
# on its own while the party AI is on (DCCSetPartyAI: defend, flee when hurt, passive, engage).
# Built from code, no CHU.

import GemRB
from GUIDefines import *
from ie_stats import *

Font = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 255}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
OnColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}

LUCK_POLICIES = [("Never", DCC_BURN_NEVER), ("Avoid death", DCC_BURN_AVOID_DISASTER), ("Misses too", DCC_BURN_MISS_TO_HIT)]
SPELLBURN = [("Never", 0), ("Ask", 1), ("Preset", 2)]
CORRUPTION = [("Never", 0), ("Greater", 1), ("Major+", 2), ("Always", 3)]
PARTY_AI = [("Defend", DCC_AI_DEFEND), ("Flee hurt", DCC_AI_FLEE), ("Passive", DCC_AI_PASSIVE), ("Engage", DCC_AI_ENGAGE)]
AUTO_SECONDS = [0, 5, 10, 20, 30]
MISS_THRESHOLD = 10 # "misses too" keeps this much Luck (the M2.4 default)

SettingsWindow = None
LastApplied = {} # what the last button set, for tests

def Mine ():
	"""the party slots this player controls: everyone in single player, else the lobby's"""
	size = GemRB.GetPartySize ()
	lobby = GemRB.NetGetLobby () if hasattr (GemRB, "NetGetLobby") else None
	if not lobby:
		return list (range (1, size + 1))
	me = lobby["Slot"]
	return [slot["Slot"] for slot in lobby["Party"] if slot["Controller"] == me and slot["Slot"] <= size]

def IsHost ():
	mode = GemRB.NetGetMode () if hasattr (GemRB, "NetGetMode") else ""
	return mode != "client"

def SetLuckPolicy (policy, pause = None):
	"""every character of ours: the Luck policy, and pause on key rolls (None keeps each one's)"""
	count = 0
	for pc in Mine ():
		stats = GemRB.GetDCCStats (pc)
		if not stats or not stats.get ("Character", 0):
			continue
		flags = stats.get ("LuckFlags", DCC_LUCK_PAUSE | DCC_LUCK_ALLIES)
		if pause is not None:
			flags = (flags | DCC_LUCK_PAUSE) if pause else (flags & ~DCC_LUCK_PAUSE)
		if policy is None:
			policy_now = stats.get ("LuckPolicy", DCC_BURN_AVOID_DISASTER)
		else:
			policy_now = policy
		threshold = MISS_THRESHOLD if policy == DCC_BURN_MISS_TO_HIT else stats.get ("LuckThreshold", MISS_THRESHOLD)
		GemRB.DCCSetLuckPolicy (pc, policy_now, threshold, flags)
		count += 1
	LastApplied["Luck"] = (policy, pause, count)
	return count

def SetSpellburn (policy, corruption = None):
	"""our wizards and elves: the spellburn policy (preset 0), and the corruption Luck burn"""
	count = 0
	for pc in Mine ():
		magic = GemRB.GetDCCMagic (pc)
		if not magic or magic["CasterKind"] != 1:
			continue
		burn = magic["Spellburn"]
		GemRB.DCCSetSpellburn (pc, burn["Policy"] if policy is None else policy, burn["Preset"], burn["Corruption"] if corruption is None else corruption)
		count += 1
	LastApplied["Spellburn"] = (policy, corruption, count)
	return count

def SetPartyAI (mode):
	"""every character of ours: what it does on its own while the party AI is on"""
	count = 0
	for pc in Mine ():
		stats = GemRB.GetDCCStats (pc)
		if not stats:
			continue
		GemRB.DCCSetPartyAI (pc, mode)
		count += 1
	LastApplied["PartyAI"] = (mode, count)
	return count

def SetPromptOptions (seconds, level0Ask):
	if not IsHost ():
		return False
	return GemRB.DCCSetPromptOptions (seconds, level0Ask)

def OpenPartySettings ():
	global SettingsWindow
	if SettingsWindow:
		ClosePartySettings ()
	width, height = 520, 332
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - width) // 2, 'y': (screen[1] - height) // 3, 'w': width, 'h': height}
	win = GemRB.CreateView (96, 255, frame)
	win.AddAlias ("PARTYSET")
	win.SetBackground (WindowColor)
	scope = "the whole party" if len (Mine ()) == GemRB.GetPartySize () else "your %d characters" % len (Mine ())
	win.CreateLabel (0x10000000, 10, 6, width - 20, 20, Font, "Party settings: for %s" % scope, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)

	y = 34
	_Row (win, 0x10000001, y, "Burn Luck on its own")
	for i, (name, value) in enumerate (LUCK_POLICIES):
		_Button (win, 10 + i, 170 + i * 112, y, 108, 24, name, lambda v = value: (SetLuckPolicy (v), _Refresh ()))
	y += 32
	_Row (win, 0x10000002, y, "Pause on key rolls")
	_Button (win, 20, 170, y, 108, 24, "Ask", lambda: (SetLuckPolicy (None, True), _Refresh ()))
	_Button (win, 21, 282, y, 108, 24, "Don't ask", lambda: (SetLuckPolicy (None, False), _Refresh ()))
	y += 32
	_Row (win, 0x10000003, y, "Spellburn")
	for i, (name, value) in enumerate (SPELLBURN):
		_Button (win, 30 + i, 170 + i * 112, y, 108, 24, name, lambda v = value: (SetSpellburn (v), _Refresh ()))
	y += 32
	_Row (win, 0x10000004, y, "Luck vs corruption")
	for i, (name, value) in enumerate (CORRUPTION):
		_Button (win, 40 + i, 170 + i * 86, y, 82, 24, name, lambda v = value: (SetSpellburn (None, v), _Refresh ()))
	y += 32
	_Row (win, 0x10000007, y, "On its own (AI)")
	for i, (name, value) in enumerate (PARTY_AI):
		_Button (win, 60 + i, 170 + i * 86, y, 82, 24, name, lambda v = value: (SetPartyAI (v), _Refresh ()))
	y += 40
	host = IsHost ()
	_Row (win, 0x10000005, y, "Auto-answer prompts")
	auto = _Button (win, 50, 170, y, 160, 24, "", _CycleAuto)
	auto.SetTooltip ("An unanswered prompt takes the character's policy after this long (host)")
	y += 32
	_Row (win, 0x10000006, y, "Level-0 characters")
	loud = _Button (win, 51, 170, y, 160, 24, "", _ToggleLoud)
	loud.SetTooltip ("Whether the funnel's peasants pause and ask on key rolls (host)")
	auto.SetDisabled (not host)
	loud.SetDisabled (not host)
	close = _Button (win, 99, width - 120, height - 36, 110, 28, "Close", ClosePartySettings)
	close.MakeEscape ()
	SettingsWindow = win
	_Refresh ()
	win.Focus ()
	return win

def ClosePartySettings ():
	global SettingsWindow
	if SettingsWindow:
		try:
			SettingsWindow.Close ()
		except Exception:
			pass
	SettingsWindow = None

def _Row (win, id, y, text):
	win.CreateLabel (id, 10, y, 156, 24, Font, text, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)

def _Button (win, id, x, y, w, h, text, action):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	# the engine passes the button to a handler that takes arguments, which would fill a lambda's
	# defaults (lambda i = i: ...): the action is called without any
	button.OnPress (lambda: action ())
	return button

def _CycleAuto ():
	now = GemRB.GetGameVar ("DCC_PROMPT_AUTO")
	nxt = AUTO_SECONDS[(AUTO_SECONDS.index (now) + 1) % len (AUTO_SECONDS)] if now in AUTO_SECONDS else 0
	SetPromptOptions (nxt, GemRB.GetGameVar ("DCC_PROMPT_LOUD0"))
	_Refresh ()

def _ToggleLoud ():
	SetPromptOptions (GemRB.GetGameVar ("DCC_PROMPT_AUTO"), 0 if GemRB.GetGameVar ("DCC_PROMPT_LOUD0") else 1)
	_Refresh ()

def _Refresh ():
	win = SettingsWindow
	if not win:
		return
	auto = GemRB.GetGameVar ("DCC_PROMPT_AUTO")
	win.GetControl (50).SetText ("After %d s" % auto if auto else "Off (usual timeouts)")
	win.GetControl (51).SetText ("Ask too" if GemRB.GetGameVar ("DCC_PROMPT_LOUD0") else "Never ask")
	# light up a setting every one of ours shares
	mine = Mine ()
	stats = [GemRB.GetDCCStats (pc) for pc in mine]
	policies = set (s.get ("LuckPolicy") for s in stats if s)
	pauses = set (bool (s.get ("LuckFlags", 0) & DCC_LUCK_PAUSE) for s in stats if s)
	for i, (name, value) in enumerate (LUCK_POLICIES):
		win.GetControl (10 + i).SetBackground (OnColor if policies == {value} else ButtonColor)
	win.GetControl (20).SetBackground (OnColor if pauses == {True} else ButtonColor)
	win.GetControl (21).SetBackground (OnColor if pauses == {False} else ButtonColor)
	casters = [GemRB.GetDCCMagic (pc) for pc in mine]
	casters = [m for m in casters if m and m["CasterKind"] == 1]
	burns = set (m["Spellburn"]["Policy"] for m in casters)
	corruptions = set (m["Spellburn"]["Corruption"] for m in casters)
	for i, (name, value) in enumerate (SPELLBURN):
		win.GetControl (30 + i).SetBackground (OnColor if burns == {value} else ButtonColor)
	for i, (name, value) in enumerate (CORRUPTION):
		win.GetControl (40 + i).SetBackground (OnColor if corruptions == {value} else ButtonColor)
	modes = set (s.get ("PartyAI") for s in stats if s)
	for i, (name, value) in enumerate (PARTY_AI):
		win.GetControl (60 + i).SetBackground (OnColor if modes == {value} else ButtonColor)
