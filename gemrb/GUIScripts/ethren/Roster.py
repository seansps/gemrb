# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the funnel's roster panel (M3.2, docs/architecture/ui-design.md, "The funnel: sixteen at
# once"), in place of the portrait column the demo GUI doesn't have. Built from code (no CHU), in the
# demo's plain colours until Phase 5 skins it; it refreshes itself on a timer.
#   - the header: the party summary ("Alive 12 of 16, 2 dying, 2 dead"; a press opens the graveyard)
#     and the prompts waiting for this player ("3 waiting");
#   - Select all, Select mine (co-op), the formation buttons (Block, Column: set the party's formation
#     and line the selection up in it where its leader stands), the party settings (DCCParty.py);
#   - saved groups 1-9: Ctrl+1-9 (or a right click) saves the selection, Alt+1-9 (or a click) selects it;
#   - a tile for each party member (sixteen in the funnel: two columns of eight; the four after it: two
#     rows of two): the name, the occupation, the worst status, an HP bar,
#     the selection highlight, the owner's colour as a stripe down the left edge in co-op; a click selects one, a shift
#     click (or a right click) adds or takes one away; hovering shows occupation, HP, Luck and weapon.
#     The dying flash, the dead stay in the list, greyed and marked (dead).
# The tiles are rebuilt whenever the party changes (who is in which slot: the funnel's end, a dismissal,
# a recruit, the dead taken out, a load, a co-op replica's party), so the panel shows the current party
# only. A hidden tile would not do: hiding a view doesn't redraw the window under it, so the old tiles'
# pixels stayed (M3.3's "Alive 4 of 4" over sixteen tiles).
# Drag-box selection works in the world as before.

import GemRB
import FunnelRoll
from GUIDefines import *
from ie_stats import *

Font = "INFOFONT"
HeaderFont = "NORMAL"
WindowColor = {'r': 24, 'g': 22, 'b': 30, 'a': 230}
ButtonColor = {'r': 70, 'g': 60, 'b': 45, 'a': 255}
TileColor = {'r': 52, 'g': 46, 'b': 38, 'a': 255}
SelectedColor = {'r': 140, 'g': 120, 'b': 60, 'a': 255}
DeadColor = {'r': 34, 'g': 34, 'b': 34, 'a': 255}
DyingColor = {'r': 150, 'g': 30, 'b': 25, 'a': 255}
DeadText = {'r': 120, 'g': 120, 'b': 120, 'a': 255}
LiveText = {'r': 235, 'g': 228, 'b': 210, 'a': 255}
BarBack = {'r': 20, 'g': 20, 'b': 20, 'a': 255}
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
COLUMNS = 2
ROWS = 8
TILE_W = 150
TILE_H = 36
GAP = 4
BAR_H = 4
OWNER_W = 4 # the owner's colour: a stripe down the tile's left edge (co-op)
SUMMARY_Y = 4
BUTTONS_Y = 26
GROUPS_Y = 52
TILES_Y = 74
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

def PartyKey ():
	"""who is in the party, slot by slot (global IDs): the tiles are rebuilt when it changes"""
	return tuple (GemRB.GetDCCFunnel (slot)["GlobalID"] for slot in range (1, min (_Size (), MAX_SLOTS) + 1))

