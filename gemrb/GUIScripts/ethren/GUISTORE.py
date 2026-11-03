# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the store window (the engine's EF_OPENSTORE runs GUISTORE.OpenStoreWindow, a StartStore in a
# dialog). The shared GUISTORE.py needs GUISTORE.chu, which neither the demo nor Ethren has, so this one is
# built from code in the demo's plain colours (Phase 5 skins it). Functional so far: an inn's rooms and,
# beside Rent room, the Reserves tab (docs/architecture/ui-design.md, "The reserves at an inn or camp"):
#   - the store's name, the purse, a tab per service the store offers (the engine's StoreButtons order) and
#     Done; the services not built here yet (buying and selling, identifying, drinks, ...) show disabled;
#   - Rent room: the rooms the store has with their prices (cp); Rent pays and rests the party for the night
#     (bed rest: +2 HP and ability loss as bed rest, combat.md), everyone together;
#   - Reserves: only in a store that offers rooms (any room price in the STO), once the funnel is over: the
#     reserves' screen (Reserves.py) over this window; its Back returns here.
# Co-op: the store opens on the screen of the player whose conversation it was (multiplayer.md, "Stores");
# renting is the routed RestParty and GameSetPartyGold, the reserves' calls are routed too.

import GemRB
from GUIDefines import *

Font = "NORMAL"
TextFont = "INFOFONT"
WindowColor = {'r': 22, 'g': 19, 'b': 26, 'a': 250}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
TabColor = {'r': 45, 'g': 40, 'b': 36, 'a': 255}
PickedColor = {'r': 120, 'g': 100, 'b': 55, 'a': 255}
WINDOW_ID = 84
W, H = 600, 360
GAP = 8

# the engine's StoreActionType: the tab's name; only RENT (and Reserves) are built here
ACTIONS = {0: "Buy and sell", 1: "Identify", 2: "Steal", 3: "Healing", 4: "Donate", 5: "Drinks", 6: "Rent room"}
RENT = 6
RESERVES = 100 # our own tab
ROOMS = ("Common room", "Merchant's room", "Noble's room", "Royal suite")
BED_REST_HP = 2 # a night in a bed (combat.md, "Healing": bed rest +2 HP)
RENT_CHECKS = 2 | 16 # the party together, no repeats (as the stock inn's rest)

StoreWindow = None
Store = None
Tab = None
Room = -1
Shown = {} # for tests: "Name", "Tabs" [(name, enabled)], "Tab", "Rooms" [(name, price) or None], "Text", "Purse"

def _Log (text):
	print ("[Store] " + text)

def HasRooms (store = None):
	store = store or Store
	return bool (store) and any (price is not None for price in store["StoreRoomPrices"])

def IsInn ():
	"""this player's store window is open on a store that offers rooms: the reserves may join here"""
	if not _Alive ():
		return False
	return HasRooms (GemRB.GetStore ())

def _Alive ():
	try:
		return bool (StoreWindow and StoreWindow.GetControl (1))
	except Exception:
		return False

def Tabs ():
	"""the tabs this store gets: its services (StoreButtons), then Reserves where it offers rooms"""
	tabs = [action for action in Store["StoreButtons"] if action is not None]
	if HasRooms () and GemRB.GetGameVar ("DCC_PARTY_SIZE"):
		tabs.append (RESERVES)
	return tabs

def TabName (tab):
	return "Reserves" if tab == RESERVES else ACTIONS.get (tab, "?")

def Built (tab):
	return tab in (RENT, RESERVES)

