# SPDX-FileCopyrightText: 2020 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later


# GUIINV.py - scripts to control inventory windows from GUIINV winpack
# ethren: the demo's (GUIScripts/demo/GUIINV.py) with
#   - the info label kept as the inventory's feedback line, the alias "MsgSys" 1: the shared item code
#     (InventoryCommon's OnDragItem, OnDragItemGround, OnDropItemToPC) clears it before every pick-up and
#     drop, and the demo removed it, so every item click and drag raised instead of moving anything;
#   - I and C as the window's own keys (it has the keyboard while open): I closes it, C swaps it for the record;
#   - the window centred left of the party column and above the bar, both still in sight;
#   - the ground slots take a drag from the pack (they share its slots' variable), and their items drag into it;
#   - a refresh after an item is given to another party member from the party column (Roster.py).

###################################################

import GemRB
import GUICommon
import GUICommonWindows
import InventoryCommon
import PaperDoll
from GUIDefines import *
from ie_stats import *
from ie_slots import *
from ie_spells import *
from ie_restype import RES_BAM

InventoryWindow = None
PaperDoll.StanceAnim = "WK"

# ground slots are transposed with one outlier, so map them to something normal
# 68 71 -> 68 69
# 69 72 -> 70 71
# 70 81 -> 72 81
# the original seems to have stored the order in the Cycle field (0-5)
HorizontalSlots = { 68: 68,  69: 70, 70: 72, 71: 69, 72: 71, 81: 81 }

