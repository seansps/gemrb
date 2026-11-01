# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The ethren Luck and Deeds panels (M2.4), opened from the record screen (GUIREC):
# - Luck: when the character burns Luck on its own (never, only to avoid death or a
#   fumble, or also to turn a failed roll into a success while Luck stays at N or more),
#   whether the game pauses and asks on key rolls, and for halflings whether they burn
#   for allies in sight.
# - Deeds (warriors and dwarves): the signature Mighty Deed declared for every attack,
#   and a one-off deed for the next attack only (it can be queued while paused).
# - Combat (M2.3b): a charge or a grapple for the next melee attack, subdual blows,
#   and how much Personality or Intelligence a battle rage crit spends.
# Plain controls in windows built from code, like GUIREC. The changes go through
# GemRB.DCCSetLuckPolicy, DCCSetDeeds and DCCSetCombat, which a co-op client also sends
# to the host (only the character's owner may change them).
import GemRB
import DCCSheet
from GUIDefines import *
from ie_stats import *

Font = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 245}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
ChosenColor = {'r': 150, 'g': 110, 'b': 40, 'a': 255}
Width = 440

Policies = (
	(DCC_BURN_NEVER, "Never"),
	(DCC_BURN_AVOID_DISASTER, "Only to avoid death or a fumble"),
	(DCC_BURN_MISS_TO_HIT, "Also to turn a miss into a hit"),
)
PolicyNames = {DCC_BURN_NEVER: "never", DCC_BURN_AVOID_DISASTER: "only to avoid death or a fumble", DCC_BURN_MISS_TO_HIT: "to turn misses into hits"}

Deeds = (
	(DCC_DEED_BLIND, "Blind", "dirt, a pommel or a feint to the eyes: penalties, then blindness"),
	(DCC_DEED_DISARM, "Disarm", "the foe drops its weapon; higher results sunder it"),
	(DCC_DEED_PUSH, "Push", "shoves the foe back, further the higher the deed die"),
	(DCC_DEED_TRIP, "Trip", "knocks the foe prone (a Reflex save against your attack on a 3)"),
	(DCC_DEED_PRECISION, "Precision", "a called shot: +1d4 damage on a 3, up to +1d8"),
	(DCC_DEED_RALLY, "Rally", "nearby allies take heart; on 5+ they also hit better"),
	(DCC_DEED_DEFEND, "Defend", "you (and up to 4 allies near you) get +1 to +3 AC for a round"),
)
DeedNames = dict ((deed, name.lower ()) for deed, name, text in Deeds)

Window = None
Kind = None # "luck", "deeds" or "combat"
Shown = 0 # party slot

def _Stats ():
	return GemRB.GetDCCStats (Shown) if Shown else None

def _Button (win, id, x, y, w, h, text, action, chosen = False):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetBackground (ChosenColor if chosen else ButtonColor)
	button.SetText (text)
	# the engine passes the button to a handler that takes arguments, which would fill a lambda's
	# defaults (lambda i = i: ...): the action is called without any
	button.OnPress (lambda: action ())
	return button

def _Label (win, id, x, y, w, h, text):
	return win.CreateLabel (id, x, y, w, h, Font, text, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)

def _Call (function, *args):
	return lambda: function (*args)

def _Open (height):
	global Window
	Close ()
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': (screen[0] - Width) // 2, 'y': max (0, (screen[1] - height) // 3), 'w': Width, 'h': height}
	Window = GemRB.CreateView (97, 255, frame)
	Window.AddAlias ("WIN_DCCLUCK")
	Window.SetBackground (WindowColor)
	return Window

def Close ():
	global Window, Kind
	if Window:
		Window.Close ()
	Window = Kind = None

def OnGameStart ():
	global Window, Kind
	Window = Kind = None

def _Refresh ():
	"""rebuilds the open panel with the current settings, and the record screen under it"""
	if Kind == "luck":
		OpenLuckWindow (Shown)
	elif Kind == "deeds":
		OpenDeedsWindow (Shown)
	elif Kind == "combat":
		OpenCombatWindow (Shown)
	import GUIREC
	GUIREC.UpdateRecordsWindow ()

###################################################
# Luck

def _SetPolicy (policy = None, threshold = None, flags = None):
	stats = _Stats ()
	if not stats:
		return
	policy = stats["LuckPolicy"] if policy is None else policy
	threshold = stats["LuckThreshold"] if threshold is None else max (0, min (threshold, 30))
	flags = stats["LuckFlags"] if flags is None else flags
	GemRB.DCCSetLuckPolicy (Shown, policy, threshold, flags)
	_Refresh ()

def BurnText (stats):
	"""what a point of Luck buys this character"""
	if stats["BurnDie"]:
		return "a d%d each (thief's luck die)" % stats["BurnDie"]
	return "+%d each" % stats["BurnPerPoint"]

