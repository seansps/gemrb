# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.3): the level-1 class pick of a funnel survivor (docs/rules/funnel.md, "Advancement from
# level 0"): the player confirms or changes its alignment (the player's choice; the rolling card's was a
# default), picks a class (a demihuman takes its own; a human wizard, warrior, cleric or thief) and, for a
# cleric, a god of that alignment. Confirm sets the alignment and god (GemRB.DCCSetAlignment) and takes the
# level (GemRB.DCCLevelUp: the hit die, and for casters the spells, which the learn window, DCCLearn.py,
# then shows). Both calls go to the host in co-op, for the character's owner only; a client's learn window
# opens when the host's rolls come back.
# ChooseFour.Watch calls Next () on the roster's timer: each player gets the picks of its own characters,
# one window at a time (the four after the funnel, and a reserve that joins at level 0).

import GemRB
import FunnelRoll
import FunnelChoose
from GUIDefines import *

Font = "NORMAL"
CardFont = "INFOFONT"
WindowColor = {'r': 22, 'g': 20, 'b': 30, 'a': 250}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
LitColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}
WINDOW_ID = 81
Width = 640
Height = 430

Window = None
Shown = 0 # the character's global ID
Choice = {} # "Class", "Alignment", "God"
Waiting = None # co-op client: the character whose level the host is rolling
LearnOffered = set () # global IDs whose learn window opened by itself once
Last = {} # for tests: what the window shows

def _Log (text):
	print ("[ClassPick] " + text)

def _Tables ():
	import ChooseFour
	return ChooseFour.Tables ()

def _Mine ():
	"""the party slots this player picks for (all of them in single player)"""
	import Roster
	return Roster.Mine ()

def Pending (slot):
	record = GemRB.GetDCCFunnel (slot)
	stats = GemRB.GetDCCStats (slot)
	return bool (record["Funnel"]["ClassPending"]) and stats and stats["Level"] == 0 and stats["HP"] > 0

def AnyPending ():
	"""anyone in the party still to pick a class (any player's)"""
	return any (Pending (slot) for slot in range (1, GemRB.GetPartySize () + 1))

def Busy ():
	import DCCLearn
	return bool (Window or DCCLearn.Window or Waiting)

def Next ():
	"""opens the next pick of this player's own, or the learn window of a caster that has its spells"""
	global Waiting
	import DCCLearn
	if Waiting:
		stats = GemRB.GetDCCStats (Waiting)
		if stats and stats["Level"] >= 1:
			who = Waiting
			Waiting = None
			if DCCLearn.NeedsLearning (who) and who not in LearnOffered:
				LearnOffered.add (who)
				DCCLearn.OpenLearnWindow (who)
		return
	if Window or DCCLearn.Window:
		return
	for slot in _Mine ():
		if Pending (slot):
			Open (slot)
			return

