# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the learn window after a level-up (Phase 3: the funnel's level-1 class pick). The
# engine learns the spells when the level comes (DCCLevelUp: random on Table 5-8 for wizards
# and elves, a default choice from the god's list for clerics, with mercurial magic); this
# window shows them and lets the player choose where the rules allow:
# - clerics choose their spells: any new one can be swapped for another of that level;
# - wizards and elves may swap up to half of the random ones (the book's optional rule);
# - an elf of level 1 names its lucky spell (its Luck modifier is added to that one, frozen).
# Rows the game has no content for yet were substituted, or left open; the notes say which.
# Confirm keeps the spells for good (DCCConfirmSpells). Swaps and the confirm are routed to the
# host in co-op (GemRB.DCCReplaceSpell / DCCConfirmSpells, the owner only).

import GemRB
from GUIDefines import *

Font = "NORMAL"
WindowColor = {'r': 22, 'g': 20, 'b': 34, 'a': 250}
ButtonColor = {'r': 60, 'g': 55, 'b': 80, 'a': 255}
ChosenColor = {'r': 120, 'g': 90, 'b': 160, 'a': 255}
Width = 560

Window = None
Shown = 0
Lucky = "" # the elf's pick so far
ChoiceIndex = {} # fresh spell -> the choice its swap button offers
Last = {} # for tests: what the window shows

def NeedsLearning (pc):
	magic = GemRB.GetDCCMagic (pc)
	return bool (magic and magic["Fresh"])

def _Fresh (magic):
	return [spell for spell in magic["Spells"] if spell["Fresh"]]

def _Choices (magic, level):
	choices = magic["Choices"]
	return choices[level - 1] if 0 < level <= len (choices) else []

def OpenLearnWindow (pc = None):
	global Window, Shown, Lucky
	if pc:
		Shown = pc
	Shown = Shown or GemRB.GameGetSelectedPCSingle () or 1
	magic = GemRB.GetDCCMagic (Shown)
	if not magic:
		return None
	fresh = _Fresh (magic)
	if not Lucky:
		lucky = [spell["Spell"] for spell in fresh if spell["Lucky"]]
		Lucky = lucky[0] if lucky else ""
	notes = magic["LearnNotes"]
	height = 90 + 36 * max (1, len (fresh)) + 22 * min (8, len (notes)) + 60
	Close (False)
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - height) // 4), 'w': Width, 'h': height}
	win = Window = GemRB.CreateView (97, 255, frame)
	win.AddAlias ("WIN_DCCLEARN")
	win.SetBackground (WindowColor)
	name = GemRB.GetPlayerName (Shown, 1)
	kind = "cleric" if magic["CasterKind"] == 2 else "arcane caster"
	swaps = magic["Swaps"]
	rule = "choose from the god's list" if magic["CasterKind"] == 2 else "may swap %d of the random ones" % swaps
	win.CreateLabel (1, 12, 6, Width - 24, 22, Font, "%s learns spells (%s): %s" % (name, kind, rule), IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	y = 34
	Last.clear ()
	Last["Spells"] = []
	for i, spell in enumerate (fresh):
		text = spell["Name"].replace ("_", " ")
		if spell["Mercurial"]:
			text += " (mercurial %s)" % ", ".join (str (m) for m in spell["Mercurial"])
		if spell["Replaced"]:
			text += " - chosen"
		win.CreateLabel (100 + i, 12, y, 300, 30, Font, text, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		Last["Spells"].append (spell["Spell"])
		choices = _Choices (magic, spell["Level"])
		if choices and (swaps > 0 or spell["Replaced"]):
			index = ChoiceIndex.get (spell["Spell"], 0) % len (choices)
			offer = choices[index]
			swap = _Button (win, 10 + i, 316, y, 150, 30, "Swap: " + offer["Name"].replace ("_", " "), lambda old = spell["Spell"], new = offer["Spell"]: Swap (old, new))
			swap.SetTooltip ("Right click: another choice")
			swap.OnRightPress (lambda old = spell["Spell"], n = len (choices): _NextChoice (old, n))
		if magic["PicksLucky"]:
			_Button (win, 40 + i, 472, y, 76, 30, "Lucky", lambda s = spell["Spell"]: PickLucky (s), Lucky == spell["Spell"])
		y += 36
	if not fresh:
		win.CreateLabel (100, 12, y, Width - 24, 30, Font, "No new spells.", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		y += 36
	for j, note in enumerate (notes[:8]):
		win.CreateLabel (200 + j, 12, y, Width - 24, 22, Font, note, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		y += 22
	Last["Notes"] = list (notes)
	Last["Lucky"] = Lucky
	Last["Swaps"] = swaps
	ok = _Button (win, 50, Width - 232, height - 40, 110, 30, "Confirm", Confirm)
	ok.MakeDefault ()
	close = _Button (win, 51, Width - 116, height - 40, 104, 30, "Later", lambda: Close (True))
	close.MakeEscape ()
	win.Focus ()
	return win

def _Button (win, id, x, y, w, h, text, action, chosen = False):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ChosenColor if chosen else ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	return button

def _NextChoice (spell, count):
	ChoiceIndex[spell] = (ChoiceIndex.get (spell, 0) + 1) % max (1, count)
	OpenLearnWindow ()

def Swap (old, new):
	global Lucky
	ok = GemRB.DCCReplaceSpell (Shown, old, new)
	if ok and Lucky == old:
		Lucky = new
	ChoiceIndex.pop (old, None)
	OpenLearnWindow ()
	return ok

def PickLucky (spell):
	global Lucky
	Lucky = spell
	OpenLearnWindow ()

def Confirm ():
	global Lucky
	magic = GemRB.GetDCCMagic (Shown)
	lucky = Lucky if magic and magic["PicksLucky"] else ""
	done = GemRB.DCCConfirmSpells (Shown, lucky)
	Lucky = ""
	ChoiceIndex.clear ()
	Close (True)
	return done

def Close (refresh = True):
	global Window
	if Window:
		try:
			Window.Close ()
		except Exception:
			pass
	Window = None