def InitInventoryWindow (Window):
	global InventoryWindow

	Window.AddAlias("WIN_INV")
	InventoryWindow = Window
	# ethren: in the play area, clear of the party column and the bar (the column takes items given to others)
	import Roster
	Window.SetFrame (Roster.Centred (*Window.GetSize ()))

	#ground items scrollbar
	ScrollBar = Window.GetControl (66)
	ScrollBar.OnChange (RefreshInventoryWindow)

	# Ground Items (6)
	for cid in HorizontalSlots:
		Button = Window.GetControl (cid)
		Button.OnMouseEnter (InventoryCommon.MouseEnterGround)
		Button.OnMouseLeave (InventoryCommon.MouseLeaveGround)
		Button.SetFont ("NUMBER")
		# ethren: the pack slots' variable, so a drag from one ends here too (a control only takes a drop from
		# one sharing its variable): the item goes on the ground, not back to the cursor
		Button.SetVarAssoc ("ItemButton", 6 + cid)

	Button = Window.GetControl (81)
	Button.SetTooltip (83)
	Button.SetVarAssoc ("ItemButton", 6 + 81)
	Button.SetFont ("NUMBER")
	Button.SetFlags (IE_GUI_BUTTON_ALIGN_RIGHT | IE_GUI_BUTTON_ALIGN_BOTTOM | IE_GUI_BUTTON_PICTURE, OP_OR)

	#major & minor clothing color
	Button = Window.GetControl (62)
	Button.SetFlags (IE_GUI_BUTTON_PICTURE,OP_OR)
	Button.OnPress (lambda: PaperDoll.SelectPickerColor (IE_MAJOR_COLOR))
	Button.SetTooltip ("Major color")

	Button = Window.GetControl (63)
	Button.SetFlags (IE_GUI_BUTTON_PICTURE,OP_OR)
	Button.OnPress (lambda: PaperDoll.SelectPickerColor (IE_MINOR_COLOR))
	Button.SetTooltip ("Minor color")

	#hair & skin color
	Button = Window.GetControl (82)
	Button.SetFlags (IE_GUI_BUTTON_PICTURE,OP_OR)
	Button.OnPress (lambda: PaperDoll.SelectPickerColor (IE_HAIR_COLOR))
	Button.SetTooltip ("Hair color")

	Button = Window.GetControl (83)
	Button.SetFlags (IE_GUI_BUTTON_PICTURE,OP_OR)
	Button.OnPress (lambda: PaperDoll.SelectPickerColor (IE_SKIN_COLOR))
	Button.SetTooltip ("Skin color")

	# paperdoll
	Button = Window.GetControl (50)
	Button.SetState (IE_GUI_BUTTON_LOCKED)
	Button.SetFlags (IE_GUI_BUTTON_NO_IMAGE | IE_GUI_BUTTON_PICTURE, OP_SET)
	Button.SetAction (InventoryCommon.OnAutoEquip, IE_ACT_DRAG_DROP_DST)

	# portrait
	Button = Window.GetControl (84)
	Button.SetState (IE_GUI_BUTTON_LOCKED)
	Button.SetFlags (IE_GUI_BUTTON_NO_IMAGE | IE_GUI_BUTTON_PICTURE, OP_SET)
	Button.OnPress (GUICommonWindows.CloseTopWindow)
	Button.SetHotKey ('i')

	# eXit
	Button = Window.GetControl (200)
	Button.SetText ("X")
	Button.OnPress (GUICommonWindows.CloseTopWindow)

	# armor class
	Label = Window.GetControl (0x10000038)
	Label.SetTooltip ("Armor class")

	# hp current
	Label = Window.GetControl (0x10000039)
	Label.SetTooltip ("Hitpoints")

	# hp max
	Label = Window.GetControl (0x1000003a)
	Label.SetTooltip ("Max hitpoints")

	# info label, game paused, etc: the feedback line of the item code ("MsgSys" 1, as bg1's and iwd's)
	Label = Window.GetControl (0x1000003f)
	Label.SetText ("")
	Label.AddAlias ("MsgSys", 1)

	# C: the record instead (the portrait's I closes the window); a hidden button, for its hot key
	Button = Window.CreateButton (201, 0, 0, 0, 0)
	Button.OnPress (SwapForRecord)
	Button.SetHotKey ('c')

	SlotCount = GemRB.GetSlotType (-1)["Count"]
	for slot in range (SlotCount):
		SlotType = GemRB.GetSlotType (slot+1)
		Button = Window.GetControl (SlotType["ID"])
		if SlotType["ID"] and Button:
			Button.OnMouseEnter (InventoryCommon.MouseEnterSlot)
			Button.OnMouseLeave (InventoryCommon.MouseLeaveSlot)
			Button.SetVarAssoc ("ItemButton", slot+1)
			Button.SetFont ("NUMBER")

	GemRB.SetVar ("TopIndex", 0)

	for i in range (0, 3, 2):
		Button = Window.GetControl (109+i)
		Button.OnPress (ChangeWeaponPressed)
		Button.SetVarAssoc("Equipped", i // 2)
		Button.SetFlags (IE_GUI_BUTTON_RADIOBUTTON, OP_OR)

def SwapForRecord ():
	"""C with the inventory open: the record screen of the same character instead"""
	pc = GemRB.GameGetSelectedPCSingle ()
	GUICommonWindows.CloseTopWindow ()
	import GUIREC
	if not GUIREC.Window:
		GUIREC.OpenRecordsWindow (pc)

def Refresh ():
	"""the open window again, eg. after an item went to another party member (the party column)"""
	if InventoryWindow and GemRB.GetView ("WIN_INV"):
		UpdateInventoryWindow (InventoryWindow)

def ChangeWeaponPressed ():
	pc = GemRB.GameGetSelectedPCSingle ()
	Equipped = GemRB.GetVar ("Equipped")
	GemRB.SetEquippedQuickSlot (pc, Equipped, -1)
	return

#complete update
def UpdateInventoryWindow (Window):
	Window.OnClose(InventoryCommon.InventoryClosed)
	pc = GemRB.GameGetSelectedPCSingle ()
	Container = GemRB.GetContainer (pc, 1)
	ScrollBar = Window.GetControl (66)
	Count = Container['ItemCount']
	# account for two columns
	ScrollBar.SetVarAssoc ("TopIndex", max(0, (Count - 6 + 1) // 2))
	Equipped = GemRB.GetEquippedQuickSlot (pc, 1)
	GemRB.SetVar ("Equipped", Equipped)
	for i in range (0, 3, 2):
		Button = Window.GetControl (109+i)
		Button.SetVarAssoc("Equipped", i // 2)
	RefreshInventoryWindow ()
	# populate inventory slot controls
	SlotCount = GemRB.GetSlotType (-1)["Count"]
	for i in range (SlotCount):
		InventoryCommon.UpdateSlot (pc, i)
	return

ToggleInventoryWindow = GUICommonWindows.CreateTopWinLoader(2, "GUIINV", GUICommonWindows.ToggleWindow, InitInventoryWindow, UpdateInventoryWindow)
OpenInventoryWindow = GUICommonWindows.CreateTopWinLoader(2, "GUIINV", GUICommonWindows.OpenWindowOnce, InitInventoryWindow, UpdateInventoryWindow)

def RefreshInventoryWindow ():
	Window = InventoryWindow

	pc = GemRB.GameGetSelectedPCSingle ()

	# name
	Label = Window.GetControl (0x10000032)
	Label.SetText (GemRB.GetPlayerName (pc, 0))

	# paperdoll
	Button = Window.GetControl (50)

	Button.SetFlags (IE_GUI_BUTTON_CENTER_PICTURES, OP_OR)
	pdoll = PaperDoll.GetActorPaperDoll (pc)
	# FIXME: animation bleeds through top window
	# NOTE: regenerate invenwin.png with a transparent hole if we
	# can ever keep drawing the gamecontrol underneath ...
	# and then remove this block
	if GemRB.HasResource (pdoll, RES_BAM):
		pal = [GemRB.GetPlayerStat (pc, c) for c in range(IE_METAL_COLOR, IE_HAIR_COLOR + 1)]
		Button.SetAnimation (pdoll, 0, A_ANI_ACTIVE, pal)

	# portrait
	Button = Window.GetControl (84)
	# NOTE: use FillPlayerInfo or code to switch pics iff the party will grow
	#Button.SetPicture (GemRB.GetPlayerPortrait (pc,0))
	Button.SetPicture ("portr1L")

	# encumbrance
	InventoryCommon.SetEncumbranceLabels (Window, 0x10000042, None, pc)

	# armor class
	ac = GemRB.GetPlayerStat (pc, IE_ARMORCLASS)
	Label = Window.GetControl (0x10000038)
	Label.SetText (str (ac))

	# hp current
	hp = GemRB.GetPlayerStat (pc, IE_HITPOINTS)
	Label = Window.GetControl (0x10000039)
	Label.SetText (str (hp))

	# hp max
	hpmax = GemRB.GetPlayerStat (pc, IE_MAXHITPOINTS)
	Label = Window.GetControl (0x1000003a)
	Label.SetText (str (hpmax))

	# party gold
	Label = Window.GetControl (0x10000040)
	Label.SetText (str (GemRB.GameGetPartyGold ()))

	Button = Window.GetControl (62)
	Color = GemRB.GetPlayerStat (pc, IE_MAJOR_COLOR, 1) & 0xFF
	Button.SetBAM ("COLGRAD", 0, 0, Color)

	Button = Window.GetControl (63)
	Color = GemRB.GetPlayerStat (pc, IE_MINOR_COLOR, 1) & 0xFF
	Button.SetBAM ("COLGRAD", 0, 0, Color)

	Button = Window.GetControl (82)
	Color = GemRB.GetPlayerStat (pc, IE_HAIR_COLOR, 1) & 0xFF
	Button.SetBAM ("COLGRAD", 0, 0, Color)

	Button = Window.GetControl (83)
	Color = GemRB.GetPlayerStat (pc, IE_SKIN_COLOR, 1) & 0xFF
	Button.SetBAM ("COLGRAD", 0, 0, Color)

	# update ground inventory slots
	TopIndex = GemRB.GetVar ("TopIndex") * 2
	for (i, cid) in enumerate(HorizontalSlots):
		Button = Window.GetControl (cid)
		# use a different item, so the order is LTR
		i = min(5, HorizontalSlots[cid] - 68)
		slotID = i + TopIndex

		if GemRB.IsDraggingItem ()==1:
			Button.SetState (IE_GUI_BUTTON_FAKEPRESSED)
		else:
			Button.SetState (IE_GUI_BUTTON_ENABLED)
		Button.SetAction (InventoryCommon.OnDragItemGround, IE_ACT_DRAG_DROP_DST)

		Slot = GemRB.GetContainerItem (pc, slotID)
		# ethren: a ground item can be dragged into the pack too, not only clicked (as bg2's)
		Button.SetAction (InventoryCommon.OnDragItemGround if Slot else None, IE_ACT_DRAG_DROP_CRT)
		if Slot == None:
			Button.OnPress (None)
			Button.OnRightPress (None)
			Button.OnShiftPress (None)
			Button.OnDoublePress (None)
		else:
			Button.SetValue (slotID)
			Button.OnPress (InventoryCommon.OnDragItemGround)
			Button.OnRightPress (InventoryCommon.OpenGroundItemInfoWindow)
			Button.OnShiftPress (InventoryCommon.OpenGroundItemAmountWindow)
			Button.OnDoublePress (InventoryCommon.OpenGroundItemAmountWindow)

		InventoryCommon.UpdateInventorySlot (pc, Button, Slot, "ground")

	#if actor is uncontrollable, make this grayed
	GUICommon.AdjustWindowVisibility (Window, pc, False)
	return

###################################################
# End of file GUIINV.py
