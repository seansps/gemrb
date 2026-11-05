# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The in-game UI checks: the party column and the slim bar (docs/architecture/ui-design.md, "The party column
# and the slim bar"), the keys, the inventory; all by real mouse clicks, drags, wheel turns and key presses
# through the event manager (GemRB.TestClick, TestDrag, TestWheel, TestKey), as a player's would go. Run from the
# funneltest autotest (FunnelAutotest.py): Funnel16 in its "ui" scenario (the sixteen), Party4 in "ui4" (the four
# after the funnel's end); the checks go through the caller's _Check and _Log. Screenshots ui_*.bmp in the save path.
#   Funnel16: the layout (the column on the right edge, the bar along the bottom left of it, neither taking the
#     keyboard); the sixteen's one-line tiles all fit, and a shorter column scrolls by the wheel, its tiles answering
#     clicks while scrolled; the keys (I, C, W, R,
#     space; I and C again from the open inventory and record); the inventory by drags and clicks: an item moved
#     between pack slots (dragged, then picked up and put down), the weapon unequipped into the pack and equipped
#     again, an item dropped on the ground and picked up again, one given to another peasant by a click on its tile
#     and given back; the inventory switched to another peasant by a tile click.
#   Party4: portrait tiles for the four, no scroll bar, a click selects; the inventory and the record over them.

import time
import GemRB
from GUIDefines import *
from ie_stats import *
from ie_slots import *

Check = None # the caller's _Check (what, got, expected)
Log = None
WEAPON_SLOT = 10 # the first quick weapon slot (slottype.2da), GUIINV's control 1
GROUND_CONTROL = 68 # GUIINV's first ground slot

def _Centre (win, control):
	outer, frame = win.GetFrame (), control.GetFrame ()
	return outer['x'] + frame['x'] + frame['w'] // 2, outer['y'] + frame['y'] + frame['h'] // 2

def _Apart ():
	"""a moment between two presses, so the second is no double click (a player's are further apart)"""
	time.sleep (0.3)

def Click (win, control, button = 1, mod = 0):
	x, y = _Centre (win, control)
	_Apart ()
	GemRB.TestClick (x, y, button, mod)

def Drag (win1, control1, win2, control2):
	"""a drag from one control to another: pressed and moved, a frame for the drag to start (a slot picks its item
	up on a timer), released"""
	x1, y1 = _Centre (win1, control1)
	x2, y2 = _Centre (win2, control2)
	_Apart ()
	GemRB.TestDrag (x1, y1, x2, y2, 1, 0, 1)
	yield
	GemRB.TestDrag (x1, y1, x2, y2, 1, 0, 2)

def Shot (name):
	path = GemRB.GetSystemVariable (SV_SAVEPATH) + "/ui_%s.bmp" % name
	Check ("ui: screenshot %s" % name, GemRB.SaveScreenshot (path, 0, 1), True)
	Log ("screenshot: " + path)

def _Frames ():
	return GemRB.GetView ("ROSTWIN").GetFrame (), GemRB.GetView ("MSGWIN").GetFrame ()

def _HasKeyboard (name):
	view = GemRB.GetView (name)
	return bool (view and view.HasFocus)

def _Ticks (n):
	for i in range (n):
		yield

def _Inv ():
	return GemRB.GetView ("WIN_INV")

def _SlotControl (pc, slot):
	return _Inv ().GetControl (GemRB.GetSlotType (slot, pc)["ID"])

def _Item (pc, slot):
	item = GemRB.GetSlotItem (pc, slot)
	return item["ItemResRef"].upper () if item else None

def _Pack (pc):
	"""the pack (inventory slots), slot -> item"""
	return dict ((slot, _Item (pc, slot)) for slot in GemRB.GetSlots (pc, SLOT_INVENTORY, 1))

def _Ground (pc):
	pile = GemRB.GetContainer (pc, 1)
	return [GemRB.GetContainerItem (pc, i)["ItemResRef"].upper () for i in range (pile["ItemCount"] if pile else 0)]

def _Empty (pc):
	return list (GemRB.GetSlots (pc, SLOT_INVENTORY, -1))

