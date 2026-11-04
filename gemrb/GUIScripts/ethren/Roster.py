# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the party column (docs/architecture/ui-design.md, "The party column and the slim bar"), in
# place of the portrait column the demo GUI doesn't have: one narrow column down the right edge, built from code
# (no CHU), in the demo's plain colours until Phase 5 skins it; it refreshes itself on a timer.
#   - the header: the party summary ("Alive 12 of 16", "2 dying, 2 dead" under it; a press opens the graveyard)
#     and the prompts waiting for this player ("3 waiting");
#   - saved groups 1-9: Ctrl+1-9 (or a right click) saves the selection, Alt+1-9 (or a click) selects it;
#   - a tile for each party member, one under the other. In the funnel (more than six) one-line ones: the name and
#     the worst status, an HP bar along the bottom (the occupation is in the hover line); sixteen fit at 720, and
#     when they don't fit (a smaller screen) the list scrolls (the mouse wheel over the column, or its scroll bar). After the funnel (six or fewer) portrait tiles, as the IE games': a portrait
#     (until portraits can be picked: the character's own figure, as the game draws it), the name, class and
#     level, HP, Luck and the status, the HP bar under them. Either kind: the selection highlight, the owner's
#     colour as a stripe down the left edge in co-op; a click selects one (with the inventory or record open:
#     shows that one), a shift click (or a right click) adds or takes one away, a double click centres on it;
#     an item picked up in the inventory and clicked on a tile goes to that one; hovering shows occupation, HP,
#     Luck and weapon. The dying flash, the dead stay in the list, greyed and marked (dead).
# The bar's buttons (MessageWindow.py) hold the rest: select all and mine, the formations, the party settings, Rest.
# The window ignores events itself, as the stock side windows do (its controls still take clicks), so the game
# view keeps the keyboard and the keymap's keys work; a backdrop button eats the clicks between the tiles, and
# the scroll bar takes the wheel turns over the column (the window's event proxy).
# The tiles are rebuilt whenever the party changes (who is in which slot: the funnel's end, a dismissal,
# a recruit, the dead taken out, a load, a co-op replica's party), so the column shows the current party
# only. A hidden tile would not do: hiding a view doesn't redraw the window under it, so the old tiles'
# pixels stayed (M3.3's "Alive 4 of 4" over sixteen tiles).
# Drag-box selection works in the world as before.

import GemRB
import FunnelRoll
from GUIDefines import *
from ie_stats import *

Font = "INFOFONT"
HeaderFont = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 235}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
TileColor = {'r': 52, 'g': 46, 'b': 38, 'a': 255}
SelectedColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}
DeadColor = {'r': 34, 'g': 34, 'b': 34, 'a': 255}
DyingColor = {'r': 150, 'g': 30, 'b': 25, 'a': 255}
DeadText = {'r': 120, 'g': 120, 'b': 120, 'a': 255}
LiveText = {'r': 235, 'g': 228, 'b': 210, 'a': 255}
BarBack = {'r': 20, 'g': 20, 'b': 20, 'a': 255}
PortraitBack = {'r': 30, 'g': 28, 'b': 34, 'a': 255}
GroupSet = {'r': 90, 'g': 80, 'b': 55, 'a': 255}
GroupEmpty = {'r': 45, 'g': 40, 'b': 40, 'a': 255}
# each co-op player's colour (slot 0 the host), on the tiles they own and the rolling screen's cards
PLAYER_COLORS = [
	{'r': 225, 'g': 185, 'b': 60, 'a': 255},
	{'r': 70, 'g': 135, 'b': 235, 'a': 255},
	{'r': 85, 'g': 195, 'b': 95, 'a': 255},
	{'r': 215, 'g': 85, 'b': 75, 'a': 255},
]
DEAD_MARK = "(dead)" # in place of the skull until Phase 5 draws one (the demo's font has no cross or skull)