def OpenLuckWindow (pc = None):
	global Kind, Shown
	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	stats = _Stats ()
	if not stats:
		Close ()
		return
	halfling = stats["Class"] == 7 and stats["Level"] > 0
	height = 330 + (34 if halfling else 0)
	win = _Open (height)
	Kind = "luck"
	name = GemRB.GetPlayerName (Shown, 0)
	_Label (win, 0x10000000, 12, 6, Width - 24, 22, "Luck: %s" % name)
	info = "Luck %d of %d. Burning adds %s." % (stats["Luck"], stats["LuckMax"], BurnText (stats))
	if stats["LuckRecovery"]:
		info += " Gets %d back each night." % stats["LuckRecovery"]
	else:
		info += " Burned Luck doesn't come back."
	win.CreateLabel (0x10000001, 12, 28, Width - 24, 44, Font, info, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)

	y = 76
	_Label (win, 0x10000002, 12, y, Width - 24, 20, "Burn Luck on my own:")
	y += 22
	for i, (policy, text) in enumerate (Policies):
		_Button (win, 10 + i, 20, y, Width - 40, 26, text, _Call (_SetPolicy, policy), stats["LuckPolicy"] == policy)
		y += 30
	if stats["CancelsFumbles"]:
		_Label (win, 0x10000003, 20, y, Width - 40, 20, "(a fumble costs 1 Luck to shrug off)")
	y += 24
	_Label (win, 0x10000004, 20, y, 240, 26, "Keep at least this much Luck:")
	_Button (win, 20, 270, y, 30, 26, "-", _Call (_SetPolicy, None, stats["LuckThreshold"] - 1))
	win.CreateLabel (0x10000005, 304, y, 40, 26, Font, str (stats["LuckThreshold"]), IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	_Button (win, 21, 348, y, 30, 26, "+", _Call (_SetPolicy, None, stats["LuckThreshold"] + 1))
	y += 34
	flags = stats["LuckFlags"]
	pause = flags & DCC_LUCK_PAUSE
	_Button (win, 30, 20, y, Width - 40, 26, "Pause and ask on key rolls: %s" % ("on" if pause else "off"),
		_Call (_SetPolicy, None, None, flags ^ DCC_LUCK_PAUSE), pause)
	y += 34
	if halfling:
		allies = flags & DCC_LUCK_ALLIES
		_Button (win, 31, 20, y, Width - 40, 26, "Burn for allies in sight (x2): %s" % ("on" if allies else "off"),
			_Call (_SetPolicy, None, None, flags ^ DCC_LUCK_ALLIES), allies)
		y += 34
	close = _Button (win, 40, Width - 112, height - 36, 100, 28, "Close", Close)
	close.MakeEscape ()
	win.Focus ()

###################################################
# Mighty Deeds

def _SetDeeds (signature = -1, nextDeed = -1):
	if not _Stats ():
		return
	GemRB.DCCSetDeeds (Shown, signature, nextDeed)
	_Refresh ()

def OpenDeedsWindow (pc = None):
	global Kind, Shown
	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	stats = _Stats ()
	if not stats or not stats["DeedDie"]:
		Close () # only warriors and dwarves have deeds
		return
	height = 420
	win = _Open (height)
	Kind = "deeds"
	name = GemRB.GetPlayerName (Shown, 0)
	_Label (win, 0x10000000, 12, 6, Width - 24, 22, "Mighty Deeds: %s (deed die d%d)" % (name, stats["DeedDie"]))
	win.CreateLabel (0x10000001, 12, 28, Width - 24, 36, Font, "A deed happens on a hit with the deed die at 3 or more; the higher, the stronger.",
		IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	y = 68
	_Label (win, 0x10000002, 12, y, Width - 24, 20, "Signature deed (every attack):")
	y += 22
	half = (Width - 50) // 2
	for i, (deed, label, text) in enumerate (Deeds):
		x = 20 + (i % 2) * (half + 10)
		_Button (win, 10 + i, x, y + (i // 2) * 30, half, 26, label, _Call (_SetDeeds, deed), stats["Deed"] == deed)
	y += 4 * 30 + 4
	queued = stats["NextDeed"]
	_Label (win, 0x10000003, 12, y, Width - 24, 20, "Next attack only: %s" % (DeedNames.get (queued, "-")))
	y += 22
	third = (Width - 60) // 4
	for i, (deed, label, text) in enumerate (Deeds):
		x = 20 + (i % 4) * (third + 6)
		_Button (win, 20 + i, x, y + (i // 4) * 30, third, 26, label, _Call (_SetDeeds, -1, deed), queued == deed)
	_Button (win, 27, 20 + 3 * (third + 6), y + 30, third, 26, "Clear", _Call (_SetDeeds, -1, DCC_DEED_NONE))
	y += 64
	chosen = queued or stats["Deed"]
	for deed, label, text in Deeds:
		if deed == chosen:
			win.CreateLabel (0x10000004, 12, y, Width - 24, 40, Font, "%s: %s" % (label, text), IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	close = _Button (win, 40, Width - 112, height - 36, 100, 28, "Close", Close)
	close.MakeEscape ()
	win.Focus ()

###################################################
# Combat options (M2.3b)

def _SetCombat (options = None, rage = -1):
	stats = _Stats ()
	if not stats:
		return
	options = stats["CombatOptions"] if options is None else options
	GemRB.DCCSetCombat (Shown, options, rage)
	_Refresh ()

def OpenCombatWindow (pc = None):
	global Kind, Shown
	Shown = pc or GemRB.GameGetSelectedPCSingle () or 1
	stats = _Stats ()
	if not stats or not stats["Character"]:
		Close ()
		return
	height = 300
	win = _Open (height)
	Kind = "combat"
	name = GemRB.GetPlayerName (Shown, 0)
	options = stats["CombatOptions"]
	_Label (win, 0x10000000, 12, 6, Width - 24, 22, "Combat: %s" % name)
	y = 34
	_Label (win, 0x10000001, 12, y, Width - 24, 20, "Next melee attack:")
	y += 24
	charge = options & DCC_OPT_CHARGE
	_Button (win, 10, 20, y, Width - 40, 26, "Charge (+2 to hit, -2 AC; run half your speed first): %s" % ("on" if charge else "off"),
		_Call (_SetCombat, options ^ DCC_OPT_CHARGE), charge)
	y += 30
	grapple = options & DCC_OPT_GRAPPLE
	_Button (win, 11, 20, y, Width - 40, 26, "Grapple and pin instead of striking: %s" % ("on" if grapple else "off"),
		_Call (_SetCombat, options ^ DCC_OPT_GRAPPLE), grapple)
	y += 38
	subdual = options & DCC_OPT_SUBDUAL
	_Button (win, 12, 20, y, Width - 40, 26, "Subdual blows (flat of the blade, a die lower): %s" % ("on" if subdual else "off"),
		_Call (_SetCombat, options ^ DCC_OPT_SUBDUAL), subdual)
	y += 38
	_Label (win, 0x10000002, 20, y, 260, 26, "Battle rage: Per/Int to spend:")
	_Button (win, 20, 290, y, 30, 26, "-", _Call (_SetCombat, None, max (0, stats["Rage"] - 1)))
	win.CreateLabel (0x10000003, 324, y, 40, 26, Font, str (stats["Rage"]), IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	_Button (win, 21, 368, y, 30, 26, "+", _Call (_SetCombat, None, min (10, stats["Rage"] + 1)))
	y += 30
	win.CreateLabel (0x10000004, 20, y, Width - 40, 40, Font, "A battle rage crit adds 1d12 per point (they come back with rest).",
		IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	close = _Button (win, 40, Width - 112, height - 36, 100, 28, "Close", Close)
	close.MakeEscape ()
	win.Focus ()

###################################################
# the record screen's lines

def Lines (stats):
	lines = []
	if not stats or not stats["Character"]:
		return lines
	policy = PolicyNames.get (stats["LuckPolicy"], "?")
	if stats["LuckPolicy"] == DCC_BURN_MISS_TO_HIT:
		policy += " while Luck stays at %d+" % stats["LuckThreshold"]
	extras = []
	if stats["LuckFlags"] & DCC_LUCK_PAUSE:
		extras.append ("asks on key rolls")
	lines.append ("Luck burn: %s%s; %s" % (policy, ", " + ", ".join (extras) if extras else "", BurnText (stats)))
	if stats["DeedDie"]:
		deed = "Deeds: signature %s" % DeedNames.get (stats["Deed"], "none")
		if stats["NextDeed"]:
			deed += ", next attack %s" % DeedNames.get (stats["NextDeed"], "?")
		lines.append (deed)
	options = stats["CombatOptions"]
	combat = []
	if options & DCC_OPT_CHARGE:
		combat.append ("charge next")
	if options & DCC_OPT_GRAPPLE:
		combat.append ("grapple next")
	if options & DCC_OPT_SUBDUAL:
		combat.append ("subdual blows")
	if stats["Rage"]:
		combat.append ("battle rage %d" % stats["Rage"])
	if not stats["Trained"]:
		combat.append ("untrained weapon (-1d)")
	if combat:
		lines.append ("Combat: " + ", ".join (combat))
	if stats["Injuries"]:
		lines.append ("Injuries: " + "; ".join (stats["Injuries"]))
	return lines