def _Rows (count):
	"""rows of tiles for a party this size: two columns, filled down then across"""
	return min (ROWS, max (1, (count + COLUMNS - 1) // COLUMNS))

def OpenRoster ():
	global RosterWindow, Built
	if RosterWindow:
		return RosterWindow
	Built = PartyKey ()
	count = len (Built)
	rows = _Rows (count)
	width = COLUMNS * TILE_W + (COLUMNS + 1) * GAP
	height = TILES_Y + rows * (TILE_H + GAP) + GAP
	screen = GemRB.GetSystemVariable (SV_WIDTH), GemRB.GetSystemVariable (SV_HEIGHT)
	frame = {'x': screen[0] - width, 'y': 4, 'w': width, 'h': height}
	win = GemRB.CreateView (95, 255, frame)
	win.AddAlias ("ROSTWIN")
	win.SetBackground (WindowColor)
	inner = width - 2 * GAP
	summary = _Button (win, 100, GAP, SUMMARY_Y, inner - 80, 20, "", OpenGraveyard, "The graveyard: who died, how and where", HeaderFont)
	summary.SetFlags (IE_GUI_BUTTON_ALIGN_LEFT, OP_OR)
	waiting = _Button (win, 101, GAP + inner - 76, SUMMARY_Y, 76, 20, "", None, "Prompts waiting for you")
	waiting.SetState (IE_GUI_BUTTON_LOCKED)
	# the buttons: everyone, mine (co-op), the formations, the party settings
	labels = [("All", SelectAll, "Select everyone"), ("Mine", SelectMine, "Select the characters you control"),
		("Block", lambda: Formation (0), "Formation: block (four wide). Lines the selection up where its first one stands"),
		("Column", lambda: Formation (1), "Formation: column of twos. Lines the selection up where its first one stands"),
		("Set", OpenSettings, "Party settings: Luck, spellburn, prompts, the party AI")]
	w = (inner - (len (labels) - 1) * GAP) // len (labels)
	for i, (text, action, tip) in enumerate (labels):
		_Button (win, 102 + i, GAP + i * (w + GAP), BUTTONS_Y, w, 22, text, action, tip)
	# the saved groups: Ctrl+N saves the selection, Alt+N selects it (a click selects, a right click saves)
	w = (inner - (GROUPS - 1) * 2) // GROUPS
	for n in range (1, GROUPS + 1):
		button = _Button (win, 110 + n, GAP + (n - 1) * (w + 2), GROUPS_Y, w, 18, str (n), lambda n = n: SelectGroup (n), None)
		button.OnRightPress (lambda n = n: SaveGroup (n))
		button.SetBackground (GroupEmpty)
		# hot keys are the button's own (one key each), so the saving keys get buttons of their own, hidden
		saver = win.CreateButton (130 + n, 0, 0, 0, 0)
		saver.OnPress (lambda n = n: SaveGroup (n))
		saver.SetHotKey (str (n), GEM_MOD_CTRL, True)
		button.SetHotKey (str (n), GEM_MOD_ALT, True)
	for i in range (count):
		column, row = divmod (i, rows)
		x = GAP + column * (TILE_W + GAP)
		y = TILES_Y + row * (TILE_H + GAP)
		tile = _Button (win, i, x, y, TILE_W, TILE_H, "", lambda slot = i + 1: SelectOnly (slot), None)
		tile.SetFlags (IE_GUI_BUTTON_ALIGN_LEFT | IE_GUI_BUTTON_ALIGN_TOP | IE_GUI_BUTTON_ALIGN_ANCHOR, OP_OR)
		tile.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND) # its borders (the owner, the HP bar) need it off
		tile.SetAnchor (OWNER_W + 4, 3)
		tile.OnShiftPress (lambda slot = i + 1: ToggleSelect (slot))
		tile.OnRightPress (lambda slot = i + 1: ToggleSelect (slot))
		tile.OnDoublePress (lambda slot = i + 1: CenterOn (slot))
	RosterWindow = win
	_StartTimer ()
	UpdateRoster ()
	return win

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
	"""the tiles the window has (for tests: a tile beyond the party would be a stale one)"""
	if not RosterWindow:
		return 0
	return sum (1 for i in range (MAX_SLOTS) if RosterWindow.GetControl (i))

def _Button (win, id, x, y, w, h, text, action, tooltip = None, font = None):
	button = win.CreateButton (id, x, y, w, h)
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_OR)
	button.SetFont (font or Font)
	button.SetBackground (ButtonColor)
	button.SetText (text)
	if action:
		button.OnPress (action)
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

def SelectOnly (slot):
	if slot <= _Size ():
		GemRB.GameSelectPC (slot, 1, SELECT_REPLACE)
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
	text = "%s\n%s" % (first, occupation or "-")
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
	# the owner's colour on the edge (co-op), the HP bar along the bottom
	owner = info["Owner"]
	if owner is not None:
		button.SetBorder (0, PLAYER_COLORS[owner % len (PLAYER_COLORS)], 1, 1, {'x': 0, 'y': 0, 'w': OWNER_W, 'h': TILE_H})
	else:
		button.EnableBorder (0, 0)
	bar = {'x': OWNER_W + 3, 'y': TILE_H - BAR_H - 2, 'w': TILE_W - OWNER_W - 6, 'h': BAR_H}
	button.SetBorder (1, BarBack, 1, 1, bar)
	if info["Dead"] or info["MaxHP"] < 1 or info["HP"] < 1:
		button.EnableBorder (2, 0)
		return
	share = min (1.0, float (info["HP"]) / info["MaxHP"])
	colour = {'r': int (220 - 170 * share), 'g': int (60 + 150 * share), 'b': 40, 'a': 255} # red to green
	filled = dict (bar)
	filled['w'] = max (1, int (bar['w'] * share))
	button.SetBorder (2, colour, 1, 1, filled)

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
		button = win.GetControl (i)
		slot = i + 1
		info = TileInfo (slot, owners)
		infos.append (info)
		_ShowTile (button, info)
		Shown[slot] = info["Text"]
		Tiles[slot] = info
	Header["Summary"] = Summary (infos)
	Header["Waiting"] = WaitingText ()
	win.GetControl (100).SetText (Header["Summary"])
	win.GetControl (101).SetText (Header["Waiting"])
	win.GetControl (103).SetVisible (bool (GemRB.NetGetMode ())) # Select mine: co-op only
	formation = CurrentFormation ()
	for i in (0, 1):
		win.GetControl (104 + i).SetBackground (SelectedColor if formation == i else ButtonColor)
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
