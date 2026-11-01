# SPDX-FileCopyrightText: 2020 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the demo's Game.py (GUIScripts/demo/Game.py) with the party size of a DCC game (M3.3): the
# funnel's sixteen until it ends, then four (the global DCC_PARTY_SIZE, which GemRB.DCCEndFunnel sets
# and the save keeps), so a loaded game after the funnel doesn't take sixteen again. And no protagonist:
# the game is over when the whole party is dead (protagonist mode 2), not when party slot 1 dies (in a
# funnel peasant 1 is no hero, and after it the four are equals).

import GemRB

def EnterGame():
	# MaxPartySize now, not GameCheck's MAX_PARTY_SIZE: that one is read when GameCheck is first imported,
	# before the game type's gemrb.ini raised it to 16, and a party of 16 then overflowed into the stock
	# PartyReform window (which errors in this GUI) at every start
	size = GemRB.GetGameVar ("DCC_PARTY_SIZE") or GemRB.GetVar ("MaxPartySize") or 6
	GemRB.GameSetPartySize(size)
	print ("[Game] party size %d" % size)
	GemRB.GameSetProtagonistMode(2)

	import MessageWindow
	MessageWindow.OnLoad()
	GemRB.GamePause (0, 0)

def GameExpansion():
	pass