def _Alive (pc):
	return not (GemRB.GetPlayerStat (pc, IE_STATE_ID) & 0x800) and GemRB.GetPlayerStat (pc, IE_HITPOINTS) > 0

def _Distance (a, b):
	pa, pb = GemRB.GetActorPosition (a), GemRB.GetActorPosition (b)
	return abs (pa["X"] - pb["X"]) + abs (pa["Y"] - pb["Y"])

def _Unpause ():
	if GemRB.GamePause (3, 1):
		GemRB.GamePause (0, 1)

# --- the sixteen ---------------------------------------------------------------------------------------------------

def Funnel16 ():
	import Roster
	import MessageWindow
	_Unpause ()
	size = GemRB.GetPartySize ()
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	Roster.UpdateRoster ()
	column, bar = _Frames ()
	Check ("ui: the party column down the right edge, full height", [column['x'] + column['w'], column['w'], column['y'], column['h']], [screen[0], Roster.COLUMN_W, 0, screen[1]])
	Check ("ui: the bar along the bottom, left of the column, slim", [bar['x'], bar['y'] + bar['h'], bar['w'], bar['h']], [0, screen[1], screen[0] - Roster.COLUMN_W, MessageWindow.BAR_H])
	labels = MessageWindow.Order
	Check ("ui: the bar's buttons (no Centre, no Journal yet, Mine only in co-op)", labels, ["Map", "Inv", "Rec", "Spells", "Rest", "Pause", "Set", "All", "Block", "Col", "T", "S", "P"])
	frames = [MessageWindow.Buttons[label].GetFrame () for label in labels]
	rows = sorted (set (f['y'] for f in frames))
	edges = [[(f['x'], f['x'] + f['w']) for f in frames if f['y'] == y] for y in rows]
	flush = [all (a[1] + MessageWindow.GAP == b[0] for a, b in zip (row, row[1:])) for row in edges]
	Check ("ui: two rows, no gaps, both from the same left edge to the same right edge", [len (rows), flush, edges[0][0][0] == edges[1][0][0], edges[0][-1][1] == edges[1][-1][1]], [2, [True, True], True, True])
	Check ("ui: the game view has the keyboard (not the column or the bar)", [_HasKeyboard ("GAMEWIN"), _HasKeyboard ("ROSTWIN"), _HasKeyboard ("MSGWIN")], [True, False, False])
	Check ("ui: one-line tiles, all sixteen fit: no scroll bar", [Roster.Layout["Hero"], Roster.Layout["TileH"], Roster.Layout["Scrolls"], Roster.Layout["Rows"], Roster.TileCount ()], [False, Roster.TILE_H, False, size, size])
	Check ("ui: a tile's one line is the name (and the status); the occupation is in the hover line", [Roster.Shown[1].split ("  ")[0], "\n" in Roster.Shown[1], ": " in Roster.Tiles[1]["Tooltip"]], [GemRB.GetPlayerName (1, 0), False, True])
	GemRB.GameSelectPC (0, 1)
	yield
	Click (Roster.RosterWindow, Roster.RosterWindow.GetControl (size - 1))
	yield
	Check ("ui: a click on the last tile selects the last peasant", Roster.Selected (), [size])
	GemRB.GameSelectPC (0, 1)
	yield
	Shot ("funnel16")
	yield from _Skin ()
	# a shorter column (a smaller screen): the list scrolls
	Roster.MaxListHeight = 10 * Roster.Layout["Pitch"]
	Roster.Rebuild ()
	yield
	Check ("ui: a column too short for sixteen scrolls", [Roster.Layout["Scrolls"], Roster.Layout["Rows"], Roster.TileCount ()], [True, 10, size])
	# the wheel over the column scrolls it (the tiles pass it to the window, the window to its scroll bar)
	tile = Roster.RosterWindow.GetControl (2)
	x, y = _Centre (Roster.RosterWindow, tile)
	for i in range (size):
		GemRB.TestWheel (x, y, -1)
	yield
	rows = Roster.Layout["Rows"]
	Check ("ui: the wheel scrolls the list to its end", [Roster.Top, Roster.OnShow ()[-1]], [size - rows, size])
	last = Roster.RosterWindow.GetControl (rows - 1)
	Click (Roster.RosterWindow, last)
	yield
	Check ("ui: a click on the bottom tile, scrolled, selects the last peasant", Roster.Selected (), [size])
	Check ("ui: its tile shows that one", Roster.Shown[size].split ("\n")[0].split ("  ")[0], GemRB.GetPlayerName (size, 0))
	Shot ("funnel16_scrolled")
	for i in range (size):
		GemRB.TestWheel (x, y, 1)
	yield
	Check ("ui: and back to the top", Roster.Top, 0)
	Roster.MaxListHeight = None
	Roster.Rebuild ()
	yield
	Check ("ui: the full column again, no scroll bar", [Roster.Layout["Scrolls"], Roster.Layout["Rows"]], [False, size])
	# a click between the tiles stays in the column (the backdrop), it doesn't walk the party
	GemRB.GameSelectPC (1, 1, SELECT_REPLACE)
	before = GemRB.GetActorPosition (1)
	GemRB.TestClick (column['x'] + 2, column['y'] + screen[1] - 2)
	for i in range (4):
		yield
	after = GemRB.GetActorPosition (1)
	Check ("ui: a click on the column's backdrop doesn't move the party", [after["X"], after["Y"], after["Moving"]], [before["X"], before["Y"], 0])

	yield from _Keys ()
	yield from _Inventory ()