MAX_SLOTS = 16
COLUMN_W = 176 # the column's width, at any screen size
GAP = 4
BAR_H = 4
OWNER_W = 4 # the owner's colour: a stripe down the tile's left edge (co-op)
SUMMARY_Y = 4
SUMMARY_H = 30
GROUPS_Y = 38
GROUPS_H = 16
LIST_Y = 58
TILE_H = 24 # a funnel tile, one line: name and status, the HP bar under them
TILE_GAP = 2 # between funnel tiles
HERO_H = 100 # a portrait tile, after the funnel
HERO_MAX = 6 # parties this size or smaller get portrait tiles
PORTRAIT_W = 64
SCROLL_ID = 120
BACKDROP_ID = 99
PORTRAIT_ID = 40 # + the tile's index
GROUPS = 9
# formatio.2da's rows (ethren): the names; the buttons offer the two of 16 (block and column)
FORMATIONS = ["Block", "Column", "Line", "Threes", "Wedge"]

RosterWindow = None
TimerSet = False
Ticks = 0
Shown = {} # what each slot shows, for tests: slot -> the tile's text
Tiles = {} # slot -> {"Text", "Status", "Tooltip", "Owner", "Selected", "Dying", "Dead", "HP"}
Header = {} # "Summary", "Waiting"
Groups = {} # 1-9 -> [party slots]
Built = None # the party the tiles were built for: each slot's global ID, in slot order
Rebuilds = 0 # how often the tiles were rebuilt for a new party (for tests)
Layout = {} # the tiles as built: "Hero", "Rows" (tiles on show), "Pitch", "TileW", "Count"
Top = 0 # the first party member on show (0-based), when the list scrolls
Portraits = {} # slot -> what its portrait shows (for tests): "figure" or "placeholder"

def ColumnWidth ():
	return COLUMN_W

def PlayArea ():
	"""the screen left of the party column and above the bar: where screens over the game (the inventory, the record)
	go, so the column and the bar stay in sight and usable"""
	import MessageWindow
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	return {'x': 0, 'y': 0, 'w': screen[0] - COLUMN_W, 'h': screen[1] - MessageWindow.BAR_H}