def Open (pc):
	"""the pick for a party member (party slot or global ID)"""
	global Window, Shown, Choice
	Close ()
	record = GemRB.GetDCCFunnel (pc)
	Shown = record["GlobalID"]
	survivor = FunnelChoose.Survivor (Shown, _Tables ())
	classes = FunnelChoose.ClassesFor (_Tables ()["dccclass"], survivor["Race"])
	Choice = {"Class": classes[0][0] if len (classes) == 1 else 0, "Alignment": survivor["Alignment"], "God": survivor["God"]}
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - Height) // 3), 'w': Width, 'h': Height})
	win.AddAlias ("CLASSPICKWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, 12, 6, Width - 24, 26, Font, "%s reaches level 1: pick a class" % survivor["Name"], IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, 12, 34, Width - 24, 96, CardFont, "\n".join (FunnelChoose.CardLines (survivor)), IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	win.CreateLabel (0x10000002, 12, 136, 90, 26, CardFont, "Alignment:", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	for code in (1, 2, 3):
		_Button (win, 10 + code, 104 + (code - 1) * 100, 136, 94, 26, FunnelRoll.ALIGNMENTS[code], lambda code = code: SetAlignment (code))
	win.CreateLabel (0x10000003, 12, 170, 90, 26, CardFont, "Class:", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	for i, (ident, name) in enumerate (classes):
		_Button (win, 20 + i, 104 + i * 100, 170, 94, 26, name, lambda ident = ident: SetClass (ident))
	win.CreateLabel (0x10000004, 12, 204, 90, 26, CardFont, "God:", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	for i in range (8): # at most eight gods of one alignment
		_Button (win, 40 + i, 104 + (i % 4) * 128, 204 + (i // 4) * 30, 122, 26, "", lambda i = i: SetGod (i))
	win.CreateLabel (0x10000005, 12, 270, Width - 24, 90, CardFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	ok = _Button (win, 1, Width - 140, Height - 40, 128, 30, "Confirm", Confirm)
	ok.MakeDefault ()
	Window = win
	_Log ("%s: %s" % (survivor["Name"], ", ".join (name for ident, name in classes)))
	Refresh ()
	win.Focus ()
	return win

def _Button (win, id, x, y, w, h, text, action):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (CardFont)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	return button

def _Gods ():
	return FunnelChoose.GodsFor (_Tables ()["dccgods"], Choice["Alignment"])

def Refresh ():
	if not Window:
		return
	tables = _Tables ()
	for code in (1, 2, 3):
		Window.GetControl (10 + code).SetBackground (LitColor if Choice["Alignment"] == code else ButtonColor)
	survivor = FunnelChoose.Survivor (Shown, tables)
	classes = FunnelChoose.ClassesFor (tables["dccclass"], survivor["Race"])
	for i, (ident, name) in enumerate (classes):
		Window.GetControl (20 + i).SetBackground (LitColor if Choice["Class"] == ident else ButtonColor)
	cleric = Choice["Class"] == FunnelChoose.CLERIC
	gods = _Gods ()
	Window.GetControl (0x10000004).SetVisible (cleric)
	for i in range (8):
		button = Window.GetControl (40 + i)
		button.SetVisible (cleric and i < len (gods))
		if i < len (gods):
			button.SetText (gods[i][1])
			button.SetTooltip (FunnelChoose.GodLine (tables["dccgods"], gods[i][0])) # the domain on hover
			button.SetBackground (LitColor if Choice["God"] == gods[i][0] else ButtonColor)
	info = FunnelChoose.ClassInfo (tables["dccclass"], Choice["Class"]) if Choice["Class"] else "Pick a class."
	if cleric:
		info += "\nA cleric serves a god of its own alignment" + (": %s." % FunnelChoose.GodLine (tables["dccgods"], Choice["God"]) if Choice["God"] else ": pick one.")
	valid = FunnelChoose.PickValid (tables["dccclass"], tables["dccgods"], survivor["Race"], Choice["Class"], Choice["Alignment"], Choice["God"])
	Window.GetControl (0x10000005).SetText (info)
	Window.GetControl (1).SetState (IE_GUI_BUTTON_ENABLED if valid else IE_GUI_BUTTON_DISABLED)
	Last.clear ()
	Last.update ({"Name": survivor["Name"], "Classes": [name for ident, name in classes], "Gods": [name for ident, name in gods] if cleric else [],
		"GodLines": [FunnelChoose.GodLine (tables["dccgods"], ident) for ident, name in gods] if cleric else [],
		"Choice": dict (Choice), "Valid": valid, "Info": info})

def SetAlignment (code):
	Choice["Alignment"] = code
	# the god follows the alignment (kept if it is of it, else none)
	Choice["God"] = FunnelChoose.GodFollows (_Tables ()["dccgods"], Choice["God"], code)
	Refresh ()

def SetClass (ident):
	Choice["Class"] = ident
	if ident == FunnelChoose.CLERIC and not Choice["God"]:
		gods = _Gods ()
		Choice["God"] = gods[0][0] if gods else 0
	Refresh ()

def SetGod (index):
	gods = _Gods ()
	if 0 <= index < len (gods):
		Choice["God"] = gods[index][0]
	Refresh ()

def Choose (classID, alignment = None, god = None):
	"""for tests and scripts: the choices as if clicked"""
	if alignment:
		SetAlignment (alignment)
	SetClass (classID)
	if god is not None:
		Choice["God"] = god
		Refresh ()
	return Last.get ("Valid", False)

def Confirm ():
	"""the alignment and god, then the level (both routed to the host in co-op)"""
	global Waiting
	if not Window or not Last.get ("Valid"):
		return -1
	pc = Shown
	god = Choice["God"] if Choice["Class"] == FunnelChoose.CLERIC else -1
	aligned = GemRB.DCCSetAlignment (pc, Choice["Alignment"], god)
	hp = GemRB.DCCLevelUp (pc, Choice["Class"])
	_Log ("%s: %s %s%s -> alignment %s, hit points %+d" % (GemRB.GetPlayerName (pc, 0), FunnelRoll.ALIGNMENTS[Choice["Alignment"]],
		FunnelChoose.CLASS_NAMES.get (Choice["Class"]), " of god %d" % Choice["God"] if god > 0 else "", aligned, hp))
	Close ()
	if hp == -2:
		# a co-op client: the host rolls; the learn window opens when its rolls arrive
		Waiting = pc
	elif hp >= 0 and Choice["Class"] in FunnelChoose.CASTERS:
		import DCCLearn
		if DCCLearn.NeedsLearning (pc):
			LearnOffered.add (pc)
			DCCLearn.OpenLearnWindow (pc)
	return hp

def Close ():
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass # gone with the game already (a load, the host's new save in co-op)
		Window = None