def _Skin ():
	"""the skin (Skin.py), when the game has it: a bar button under the mouse shows its hover frame and leaves it again,
	the formation in use is lit"""
	import Skin
	import MessageWindow
	import Roster
	if not Skin.On ():
		Log ("ui: no skin in this game, its checks skipped")
		return
	bar = MessageWindow.Window
	x, y = _Centre (bar, MessageWindow.Buttons["Inv"])
	GemRB.TestClick (x, y, 0) # only moves the mouse
	yield from _Ticks (2)
	lit = Skin.LIT if Roster.CurrentFormation () == 0 else Skin.NORMAL
	Check ("ui: the skin: the button under the mouse hovers, the others don't, the formation in use is lit", [Skin.Frame ("Inv"), Skin.Frame ("Map"), Skin.Frame ("Block")], [Skin.HOVER, Skin.NORMAL, lit])
	Shot ("bar_hover")
	GemRB.TestClick (300, 300, 0) # over the game view
	yield from _Ticks (2)
	Check ("ui: the skin: and the hover goes when the mouse leaves", Skin.Frame ("Inv"), Skin.NORMAL)

def _Keys ():
	"""the keymap's keys reach the game view: I, C, W, R, space; and the open windows' own I and C"""
	import GUIREC
	import DCCMagic
	# someone living, not the first (the record opens on the selected, not on party slot 1)
	who = [pc for pc in range (2, GemRB.GetPartySize () + 1) if _Alive (pc)][0]
	GemRB.GameSelectPC (who, 1, SELECT_REPLACE)
	yield
	GemRB.TestKey ("i")
	yield from _Ticks (2)
	Check ("ui: I opens the inventory", bool (_Inv ()), True)
	GemRB.TestKey ("i")
	yield from _Ticks (2)
	Check ("ui: I again closes it", bool (_Inv ()), False)
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	Check ("ui: C opens the record, on the selected", [bool (GUIREC.Window), GUIREC.Shown], [True, who])
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	Check ("ui: C again closes it", GUIREC.Window, None)
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	GemRB.TestKey ("i")
	yield from _Ticks (2)
	Check ("ui: I with the record open swaps it for the inventory", [GUIREC.Window is None, bool (_Inv ())], [True, True])
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	Check ("ui: C with the inventory open swaps it for the record", [bool (GUIREC.Window), bool (_Inv ())], [True, False])
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	GemRB.TestKey ("w")
	yield from _Ticks (2)
	Check ("ui: W opens the spells", bool (DCCMagic.Window), True)
	GemRB.TestKey ("w")
	yield from _Ticks (2)
	Check ("ui: W again closes them", DCCMagic.Window, None)
	run = GemRB.GetVar ("Always Run")
	GemRB.TestKey ("r")
	yield
	Check ("ui: R toggles Always Run", bool (GemRB.GetVar ("Always Run")), not run)
	GemRB.TestKey ("r")
	yield
	paused = bool (GemRB.GamePause (3, 1))
	GemRB.TestKey (" ")
	yield
	Check ("ui: space pauses", bool (GemRB.GamePause (3, 1)), not paused)
	GemRB.TestKey (" ")
	yield
	_Unpause ()
	Check ("ui: the game view has the keyboard again", _HasKeyboard ("GAMEWIN"), True)