def Centred (w, h):
	"""the frame of a w by h window centred in the play area (as far up as it must go when it is taller)"""
	area = PlayArea ()
	return {'x': max (0, area['x'] + (area['w'] - w) // 2), 'y': max (0, area['y'] + (area['h'] - h) // 2), 'w': w, 'h': h}

def PartyKey ():
	"""who is in the party, slot by slot (global IDs): the tiles are rebuilt when it changes"""
	return tuple (GemRB.GetDCCFunnel (slot)["GlobalID"] for slot in range (1, min (_Size (), MAX_SLOTS) + 1))

MaxListHeight = None # for tests: a shorter column than the screen gives (to see it scroll)

def _ListHeight ():
	"""the column's height for tiles: from under the groups to the bottom of the screen"""
	height = GemRB.GetSystemVariable (SV_HEIGHT) - LIST_Y - GAP
	return min (height, MaxListHeight) if MaxListHeight else height

def OpenRoster ():
	global RosterWindow, Built, Top
	if RosterWindow:
		return RosterWindow
	Built = PartyKey ()
	count = len (Built)
	hero = count <= HERO_MAX
	gap = GAP if hero else TILE_GAP
	pitch = (HERO_H if hero else TILE_H) + gap
	rows = min (count, max (1, (_ListHeight () + gap) // pitch))
	scrolls = rows < count
	Top = min (Top, count - rows) if scrolls else 0
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': screen[0] - COLUMN_W, 'y': 0, 'w': COLUMN_W, 'h': screen[1]}
	win = GemRB.CreateView (95, 255, frame)
	win.AddAlias ("ROSTWIN")
	win.SetBackground (WindowColor)
	# like the stock side windows: the controls take clicks, the window never the keyboard (the game view keeps
	# it, for the keymap's keys); the backdrop eats the clicks between the controls (they would fall through to
	# the game view and walk the party)
	win.SetFlags (IE_GUI_VIEW_IGNORE_EVENTS, OP_OR)
	inner = COLUMN_W - 2 * GAP
	summary = _Button (win, 100, GAP, SUMMARY_Y, inner - 56 - GAP, SUMMARY_H, "", OpenGraveyard, "The graveyard: who died, how and where")
	summary.SetFlags (IE_GUI_BUTTON_ALIGN_LEFT, OP_OR)
	waiting = _Button (win, 101, COLUMN_W - GAP - 56, SUMMARY_Y, 56, SUMMARY_H, "", None, "Prompts waiting for you")
	waiting.SetState (IE_GUI_BUTTON_LOCKED)
	# the saved groups: Ctrl+N saves the selection, Alt+N selects it (a click selects, a right click saves)
	w = (inner - (GROUPS - 1) * 2) // GROUPS
	for n in range (1, GROUPS + 1):
		button = _Button (win, 110 + n, GAP + (n - 1) * (w + 2), GROUPS_Y, w, GROUPS_H, str (n), lambda n = n: SelectGroup (n), "Group %d: a click (Alt+%d) selects it, a right click (Ctrl+%d) saves the selection" % (n, n, n))
		button.OnRightPress (lambda control, n = n: SaveGroup (n))
		button.SetBackground (GroupEmpty)
		# hot keys are the button's own (one key each), so the saving keys get buttons of their own, hidden
		saver = win.CreateButton (130 + n, 0, 0, 0, 0)
		saver.OnPress (lambda control, n = n: SaveGroup (n))
		saver.SetHotKey (str (n), GEM_MOD_CTRL, True)
		button.SetHotKey (str (n), GEM_MOD_ALT, True)
	scrollW = 0
	if scrolls:
		scroll = win.CreateScrollBar (SCROLL_ID, {'x': COLUMN_W - GAP - 20, 'y': LIST_Y, 'w': 20, 'h': rows * pitch - gap})
		scrollW = scroll.GetFrame ()['w']
		scroll.SetFrame ({'x': COLUMN_W - GAP - scrollW, 'y': LIST_Y, 'w': scrollW, 'h': rows * pitch - gap})
		scroll.SetVarAssoc ("RosterTop", Top, 0, count - rows)
		scroll.OnChange (lambda: ScrollTo (GemRB.GetVar ("RosterTop")))
		# the wheel over the column (the tiles pass it up to the window): the scroll bar's
		win.SetEventProxy (scroll)
	tileW = inner - (scrollW + 2 if scrollW else 0)
	tileH = pitch - gap
	# (a new view goes behind the ones before it: the backdrop last. Views must not overlap: a tile redrawn on
	# its timer would paint over a sibling on top of it)
	textX = GAP + (PORTRAIT_W + 2 if hero else 0) # a portrait tile: the picture on the left, the text tile beside it
	for i in range (rows):
		y = LIST_Y + i * pitch
		if hero:
			# the portrait: a button of its own left of the tile, with the tile's handlers
			portrait = win.CreateButton (PORTRAIT_ID + i, GAP, y, PORTRAIT_W, tileH)
			portrait.SetFlags (IE_GUI_BUTTON_NO_IMAGE | IE_GUI_BUTTON_PICTURE | IE_GUI_BUTTON_CENTER_PICTURES, OP_OR)
			portrait.SetBackground (PortraitBack)
			portrait.SetFont (HeaderFont)
			portrait.OnPress (lambda control, i = i: _TileAction (SelectOnly, i))
			_TileHandlers (portrait, i)
		tile = _Button (win, i, textX, y, tileW - (textX - GAP), tileH, "", lambda i = i: _TileAction (SelectOnly, i), None)
		tile.SetFlags (IE_GUI_BUTTON_ALIGN_LEFT | IE_GUI_BUTTON_ALIGN_TOP | IE_GUI_BUTTON_ALIGN_ANCHOR, OP_OR)
		tile.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND) # its borders (the owner, the HP bar) need it off
		_TileHandlers (tile, i)
		tile.SetAnchor (OWNER_W + 4, 4 if hero else 2)
	tileW -= textX - GAP
	Backdrop (win, COLUMN_W, screen[1])
	Layout.clear ()
	Layout.update ({"Hero": hero, "Rows": rows, "Pitch": pitch, "TileW": tileW, "TileH": tileH, "Count": count, "Scrolls": scrolls})
	Portraits.clear ()
	RosterWindow = win
	_StartTimer ()
	UpdateRoster ()
	return win

def Backdrop (win, w, h):
	"""a locked button behind everything (create it last): the clicks between the controls stop there instead of
	falling through a window that ignores events to the game view (and walking the party)"""
	backdrop = win.CreateButton (BACKDROP_ID, 0, 0, w, h)
	backdrop.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	backdrop.SetState (IE_GUI_BUTTON_LOCKED)
	return backdrop

def _TileHandlers (button, i):
	button.OnShiftPress (lambda control, i = i: _TileAction (ToggleSelect, i))
	button.OnRightPress (lambda control, i = i: _TileAction (ToggleSelect, i))
	button.OnDoublePress (lambda control, i = i: _TileAction (CenterOn, i))
	# an item picked up in the inventory, dropped on the tile: to that party member
	button.SetAction (lambda control, i = i: _TileAction (GiveItem, i), IE_ACT_DRAG_DROP_DST)

def _TileAction (action, i):
	"""a tile's click: what it does to the party member it shows now (the list may have scrolled)"""
	slot = Top + i + 1
	if slot <= len (Built or ()):
		action (slot)

def ScrollTo (top):
	"""the list from party member top (0-based) on; clamped to the party"""
	global Top
	rows, count = Layout.get ("Rows", 0), Layout.get ("Count", 0)
	top = max (0, min (int (top), count - rows))
	if top == Top:
		return
	Top = top
	if RosterWindow and Layout.get ("Scrolls"):
		GemRB.SetVar ("RosterTop", Top)
		scroll = RosterWindow.GetControl (SCROLL_ID)
		if scroll:
			scroll.SetVarAssoc ("RosterTop", Top, 0, count - rows)
	Portraits.clear ()
	UpdateRoster ()

def TileControl (slot):
	"""the tile showing party member slot, scrolling the list to it if need be (for tests and clicks)"""
	rows = Layout.get ("Rows", 0)
	if slot <= Top:
		ScrollTo (slot - 1)
	elif slot > Top + rows:
		ScrollTo (slot - rows)
	return RosterWindow.GetControl (slot - Top - 1) if RosterWindow else None

def OnShow ():
	"""the party members on show now: their slots, top to bottom"""
	return list (range (Top + 1, Top + Layout.get ("Rows", 0) + 1))

def Rebuild ():
	"""the tiles again for the party as it is now (the window closed and made anew)"""
	global RosterWindow, Rebuilds
	old = RosterWindow
	RosterWindow = None
	if old:
		try:
			old.Close ()
		except Exception:
			pass # gone with the game already
	Rebuilds += 1
	size = _Size ()
	for n in list (Groups):
		Groups[n] = [slot for slot in Groups[n] if slot <= size]
	Shown.clear ()
	Tiles.clear ()
	return OpenRoster ()

def TileCount ():
	"""the party members the column has tiles for, on show or a scroll away (for tests: one beyond the party would
	be a stale tile); tile controls past the list's rows would be stale too"""
	if not RosterWindow:
		return 0
	controls = sum (1 for i in range (MAX_SLOTS) if RosterWindow.GetControl (i))
	if controls != Layout.get ("Rows"):
		return -controls
	return Layout.get ("Count", 0)

def _Button (win, id, x, y, w, h, text, action, tooltip = None, font = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (font or Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	if action:
		# the engine passes the button to a handler that takes arguments, which would fill a lambda's
		# defaults (lambda i = i: ...): the action is called without any
		button.OnPress (lambda: action ())
	if tooltip:
		button.SetTooltip (tooltip)
	return button

def _StartTimer ():
	global TimerSet
	if not TimerSet:
		TimerSet = True
		GemRB.SetTimer (_Tick, 500, -1)

def _Tick ():
	global RosterWindow, Ticks
	Ticks += 1
	try:
		UpdateRoster ()
	except Exception:
		# the window went with the game (back to the menu, a load)
		RosterWindow = None
	try:
		import MessageWindow
		MessageWindow.UpdateBar ()
	except Exception:
		pass
	# M3.3: the end of the funnel and the reserves, when the game's state asks for them
	try:
		if RosterWindow:
			import ChooseFour
			ChooseFour.Watch ()
	except Exception as error:
		print ("[Choose] watch failed: %s" % error)

###################################################
# selection

def _Size ():
	return GemRB.GetPartySize ()

def _ShowingOne ():
	"""the inventory (a top window with a selection handler) or the record screen is open: a click shows that one"""
	import GUIREC
	return bool (GemRB.GetView ("WIN_INV")) or GUIREC.Window is not None

def SelectOnly (slot):
	if slot > _Size ():
		return
	if _ShowingOne ():
		GemRB.GameSelectPCSingle (slot)
		import GUIREC
		if GUIREC.Window:
			GUIREC.ShowRecord (slot)
	else:
		GemRB.GameSelectPC (slot, 1, SELECT_REPLACE)
	UpdateRoster ()

def GiveItem (slot):
	"""an item picked up in the inventory, clicked on a tile: into that party member's pack (too far away, or a full
	pack, and it stays in hand)"""
	if not GemRB.IsDraggingItem ():
		SelectOnly (slot)
		return
	import InventoryCommon
	import GUIINV
	if GemRB.GetView ("MsgSys", 1):
		InventoryCommon.OnDropItemToPC (slot)
	else:
		GemRB.DropDraggedItem (slot, -3)
	GUIINV.Refresh ()
	UpdateRoster ()

def ToggleSelect (slot):
	if slot <= _Size ():
		GemRB.GameSelectPC (slot, 0 if GemRB.GameIsPCSelected (slot) else 1)
		UpdateRoster ()

def SelectAll ():
	GemRB.GameSelectPC (0, 1)
	UpdateRoster ()

def Mine ():
	"""the party slots this player controls (co-op), or everyone"""
	if not GemRB.NetGetMode ():
		return list (range (1, _Size () + 1))
	lobby = GemRB.NetGetLobby ()
	if not lobby:
		return []
	return [p["Slot"] for p in lobby["Party"] if p["Controller"] == lobby["Slot"] and p["Slot"] <= _Size ()]

def SelectMine ():
	mine = [slot for slot in Mine () if not _Dead (slot)]
	GemRB.GameSelectPC (0, 0)
	for slot in mine:
		GemRB.GameSelectPC (slot, 1)
	UpdateRoster ()
	return mine

def Selected ():
	return [slot for slot in range (1, _Size () + 1) if GemRB.GameIsPCSelected (slot)]

def SaveGroup (n):
	Groups[n] = Selected ()
	print ("[Roster] group %d: %s" % (n, Groups[n]))
	UpdateRoster ()

def SelectGroup (n):
	group = [slot for slot in Groups.get (n, []) if slot <= _Size () and not _Dead (slot)]
	if not group:
		return False
	GemRB.GameSelectPC (group[0], 1, SELECT_REPLACE)
	for slot in group[1:]:
		GemRB.GameSelectPC (slot, 1)
	UpdateRoster ()
	return True

def CenterOn (slot):
	SelectOnly (slot)
	GemRB.GameControlSetScreenFlags (SF_CENTERONACTOR, OP_OR)

def CurrentFormation ():
	"""formatio.2da's row the party walks in (the preset the game has picked)"""
	return GemRB.GameGetFormation (GemRB.GameGetFormation ())

def Formation (formation):
	"""sets the party's formation and lines up the selection in it, where its first one stands"""
	GemRB.GameSetFormation (formation, GemRB.GameGetFormation ())
	selected = [slot for slot in Selected () if not _Dead (slot)]
	if len (selected) > 1:
		where = GemRB.GetActorPosition (selected[0])
		GemRB.GameControlMoveSelected (where["X"], where["Y"])
	UpdateRoster ()

def NextFormation ():
	Formation ((CurrentFormation () + 1) % len (FORMATIONS))

def RestPress ():
	import Camp
	return Camp.RestPress ()

def OpenSettings ():
	import DCCParty
	DCCParty.OpenPartySettings ()

###################################################
# what a tile shows

def _Dead (slot):
	return bool (GemRB.GetPlayerStat (slot, IE_STATE_ID) & STATE_DEAD)

TableCache = {}
ItemNames = {}

def _Table (name):
	"""a funnel table, read once (dccoccup, dccoccit)"""
	if name not in TableCache:
		try:
			TableCache[name] = FunnelRoll.ReadGemRBTable (name)
		except Exception:
			TableCache[name] = FunnelRoll.Table ([], [])
	return TableCache[name]

def _Occupations ():
	return _Table ("dccoccup")

def OccupationName (occupation, short = False):
	"""an OCCU record's occupation as text ("farmer", the crop with it unless short)"""
	ident = occupation["Id"]
	if not ident:
		return ""
	table = _Occupations ()
	name = table.Text (ident, "NAME", "occupation %d" % ident)
	name = name[0].lower () + name[1:]
	sub = occupation["Sub"]
	if not short and table.Get (ident, "SUB") == "FARMER" and sub & 15:
		name += " (%s)" % _Table ("dccoccit").Text ("FARMER_%d" % (sub & 15), "NAME")
	return name

def WorstStatus (slot):
	"""the worst of a party member's states, as a word: dead, dying, out, held, witless, asleep,
	afraid, poisoned, cursed; "" for none"""
	state = GemRB.GetPlayerStat (slot, IE_STATE_ID)
	dcc = GemRB.GetPlayerStat (slot, IE_DCC_STATE)
	if state & STATE_DEAD:
		return "dead"
	if dcc & DCC_STATE_DYING or GemRB.GetPlayerStat (slot, IE_DCC_BLEED) > 0:
		return "dying"
	if dcc & DCC_STATE_UNCONSCIOUS:
		return "out"
	if dcc & DCC_STATE_IMMOBILE or state & STATE_PETRIFIED:
		return "held"
	if dcc & DCC_STATE_WITLESS:
		return "witless"
	if state & STATE_HELPLESS:
		return "asleep"
	if state & STATE_PANIC:
		return "afraid"
	if state & STATE_POISONED:
		return "poisoned"
	if dcc & DCC_STATE_CURSED:
		return "cursed"
	return ""

def _Owners ():
	"""co-op: party slot -> the player who owns it (None outside co-op)"""
	if not GemRB.NetGetMode ():
		return None
	lobby = GemRB.NetGetLobby ()
	if not lobby:
		return None
	return dict ((p["Slot"], p["Owner"]) for p in lobby["Party"])

def _ItemName (item):
	if item not in ItemNames:
		try:
			ItemNames[item] = GemRB.GetString (GemRB.GetItem (item)["ItemNameIdentified"]).lower () or item.lower ()
		except Exception:
			ItemNames[item] = item.lower ()
	return ItemNames[item]

def _WeaponName (slot, occupation):
	"""the weapon in its hand; else the occupation's (the table's name while it has no item)"""
	held = GemRB.GetSlotItem (slot, GemRB.GetEquippedQuickSlot (slot))
	if held and held["ItemResRef"].upper () != "FIST":
		return _ItemName (held["ItemResRef"])
	if occupation["Weapon"]:
		return "bare hands" # it has its weapon, just not in hand
	table = _Occupations ()
	weapon = table.Get (occupation["Id"], "WEAPON") if occupation["Id"] else None
	if weapon:
		return "%s (no item yet)" % _Table ("dccoccit").Text (weapon, "NAME", weapon).lower ()
	return "bare hands"

def TileInfo (slot, owners = None):
	name = GemRB.GetPlayerName (slot, 0)
	hp = GemRB.GetPlayerStat (slot, IE_HITPOINTS)
	hpMax = GemRB.GetPlayerStat (slot, IE_MAXHITPOINTS)
	status = WorstStatus (slot)
	record = GemRB.GetDCCFunnel (slot)
	occupation = OccupationName (record["Occupation"], True)
	luck = GemRB.GetPlayerStat (slot, IE_DCC_LUCK)
	dead = status == "dead"
	first = "%s  %s" % (name, DEAD_MARK) if dead else name
	if status and not dead:
		first += "  [%s]" % status
	text = first # the funnel's one line; the occupation is in the hover line
	if Layout.get ("Hero"):
		# a portrait tile: class and level, HP and Luck on lines of their own
		import DCCSheet
		stats = GemRB.GetDCCStats (slot)
		klass = DCCSheet.ClassText (stats) if stats else ""
		text = "%s\n%s\nHP %d/%d\nLuck %d" % (first, klass or occupation or "-", max (hp, 0), hpMax, luck)
	tooltip = "%s: %s, HP %d/%d, Luck %d, %s" % (name, OccupationName (record["Occupation"]) or "no occupation", max (hp, 0), hpMax, luck, _WeaponName (slot, record["Occupation"]))
	if status:
		tooltip += " (%s)" % status
	return {
		"Text": text, "Status": status, "Tooltip": tooltip, "HP": hp, "MaxHP": hpMax,
		"Owner": owners.get (slot) if owners else None, "Selected": bool (GemRB.GameIsPCSelected (slot)),
		"Dead": dead, "Dying": status == "dying",
	}

def Summary (infos):
	"""the header line, eg. "Alive 12 of 16, 2 dying, 2 dead" """
	total = len (infos)
	dead = sum (1 for info in infos if info["Dead"])
	dying = sum (1 for info in infos if info["Dying"])
	text = "Alive %d of %d" % (total - dead - dying, total)
	if dying:
		text += ", %d dying" % dying
	if dead:
		text += ", %d dead" % dead
	return text

def WaitingText ():
	"""the prompts waiting for this player: the open one and the ones in line behind it"""
	prompt = GemRB.GetPrompt ()
	if not prompt:
		return ""
	return "%d waiting" % (1 + prompt.get ("Queued", 0))

def _ShowTile (button, info):
	if info["Dead"]:
		background = DeadColor
	elif info["Dying"]:
		background = DyingColor if Ticks % 2 == 0 else TileColor # it flashes
	elif info["Selected"]:
		background = SelectedColor
	else:
		background = TileColor
	button.SetBackground (background)
	button.SetColor (DeadText if info["Dead"] else LiveText)
	button.SetText (info["Text"])
	button.SetTooltip (info["Tooltip"])
	w, h = Layout["TileW"], Layout["TileH"]
	# the owner's colour on the edge (co-op), the HP bar along the bottom
	owner = info["Owner"]
	if owner is not None:
		button.SetBorder (0, PLAYER_COLORS[owner % len (PLAYER_COLORS)], 1, 1, {'x': 0, 'y': 0, 'w': OWNER_W, 'h': h})
	else:
		button.EnableBorder (0, 0)
	bar = {'x': OWNER_W + 3, 'y': h - BAR_H - (2 if Layout["Hero"] else 1), 'w': w - OWNER_W - 6, 'h': BAR_H if Layout["Hero"] else 3}
	button.SetBorder (1, BarBack, 1, 1, bar)
	if info["Dead"] or info["MaxHP"] < 1 or info["HP"] < 1:
		button.EnableBorder (2, 0)
		return
	share = min (1.0, float (info["HP"]) / info["MaxHP"])
	colour = {'r': int (220 - 170 * share), 'g': int (60 + 150 * share), 'b': 40, 'a': 255} # red to green
	filled = dict (bar)
	filled['w'] = max (1, int (bar['w'] * share))
	button.SetBorder (2, colour, 1, 1, filled)

def _ShowPortrait (button, slot, info):
	"""the portrait tile's picture: the character's own figure until portraits can be picked (the funnel's paper
	doll, GemRB.DCCAvatarFrame: the body with its hair and beard, standing, facing the viewer), drawn once"""
	if Portraits.get (slot):
		return
	figure = None
	try:
		stats = GemRB.GetDCCStats (slot)
		colours = [GemRB.GetPlayerStat (slot, stat) & 0xff for stat in range (IE_METAL_COLOR, IE_HAIR_COLOR + 1)]
		figure = GemRB.DCCAvatarFrame (GemRB.GetPlayerStat (slot, IE_ANIMATION_ID), colours, stats["Hair"], stats["Beard"],
			1, 0, 0, 2, PORTRAIT_DROP) # 1: IE_ANI_AWAKE, facing south, frame 0, twice the size
	except Exception as error:
		print ("[Roster] no figure for %d: %s" % (slot, error))
	if figure:
		button.SetPictures (figure[0])
		Portraits[slot] = "figure"
	else:
		button.SetText (GemRB.GetPlayerName (slot, 0)[:1])
		Portraits[slot] = "placeholder"

PORTRAIT_DROP = 84 # the figure moved down (twice the size, centred): head and shoulders, about

def UpdateRoster ():
	global RosterWindow
	win = RosterWindow
	if not win:
		return
	if not win.GetControl (100):
		RosterWindow = None
		return
	if PartyKey () != Built:
		# the party changed (the funnel's end, a dismissal or recruit, the dead taken out, a load, a
		# co-op replica): tiles for the party as it is now
		Rebuild ()
		return
	owners = _Owners ()
	infos = []
	for i in range (len (Built)):
		slot = i + 1
		info = TileInfo (slot, owners)
		infos.append (info)
		Shown[slot] = info["Text"]
		Tiles[slot] = info
	for row, slot in enumerate (OnShow ()):
		info = Tiles[slot]
		_ShowTile (win.GetControl (row), info)
		if Layout["Hero"]:
			portrait = win.GetControl (PORTRAIT_ID + row)
			_ShowPortrait (portrait, slot, info)
			portrait.SetTooltip (info["Tooltip"])
	Header["Summary"] = Summary (infos)
	Header["Waiting"] = WaitingText ()
	# the summary on two lines: who is alive, then the dying and the dead
	win.GetControl (100).SetText (Header["Summary"].replace (", ", "\n", 1))
	win.GetControl (101).SetText (Header["Waiting"].replace (" ", "\n"))
	for n in range (1, GROUPS + 1):
		win.GetControl (110 + n).SetBackground (GroupSet if Groups.get (n) else GroupEmpty)
	if GraveyardWindow:
		UpdateGraveyard ()

###################################################
# the graveyard: each dead peasant with the cause and the place

GraveyardWindow = None
GraveLines = [] # what the page shows, for tests

def GraveLine (slot):
	"""eg. "Gristle, cordwainer: crossbow bolt, the wooded trail" """
	record = GemRB.GetDCCFunnel (slot)
	place = record["Funnel"]
	name = GemRB.GetPlayerName (slot, 0)
	occupation = OccupationName (record["Occupation"])
	who = "%s, %s" % (name, occupation) if occupation else name
	killer, cause = place.get ("Killer", ""), place.get ("Cause", "")
	if killer and cause:
		how = "%s (%s)" % (cause, killer)
	else:
		how = cause or killer or "unknown causes"
	where = place.get ("Place", "") or place.get ("DiedIn", "")
	return "%s: %s%s" % (who, how, ", " + where if where else "")

def Graves ():
	"""the funnel's dead in the party, in roster order"""
	graves = []
	for slot in range (1, _Size () + 1):
		place = GemRB.GetDCCFunnel (slot)["Funnel"]
		if place["StatusName"] == "dead" or (place["StatusName"] == "none" and _Dead (slot)):
			graves.append ((place["Order"] or slot, slot))
	return [slot for order, slot in sorted (graves)]

def GraveLinesAll ():
	"""the graveyard (M3.3): every funnel character who died, from the game's own record (in the party or
	not any more), plus a dead party member only its funnel record knows (a save from before M3.3); in
	roster order"""
	import FunnelChoose
	entries = []
	seen = set ()
	for grave in GemRB.GetDCCGraveyard ():
		seen.add ((grave["Order"], grave["DiedAt"]))
		occupation = OccupationName ({"Id": grave["Occupation"], "Sub": grave["Sub"]})
		entries.append ((grave["Order"], grave["DiedAt"], FunnelChoose.GraveLine (grave, occupation)))
	for slot in Graves ():
		place = GemRB.GetDCCFunnel (slot)["Funnel"]
		if (place["Order"], place["DiedAt"]) not in seen:
			entries.append ((place["Order"] or slot, place["DiedAt"], GraveLine (slot)))
	return [line for order, died, line in sorted (entries)]

def OpenGraveyard ():
	global GraveyardWindow
	if GraveyardWindow:
		CloseGraveyard ()
		return
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	w, h = 620, 380
	win = GemRB.CreateView (94, 255, {'x': (screen[0] - w) // 2, 'y': 60, 'w': w, 'h': h})
	win.AddAlias ("GRAVEWIN")
	win.SetBackground (WindowColor)
	win.CreateLabel (0x10000000, 10, 6, w - 20, 24, HeaderFont, "The graveyard", IE_FONT_ALIGN_CENTER | IE_FONT_ALIGN_MIDDLE)
	text = win.CreateTextArea (1, 10, 36, w - 20, h - 80, Font)
	text.SetBackground ({'r': 40, 'g': 36, 'b': 32, 'a': 255})
	text.SetColor (LiveText, TA_COLOR_NORMAL)
	close = _Button (win, 2, w // 2 - 60, h - 38, 120, 28, "Close", CloseGraveyard, None, HeaderFont)
	close.MakeEscape ()
	GraveyardWindow = win
	UpdateGraveyard ()
	win.Focus ()
	return win

def UpdateGraveyard ():
	global GraveLines, GraveyardWindow
	lines = GraveLinesAll ()
	if not GraveyardWindow:
		GraveLines = lines
		return
	try:
		control = GraveyardWindow.GetControl (1)
	except Exception:
		control = None
	if not control:
		GraveyardWindow = None
		return
	if lines != GraveLines or not control.QueryText ():
		control.SetText ("\n".join (lines) if lines else "No one has died. Yet.")
	GraveLines = lines

def CloseGraveyard ():
	global GraveyardWindow
	if GraveyardWindow:
		try:
			GraveyardWindow.Close ()
		except RuntimeError:
			pass
		GraveyardWindow = None