def OpenStoreWindow ():
	global StoreWindow, Store, Tab, Room
	CloseWindows ()
	Store = GemRB.GetStore ()
	if not Store:
		return
	GemRB.GamePause (1, 3) # quietly: in co-op the others play on
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	win = GemRB.CreateView (WINDOW_ID, 255, {'x': (screen[0] - W) // 2, 'y': max (0, (screen[1] - H) // 2 - 40), 'w': W, 'h': H})
	win.AddAlias ("WIN_STORE")
	win.SetBackground (WindowColor)
	name = GemRB.GetString (Store["StoreName"]) if Store["StoreName"] >= 0 else "The store"
	win.CreateLabel (0x10000000, GAP, GAP, W - 2 * GAP - 150, 28, Font, name, IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_MIDDLE)
	win.CreateLabel (0x10000001, W - GAP - 150, GAP, 150, 28, TextFont, "", IE_FONT_ALIGN_RIGHT | IE_FONT_ALIGN_MIDDLE)
	done = _Button (win, 1, W - GAP - 110, H - GAP - 30, 110, 30, "Done", CloseStoreWindow, "Leave the store")
	done.MakeEscape ()
	tabs = Tabs ()
	w = (W - 2 * GAP - (len (tabs) - 1) * 4) // max (1, len (tabs))
	for i, tab in enumerate (tabs):
		button = _Button (win, 10 + i, GAP + i * (w + 4), 44, w, 26, TabName (tab), lambda tab = tab: SelectTab (tab), None, TextFont)
		if not Built (tab):
			button.SetState (IE_GUI_BUTTON_DISABLED)
			button.SetTooltip ("Not in Ethren's GUI yet")
	# Rent room: the rooms, a line about the picked one, Rent
	for i in range (len (ROOMS)):
		_Button (win, 20 + i, GAP, 84 + i * 36, 260, 30, "", lambda i = i: PickRoom (i), None, TextFont)
	win.CreateLabel (0x10000002, 280, 84, W - 280 - GAP, 140, TextFont, "", IE_FONT_ALIGN_LEFT | IE_FONT_ALIGN_TOP)
	_Button (win, 30, 280, 84 + 3 * 36, 120, 30, "Rent", RentRoom, "Pay and rest the night (bed rest)")
	StoreWindow = win
	Tab, Room = None, -1
	_Log ("%s opens: tabs %s" % (name, ", ".join (TabName (t) for t in tabs)))
	first = [tab for tab in tabs if Built (tab) and tab != RESERVES]
	SelectTab (first[0] if first else None)
	win.Focus ()
	return win

def _Button (win, id, x, y, w, h, text, action, tooltip = None, font = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (font or Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	if action:
		button.OnPress (lambda: action ())
	if tooltip:
		button.SetTooltip (tooltip)
	return button

def SelectTab (tab):
	"""a tab: Rent room shows the rooms here; Reserves opens the reserves' screen over the window"""
	global Tab
	if tab == RESERVES:
		return OpenReserves ()
	if tab is not None and not Built (tab):
		return None
	Tab = tab
	Refresh ()
	return StoreWindow

def OpenReserves ():
	import Reserves
	if not IsInn () or not GemRB.GetGameVar ("DCC_PARTY_SIZE"):
		return None
	return Reserves.Open ("inn", Refresh)

def PickRoom (room):
	global Room
	prices = Store["StoreRoomPrices"]
	if not 0 <= room < len (prices) or prices[room] is None:
		return False
	Room = room
	Refresh ()
	return True

def Refresh (text = None):
	if not _Alive ():
		return
	win = StoreWindow
	tabs = Tabs ()
	Shown.clear ()
	Shown["Name"] = win.GetControl (0x10000000).QueryText ()
	Shown["Tabs"] = [(TabName (t), Built (t)) for t in tabs]
	Shown["Tab"] = TabName (Tab) if Tab is not None else None
	Shown["Purse"] = GemRB.GameGetPartyGold ()
	win.GetControl (0x10000001).SetText ("Purse: %d cp" % Shown["Purse"])
	for i, tab in enumerate (tabs):
		button = win.GetControl (10 + i)
		if button and Built (tab):
			button.SetBackground (PickedColor if tab == Tab else TabColor)
	renting = Tab == RENT
	prices = Store["StoreRoomPrices"]
	Shown["Rooms"] = []
	for i in range (len (ROOMS)):
		button = win.GetControl (20 + i)
		price = prices[i] if i < len (prices) else None
		button.SetVisible (renting)
		if price is None:
			button.SetText ("%s: not here" % ROOMS[i])
			button.SetState (IE_GUI_BUTTON_DISABLED)
			Shown["Rooms"].append (None)
		else:
			button.SetText ("%s: %d cp" % (ROOMS[i], price))
			button.SetState (IE_GUI_BUTTON_ENABLED)
			button.SetBackground (PickedColor if i == Room else ButtonColor)
			Shown["Rooms"].append ((ROOMS[i], price))
	rent = win.GetControl (30)
	rent.SetVisible (renting)
	rent.SetState (IE_GUI_BUTTON_ENABLED if Room >= 0 else IE_GUI_BUTTON_DISABLED)
	if text is None:
		text = ""
		if renting and Room < 0:
			text = "A bed for the night: pick a room, then Rent. Everyone sleeps (bed rest: +%d HP)." % BED_REST_HP
		elif renting:
			text = "%s, %d cp for the night." % (ROOMS[Room], prices[Room])
		if RESERVES in tabs:
			text += "\n\nThe reserves wait here too: see the Reserves tab."
	Shown["Text"] = text
	win.GetControl (0x10000002).SetText (text)

def RentRoom ():
	"""pay for the picked room and rest the night; the text says how it went"""
	if Room < 0:
		return False
	price = Store["StoreRoomPrices"][Room] or 0
	gold = GemRB.GameGetPartyGold ()
	if gold < price:
		Refresh ("You can't afford it: %d cp, the purse holds %d." % (price, gold))
		return False
	GemRB.GameSetPartyGold (gold - price)
	info = GemRB.RestParty (RENT_CHECKS, -1, BED_REST_HP)
	if info and info["Error"]:
		GemRB.GameSetPartyGold (gold)
		why = GemRB.GetString (info["ErrorMsg"]) if info["ErrorMsg"] >= 0 else "not now"
		Refresh ("You can't rest now: %s" % why)
		_Log ("no rest: %s" % why)
		return False
	_Log ("rented the %s for %d cp and rested" % (ROOMS[Room], price))
	if info and info.get ("Cutscene"):
		CloseStoreWindow ()
		return True
	Refresh ("You slept in the %s (%d cp): rested." % (ROOMS[Room].lower (), price))
	return True

def CloseWindows ():
	global StoreWindow
	import Reserves
	if Reserves.Window and Reserves.Where == "inn":
		Reserves.Close ()
	if StoreWindow:
		try:
			StoreWindow.Close ()
		except Exception:
			pass # gone with the game already (a load, the host's new save in co-op)
		StoreWindow = None

def CloseStoreWindow ():
	global Store
	CloseWindows ()
	Store = None
	GemRB.LeaveStore ()
	GemRB.GamePause (0, 3)
	_Log ("left the store")
