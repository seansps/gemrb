# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The ethren spellbook (M2.5), opened from the record screen (GUIREC "Spells"):
# - the spells the caster knows (not memorized slots): a button each, greyed while the
#   spell is lost for the day; pressing one casts it (target as usual), the spell check
#   rolls when the casting ends. Mercurial magic and the manifestation are listed.
# - wizards and elves: the spellburn setting (never, ask before each check, always the
#   preset), the preset points, and when 1 Luck cancels a corruption on its own.
# - clerics: the disapproval range, atonement (50 gp a point of range), lay on hands
#   (the most wounded party member) and turn unholy.
# The record screen's sheet gets the magic lines from Lines (pc): caster level, spells,
# the corruptions with what they do, spellburn and disapproval.
# Changes go through GemRB.DCCSetSpellburn, DCCAtone, DCCLayOnHands and DCCTurnUnholy, which
# a co-op client sends to the host (only the character's owner may use them).
import GemRB
from GUIDefines import *
from ie_stats import *

Font = "NORMAL"
WindowColor = {'r': 22, 'g': 20, 'b': 34, 'a': 245}
ButtonColor = {'r': 60, 'g': 55, 'b': 80, 'a': 255}
ChosenColor = {'r': 120, 'g': 90, 'b': 160, 'a': 255}
Width = 460

SpellburnPolicies = ((0, "Never"), (1, "Ask"), (2, "Preset"))
CorruptionPolicies = ((0, "Never"), (1, "Greater"), (2, "Major+"), (3, "Always"))
PolicyNames = {0: "never", 1: "asks before each check", 2: "always the preset"}
CorruptionNames = {0: "never", 1: "only greater corruption", 2: "major and greater corruption", 3: "every corruption"}
Tables = {0: "the spell's own", 1: "minor", 2: "major", 3: "greater"}
Abilities = {"STR": "Str", "AGI": "Agi", "STA": "Sta", "PER": "Per", "INT": "Int", "LCK": "Luck"}

Window = None
Shown = 0 # party slot
Text = None

def _Magic ():
	return GemRB.GetDCCMagic (Shown) if Shown else None

def _Button (win, id, x, y, w, h, text, action, chosen = False):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ChosenColor if chosen else ButtonColor)
	button.SetText (text)
	button.OnPress (action)
	return button

def _Call (function, *args):
	return lambda: function (*args)

def EffectWords (effects):
	"""a corruption's magic effect terms in words (its mechanical effect)"""
	words = []
	for term in effects.split (","):
		if not term or term == "*":
			continue
		gate = ""
		if "#" in term[:3]:
			gate, term = term.split ("#", 1)
		parts = term.split ("^")[0].split (":")
		key = parts[0]
		text = None
		if key == "ABIL" and len (parts) >= 3:
			sign = "+" if parts[2].startswith ("-") else "-"
			text = "%s%s %s%s" % (sign, parts[2].lstrip ("-"), Abilities.get (parts[1], parts[1]), " for good" if "PERM" in parts else "")
		elif key == "ROLLMOD" and len (parts) >= 2:
			text = "%s to all rolls%s" % (parts[1], " for good" if "PERM" in parts else "")
		elif key == "DMG":
			text = "%s damage" % parts[1]
		elif key == "UNCON":
			text = "knocked out"
		elif key in ("MINOR", "MAJOR", "GREATER"):
			text = "a %s corruption" % key.lower ()
		elif key == "TODO":
			text = parts[1].replace ("_", " ") + " (not in the engine yet)"
		if text:
			words.append (("(%s) " % gate if gate else "") + text)
	return ", ".join (words) if words else "cosmetic"

def Lines (pc):
	"""the record screen's magic lines (none for non-casters)"""
	magic = GemRB.GetDCCMagic (pc)
	if not magic or not (magic["CasterKind"] or magic["Corruptions"]):
		return []
	lines = []
	if magic["CasterKind"]:
		head = "Caster level %d, spells up to level %d" % (magic["CasterLevel"], magic["MaxSpell"])
		if magic["ArmorPenalty"]:
			head += ", armor %d to spell checks" % magic["ArmorPenalty"]
		if magic["SpellMod"]:
			head += ", %+d to spell checks for now" % magic["SpellMod"]
		lines.append (head)
		for spell in magic["Spells"]:
			text = "  %s (%d)%s" % (spell["Name"].replace ("_", " "), spell["Level"], " - lost for today" if spell["Lost"] else "")
			extra = []
			if spell["ManifestationText"]:
				extra.append (spell["ManifestationText"])
			for value, what in zip (spell["Mercurial"], spell["MercurialText"]):
				if not 41 <= value <= 60:
					extra.append ("mercurial %d: %s" % (value, what))
			if spell["Lucky"]:
				extra.append ("lucky spell")
			if extra:
				text += ": " + "; ".join (extra)
			lines.append (text)
	if magic["CasterKind"] == 1:
		burn = magic["Spellburn"]
		lines.append ("Spellburn: %s (preset %d); 1 Luck cancels %s" % (PolicyNames.get (burn["Policy"], "?"), burn["Preset"], CorruptionNames.get (burn["Corruption"], "?")))
		if sum (burn["Points"]):
			lines.append ("  still burned: %d Str, %d Agi, %d Sta (1 back a day without spellburn)" % tuple (burn["Points"]))
	elif magic["CasterKind"] == 2:
		lines.append ("Disapproval range 1-%d%s" % (magic["Disapproval"], " (won't reset tomorrow)" if magic["NoReset"] else ""))
	if magic["Corruptions"]:
		lines.append ("Corruption:")
		for corruption in magic["Corruptions"]:
			lines.append ("  %s (%s): %s" % (corruption["Text"], Tables.get (corruption["Table"], "?"), EffectWords (corruption["Effects"])))
	return lines