def _Inventory ():
	"""the inventory by drags and clicks"""
	import Roster
	import MessageWindow
	party = [pc for pc in range (1, GemRB.GetPartySize () + 1) if _Alive (pc)]
	armed = [pc for pc in party if _Item (pc, WEAPON_SLOT) and _Item (pc, WEAPON_SLOT) != "FIST" and GemRB.GetSlots (pc, SLOT_INVENTORY, 1)]
	Check ("ui: a peasant with a weapon in hand and something in the pack", bool (armed), True)
	if not armed:
		return
	pc = armed[0]
	others = sorted ((_Distance (pc, other), other) for other in party if other != pc)
	friend = others[0][1]
	GemRB.GameSelectPC (pc, 1, SELECT_REPLACE)
	yield
	Click (MessageWindow.Window, MessageWindow.Buttons["Inv"])
	yield from _Ticks (2)
	Check ("ui: the bar's Inv button opens the inventory", bool (_Inv ()), True)
	if not _Inv ():
		return
	inv = _Inv ()
	Check ("ui: the inventory's feedback line is there (MsgSys 1)", bool (GemRB.GetView ("MsgSys", 1)), True)
	pack = _Pack (pc)
	Log ("ui: %s's pack %s, in hand %s; the nearest, %s" % (GemRB.GetPlayerName (pc, 0), pack, _Item (pc, WEAPON_SLOT), GemRB.GetPlayerName (friend, 0)))

	# a pack item dragged to an empty pack slot
	slot = min (pack)
	item = pack[slot]
	empty = _Empty (pc)[0]
	yield from Drag (inv, _SlotControl (pc, slot), inv, _SlotControl (pc, empty))
	yield from _Ticks (2)
	Check ("ui: an item dragged to another pack slot moves there", [_Item (pc, slot), _Item (pc, empty), GemRB.IsDraggingItem ()], [None, item, 0])
	# picked up with a click, put down with another
	there = empty
	empty = _Empty (pc)[-1]
	Click (inv, _SlotControl (pc, there))
	yield
	Check ("ui: a click on it picks it up", GemRB.IsDraggingItem (), 1)
	Click (inv, _SlotControl (pc, empty))
	yield from _Ticks (2)
	Check ("ui: a click on an empty slot puts it down there", [_Item (pc, there), _Item (pc, empty), GemRB.IsDraggingItem ()], [None, item, 0])
	yield from Drag (inv, _SlotControl (pc, empty), inv, _SlotControl (pc, slot))
	yield from _Ticks (2)
	Check ("ui: and back where it was", _Item (pc, slot), item)

	# the weapon out of the hand into the pack, and back
	weapon = _Item (pc, WEAPON_SLOT)
	empty = _Empty (pc)[0]
	yield from Drag (inv, inv.GetControl (1), inv, _SlotControl (pc, empty))
	yield from _Ticks (2)
	Check ("ui: the weapon dragged into the pack (unequipped)", [_Item (pc, WEAPON_SLOT) in (None, "FIST"), _Item (pc, empty)], [True, weapon])
	yield from Drag (inv, _SlotControl (pc, empty), inv, inv.GetControl (1))
	yield from _Ticks (2)
	Check ("ui: and dragged back into the hand (equipped)", [_Item (pc, WEAPON_SLOT), _Item (pc, empty), GemRB.GetEquippedQuickSlot (pc)], [weapon, None, WEAPON_SLOT])

	# onto the ground and back
	ground = _Ground (pc)
	yield from Drag (inv, _SlotControl (pc, slot), inv, inv.GetControl (GROUND_CONTROL))
	yield from _Ticks (2)
	Check ("ui: an item dragged onto the ground lies there", [_Item (pc, slot), _Ground (pc).count (item) - ground.count (item)], [None, 1])
	if not _Ground (pc):
		return
	# the ground slots show the pile from the start: the new one is the last
	index = len (_Ground (pc)) - 1
	GemRB.SetVar ("TopIndex", index // 2)
	import GUIINV
	GUIINV.RefreshInventoryWindow ()
	yield
	ids = sorted (GUIINV.HorizontalSlots, key = lambda cid: GUIINV.HorizontalSlots[cid])
	shown = [cid for cid in ids if inv.GetControl (cid).Value == index]
	Check ("ui: the ground slot shows it", bool (shown), True)
	if shown:
		yield from Drag (inv, inv.GetControl (shown[0]), inv, _SlotControl (pc, slot))
		yield from _Ticks (2)
	Check ("ui: and dragged back into the pack", [_Item (pc, slot), _Ground (pc).count (item) - ground.count (item)], [item, 0])
	GemRB.SetVar ("TopIndex", 0)

	# to the nearest peasant: picked up, then a click on its tile in the party column
	before = list (_Pack (friend).values ())
	Click (inv, _SlotControl (pc, slot))
	yield
	tile = Roster.TileControl (friend)
	yield
	Click (Roster.RosterWindow, tile)
	yield from _Ticks (2)
	Check ("ui: an item clicked onto another peasant's tile goes into its pack", [_Item (pc, slot), list (_Pack (friend).values ()).count (item) - before.count (item), GemRB.IsDraggingItem ()], [None, 1, 0])
	Shot ("funnel16_inventory")
	# a click on its tile (nothing in hand) shows its inventory
	Click (Roster.RosterWindow, Roster.TileControl (friend))
	yield from _Ticks (2)
	Check ("ui: a click on a tile shows that peasant's inventory", [bool (_Inv ()), GemRB.GameGetSelectedPCSingle ()], [True, friend])
	given = [s for s, ref in _Pack (friend).items () if ref == item]
	if given:
		Click (inv, _SlotControl (friend, given[-1]))
		yield
		Click (Roster.RosterWindow, Roster.TileControl (pc))
		yield from _Ticks (2)
	Check ("ui: and given back", [list (_Pack (friend).values ()).count (item), item in _Pack (pc).values ()], [before.count (item), True])
	GemRB.TestKey ("i")
	yield from _Ticks (2)
	Check ("ui: I closes the inventory", bool (_Inv ()), False)
	Roster.ScrollTo (0)
	# the record over the column
	GemRB.GameSelectPC (pc, 1, SELECT_REPLACE)
	GemRB.TestKey ("c")
	yield from _Ticks (2)
	Shot ("funnel16_record")
	GemRB.TestKey ("c")
	yield

# --- the four, after the funnel ------------------------------------------------------------------------------------

def Party4 ():
	import Roster
	import GUIREC
	_Unpause ()
	Roster.UpdateRoster ()
	size = GemRB.GetPartySize ()
	Check ("ui: portrait tiles for the party after the funnel, no scroll bar", [Roster.Layout["Hero"], Roster.Layout["Scrolls"], Roster.TileCount ()], [True, False, size])
	yield from _Ticks (2)
	Check ("ui: each portrait the character's own figure", [Roster.Portraits.get (pc) for pc in range (1, size + 1)], ["figure"] * size)
	Click (Roster.RosterWindow, Roster.RosterWindow.GetControl (Roster.PORTRAIT_ID + 1))
	yield
	Check ("ui: a click on the second portrait selects it", Roster.Selected (), [2])
	text = Roster.Shown[2].split ("\n")
	Check ("ui: the tile's lines: name, class and level, HP, Luck", [len (text) >= 4, text[2].startswith ("HP "), text[3].startswith ("Luck ")], [True, True, True])
	GemRB.GameSelectPC (0, 1)
	yield
	Shot ("party4")
	GemRB.GameSelectPC (1, 1, SELECT_REPLACE)
	GemRB.TestKey ("i")
	yield from _Ticks (3)
	Check ("ui: I opens the inventory after the funnel", bool (_Inv ()), True)
	Shot ("party4_inventory")
	GemRB.TestKey ("c")
	yield from _Ticks (3)
	Check ("ui: C swaps it for the record", [bool (GUIREC.Window), bool (_Inv ())], [True, False])
	Shot ("party4_record")
	GemRB.TestKey ("c")
	yield
	GemRB.GameSelectPC (0, 1)
