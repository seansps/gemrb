# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the thief buttons of the bottom bar (MessageWindow.py) (DCC rules, docs/rules/traps.md and
# classes.md): Detect traps (T) and Stealth (S) for the selected characters, and the thieving
# cursor (P: disarm a found trap, pick a lock, pick a pocket). Everyone may use all three: under
# DCC anyone searches and disarms (d20 + Int, d20 + Agi), and the other skills fall back to the
# occupation rules. The engine rolls (on the co-op host); a client's toggle is an order to the host.
import GemRB
from GUIDefines import *
from ie_modal import *

# what this player last asked for per character: a co-op client doesn't see the host's modal states
Wanted = {}

def _Selected ():
	return [pc for pc in range (1, GemRB.GetPartySize () + 1) if GemRB.GameIsPCSelected (pc)]

def _Mode (pc):
	if GemRB.NetGetMode () == "client":
		return Wanted.get (pc, MS_NONE)
	return GemRB.GetDCCStealth (pc)["Mode"]

def Toggle (mode):
	"""turns the mode on for every selected character, or off if all of them have it"""
	party = _Selected ()
	if not party:
		return
	on = any (_Mode (pc) != mode for pc in party)
	for pc in party:
		state = mode if on else MS_NONE
		if _Mode (pc) == state:
			continue
		# MS_NONE leaves the mode; the engine toggles a selected character's same mode off, so
		# a mode is only sent to those not in it
		GemRB.SetModalState (pc, state)
		Wanted[pc] = state

def DetectTraps ():
	Toggle (MS_DETECTTRAPS)

def Stealth ():
	Toggle (MS_STEALTH)

def Thieving ():
	GemRB.GameControlSetTargetMode (TARGET_MODE_PICK, GA_NO_DEAD | GA_NO_SELF | GA_NO_ENEMY | GA_NO_HIDDEN)

def Specs ():
	"""the three buttons: (label, tooltip, action); the label's letter is the hot key"""
	return (("T", "Detect traps (T): search at half speed; rolls Int for hidden traps nearby", DetectTraps),
		("S", "Stealth (S): hide in shadows and sneak silently", Stealth),
		("P", "Thieving (P): disarm a found trap, pick a lock or a pocket", Thieving))

def SetHotKeys (*buttons):
	"""the bar's T, S and P buttons (MessageWindow.py): each its letter, a global hot key"""
	for button, (text, tip, action) in zip (buttons, Specs ()):
		button.SetHotKey (text.lower (), 0, True)
