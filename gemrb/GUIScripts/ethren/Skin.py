# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren: the UI skin (docs/architecture/ui-design.md, "Look and build"; style A, iron-bound oak, picked 2026-11-04).
# The art is the game's, not the engine's: art/ui/kit.py renders it in Blender, tools/ui_render packs it, the iefmt
# build copies it into the game's override (game/src/ui/skin.json lists the pieces):
#   ETC<h>          the party column's background for a screen h high (PNG)
#   ETB<w>, ETD<w>  the bar's, and the bar's in a dialog, w the screen's width less the column (PNG)
#   ETK<www><hh>    a w by h button: frames normal, hover, pressed, disabled, lit (BAM)
#   ETF<www>        a one-line tile w wide, ETH<www> a portrait tile's text plate: normal, selected, dying, dead (BAM)
#   ETPORT          the portrait's frame: normal, selected, dead (BAM)
#   ETSCRLR         the scroll bar for scroll bars made from code (SCRLBAR1, the CHU windows', has frames 4 and 5
#                   the other way round)
# and its fonts in fonts.2da (ETBUTTON, ETTITLE besides the stock names). A game without them (dcctest) keeps the
# plain colours: every call here says whether it skinned the view, and the callers fall back.
# Hover is a frame swap (the button's mouse enter and leave), lit (a toggle that is on: Pause, the formation in
# use, a saved group) another frame.

import GemRB
from GUIDefines import *
from ie_restype import RES_BAM, RES_PNG

BUTTON_FONT = "ETBUTTON"
TITLE_FONT = "ETTITLE"
SMALL_FONT = "ETSMALL" # the column's summary: two lines in its 30 px

INK = {'r': 236, 'g': 224, 'b': 196, 'a': 255}
INK_HOVER = {'r': 255, 'g': 240, 'b': 196, 'a': 255}
INK_LIT = {'r': 255, 'g': 228, 'b': 150, 'a': 255}
INK_OFF = {'r': 128, 'g': 120, 'b': 108, 'a': 255}
INK_DEAD = {'r': 140, 'g': 134, 'b': 124, 'a': 255}
INK_OPTION = {'r': 222, 'g': 176, 'b': 96, 'a': 255} # the dialog's replies in the log
CLEAR = {'r': 0, 'g': 0, 'b': 0, 'a': 0}

# the frames of a button BAM, and of a tile BAM
NORMAL, HOVER, PRESSED, DISABLED, LIT = range (5)
TILE_NORMAL, TILE_SELECTED, TILE_DYING, TILE_DEAD = range (4)
PORTRAIT_NORMAL, PORTRAIT_SELECTED, PORTRAIT_DEAD = range (3)

_Have = {}
Buttons = {} # key -> the skinned button's state (for tests too)

def Has (resref, restype):
	if (resref, restype) not in _Have:
		_Have[(resref, restype)] = bool (GemRB.HasResource (resref, restype, True))
	return _Have[(resref, restype)]

def On ():
	"""the skin is installed (the game's override has it)"""
	return Has ("ETK04630", RES_BAM)

def Font (name, fallback = "NORMAL"):
	"""one of the skin's own fonts, or the stock one in a game without the skin (its fonts.2da hasn't the name)"""
	return name if On () else fallback

def Background (view, resref):
	"""a PNG background for a window; False if the game hasn't it"""
	if not resref or not Has (resref, RES_PNG):
		return False
	view.SetBackground (CLEAR)
	view.SetBackground (resref)
	return True

def ColumnBackground (height):
	return "ETC%04d" % height

def BarBackground (width, dialog):
	return ("ETD%04d" if dialog else "ETB%04d") % width

def ButtonBam (w, h):
	name = "ETK%03d%02d" % (w, h)
	return name if Has (name, RES_BAM) else None

def Button (button, key, lit = None):
	"""skins a button with the BAM of its size (set its frame first); False (and nothing changed) if there's none.
	key names it for SetLit; lit None keeps what it had (the frame changed, eg. the bar in a dialog)"""
	frame = button.GetFrame ()
	bam = ButtonBam (frame['w'], frame['h'])
	if not bam:
		Buttons.pop (key, None)
		return False
	old = Buttons.get (key)
	state = {"Button": button, "Bam": bam, "Lit": bool (lit if lit is not None else (old and old["Lit"])), "Hover": False}
	Buttons[key] = state
	button.SetFlags (IE_GUI_BUTTON_NO_IMAGE, OP_NAND)
	button.SetBackground (CLEAR)
	button.OnMouseEnter (lambda: _Hover (key, True))
	button.OnMouseLeave (lambda: _Hover (key, False))
	_Apply (state)
	return True

def SetLit (key, lit):
	"""a toggle on or off (Pause, the formation, a saved group); False if that button isn't skinned"""
	state = Buttons.get (key)
	if not state:
		return False
	if state["Lit"] != bool (lit):
		state["Lit"] = bool (lit)
		_Apply (state)
	return True

def Frame (key):
	"""the frame a skinned button shows when not pressed (for tests)"""
	state = Buttons.get (key)
	return _Up (state) if state else None

def _Hover (key, on):
	state = Buttons.get (key)
	if state and state["Hover"] != on:
		state["Hover"] = on
		try:
			_Apply (state)
		except RuntimeError:
			Buttons.pop (key, None) # gone with its window

def _Apply (state):
	button = state["Button"]
	button.SetSprites (state["Bam"], 0, _Up (state), PRESSED, LIT, DISABLED)
	button.SetColor (INK_LIT if state["Lit"] else (INK_HOVER if state["Hover"] else INK))

def _Up (state):
	return LIT if state["Lit"] else (HOVER if state["Hover"] else NORMAL)

def TileBam (hero, w):
	name = ("ETH%03d" if hero else "ETF%03d") % w
	return name if Has (name, RES_BAM) else None

def PortraitBam ():
	return "ETPORT" if Has ("ETPORT", RES_BAM) else None

def ShowFrame (button, bam, frame):
	"""a tile's or portrait's look: the one frame, pressed or not"""
	button.SetSprites (bam, 0, frame, frame, frame, frame)

def ScrollBam ():
	return "ETSCRLR" if Has ("ETSCRLR", RES_BAM) else None