def Close ():
	global Window, Text
	if Window:
		Window.Close ()
	Window = Text = None

def OnGameStart ():
	global Window, Text
	Window = Text = None

def _Refresh ():
	OpenSpellsWindow (Shown)
	import GUIREC
	GUIREC.UpdateRecordsWindow ()

def Cast (spell):
	"""casts a known spell: the usual targeting; the spell check rolls when the casting ends"""
	pc = Shown
	Close ()
	import GUIREC
	GUIREC.CloseRecordsWindow ()
	GemRB.SpellCast (pc, -3, 0, spell)

def SetSpellburn (policy = -1, preset = -1, corruption = -1):
	GemRB.DCCSetSpellburn (Shown, policy, preset, corruption)
	_Refresh ()

def Atone ():
	GemRB.DCCAtone (Shown, 50)
	_Refresh ()

def MostWounded ():
	"""the living party member missing the most hp (lay on hands)"""
	best, missing = 0, 0
	for pc in range (1, GemRB.GetPartySize () + 1):
		stats = GemRB.GetDCCStats (pc)
		if not stats or stats["HP"] <= 0:
			continue
		lack = stats["MaxHP"] - stats["HP"]
		if lack > missing:
			best, missing = pc, lack
	return best

def LayOnHands ():
	target = MostWounded ()
	if target:
		GemRB.DCCLayOnHands (Shown, target)
	_Refresh ()

def TurnUnholy ():
	GemRB.DCCTurnUnholy (Shown)
	_Refresh ()

def OpenSpellsWindow (pc = None):
	global Window, Shown, Text
	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	magic = _Magic ()
	spells = magic["Spells"] if magic else []
	height = 150 + 34 * len (spells) + 90
	Close ()
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - height) // 3), 'w': Width, 'h': height}
	win = Window = GemRB.CreateView (98, 255, frame)
	win.AddAlias ("WIN_DCCMAGIC")
	win.SetBackground (WindowColor)
	name = GemRB.GetPlayerName (Shown, 1)
	kinds = {0: "casts no spells", 1: "arcane caster", 2: "cleric"}
	title = win.CreateLabel (1, 12, 8, Width - 24, 24, Font, "%s, %s" % (name, kinds.get (magic["CasterKind"] if magic else 0)), IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	y = 40
	for i, spell in enumerate (spells):
		text = "%s%s" % (spell["Name"].replace ("_", " "), " (lost)" if spell["Lost"] else "")
		button = _Button (win, 10 + i, 12, y, Width - 24, 28, text, _Call (Cast, spell["Spell"]))
		if spell["Lost"] or not magic["CasterKind"]:
			button.SetState (IE_GUI_BUTTON_DISABLED)
		y += 34
	y += 6
	if magic and magic["CasterKind"] == 1:
		burn = magic["Spellburn"]
		win.CreateLabel (2, 12, y, 110, 28, Font, "Spellburn", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		for i, (policy, label) in enumerate (SpellburnPolicies):
			_Button (win, 30 + i, 120 + 80 * i, y, 74, 28, label, _Call (SetSpellburn, policy), burn["Policy"] == policy)
		y += 34
		win.CreateLabel (3, 12, y, 110, 28, Font, "Preset %d" % burn["Preset"], IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		_Button (win, 33, 120, y, 34, 28, "-", _Call (SetSpellburn, -1, max (0, burn["Preset"] - 1)))
		_Button (win, 34, 160, y, 34, 28, "+", _Call (SetSpellburn, -1, burn["Preset"] + 1))
		y += 34
		win.CreateLabel (4, 12, y, 110, 28, Font, "Luck vs corruption", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		for i, (policy, label) in enumerate (CorruptionPolicies):
			_Button (win, 35 + i, 120 + 80 * i, y, 74, 28, label, _Call (SetSpellburn, -1, -1, policy), burn["Corruption"] == policy)
		y += 34
	elif magic and magic["CasterKind"] == 2:
		win.CreateLabel (2, 12, y, Width - 24, 28, Font, "Disapproval range 1-%d" % magic["Disapproval"], IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
		y += 34
		atone = _Button (win, 40, 12, y, 130, 28, "Atone (50 gp)", Atone)
		if magic["Disapproval"] <= 1:
			atone.SetState (IE_GUI_BUTTON_DISABLED)
		_Button (win, 41, 150, y, 140, 28, "Lay on hands", LayOnHands)
		_Button (win, 42, 298, y, 150, 28, "Turn unholy", TurnUnholy)
		y += 34
	close = _Button (win, 50, Width - 112, height - 38, 100, 28, "Close", Close)
	close.MakeEscape ()
	win.Focus ()
	return win
