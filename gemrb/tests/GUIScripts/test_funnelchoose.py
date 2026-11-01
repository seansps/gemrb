# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# M3.3: the end of the funnel's rules (GUIScripts/ethren/FunnelChoose.py) with ethren's shipped tables,
# outside the engine: python3 gemrb/tests/GUIScripts/test_funnelchoose.py (ctest runs it as
# Test_FunnelChoose).

import os
import sys
import unittest

HERE = os.path.dirname (os.path.abspath (__file__))
GEMRB = os.path.normpath (os.path.join (HERE, "..", ".."))
sys.path.insert (0, os.path.join (GEMRB, "GUIScripts", "ethren"))
import FunnelRoll # noqa: E402
import FunnelChoose # noqa: E402

def ReadTable (name):
	with open (os.path.join (GEMRB, "unhardcoded", "ethren", name + ".2da")) as f:
		return FunnelRoll.ParseTwoDA (f.read ())

TABLES = dict ((name, ReadTable (name)) for name in ("dccoccup", "dccoccit", "dccclass", "dccgods"))

def Card (ident, order, scores, hp = 3, kills = 0, near = 0, race = 1, alignment = 2):
	return {"Id": ident, "Order": order, "Name": "P%d" % order, "Scores": scores, "Mods": [0] * 6, "HP": hp, "MaxHP": hp,
		"Kills": kills, "NearDeaths": near, "Occupation": "farmer", "Race": race, "Alignment": alignment, "Player": -1,
		"Level": 0, "Class": 8, "God": 0, "GodName": "", "Dead": False}

SIX = [
	Card (1001, 1, [10, 10, 10, 10, 10, 10], hp = 2),
	Card (1002, 2, [15, 8, 12, 9, 13, 7], hp = 5, kills = 2),
	Card (1003, 3, [9, 16, 11, 14, 8, 15], hp = 4, near = 3),
	Card (1004, 4, [12, 12, 17, 6, 11, 12], hp = 7),
	Card (1005, 5, [18, 5, 9, 12, 16, 9], hp = 1, kills = 1, near = 1),
	Card (1006, 6, [7, 13, 13, 17, 9, 18], hp = 3),
]

class FunnelChooseTest (unittest.TestCase):
	def test_sorting (self):
		order = lambda key, **kw: [c["Order"] for c in FunnelChoose.Sorted (SIX, key, **kw)]
		self.assertEqual (order ("Order"), [1, 2, 3, 4, 5, 6])
		self.assertEqual (order ("Str"), [5, 2, 4, 1, 3, 6])
		self.assertEqual (order ("Agi"), [3, 6, 4, 1, 2, 5])
		self.assertEqual (order ("Luck"), [6, 3, 4, 1, 5, 2])
		self.assertEqual (order ("HP"), [4, 2, 3, 6, 1, 5])
		self.assertEqual (order ("Kills"), [2, 5, 1, 3, 4, 6]) # ties stay in roster order
		self.assertEqual (order ("NearDeaths"), [3, 5, 1, 2, 4, 6])
		self.assertEqual (order ("Str", descending = False), [6, 3, 1, 4, 2, 5])
		self.assertEqual ([key for key, label in FunnelChoose.SORTS][:8], ["Order", "Str", "Agi", "Sta", "Per", "Int", "Luck", "HP"])

	def test_picking (self):
		picks = []
		for ident in (1002, 1004, 1006, 1001):
			picks = FunnelChoose.Toggle (picks, ident, SIX)
		self.assertEqual (picks, [1002, 1004, 1006, 1001])
		self.assertTrue (FunnelChoose.Ready (picks, SIX))
		# a fifth doesn't fit; one picked again is taken away
		self.assertEqual (FunnelChoose.Toggle (picks, 1003, SIX), picks)
		picks = FunnelChoose.Toggle (picks, 1004, SIX)
		self.assertEqual (picks, [1002, 1006, 1001])
		# fewer than four may go on (the rest wait in the reserves); none may not
		self.assertTrue (FunnelChoose.Ready (picks, SIX))
		self.assertTrue (FunnelChoose.Ready ([1003], SIX))
		self.assertFalse (FunnelChoose.Ready ([], SIX))
		self.assertFalse (FunnelChoose.Ready ([1003, 1003], SIX))
		self.assertFalse (FunnelChoose.Ready ([9999], SIX))
		self.assertEqual (FunnelChoose.Toggle ([], 9999, SIX), [])
		# two survivors: at most two
		two = SIX[:2]
		self.assertEqual (FunnelChoose.Limit (two), 2)
		self.assertEqual (FunnelChoose.Toggle ([1001, 1002], 1003, two), [1001, 1002])
		self.assertEqual (FunnelChoose.Limit ([]), 0)
		self.assertFalse (FunnelChoose.Ready ([], []))

	def test_card_lines (self):
		card = dict (SIX[1], Mods = [1, -1, 0, 0, 1, -1], Race = 4, Occupation = "dwarven miner", Alignment = 1)
		lines = FunnelChoose.CardLines (card)
		self.assertEqual (lines[0], "Str 15 (+1)  Agi 8 (-1)  Sta 12 (0)")
		self.assertEqual (lines[1], "Per 9 (0)  Int 13 (+1)  Luck 7 (-1)")
		self.assertEqual (lines[2], "dwarven miner (dwarf)")
		self.assertEqual (lines[3], "HP 5/5   Lawful   Level 0")
		self.assertEqual (lines[4], "Kills 2   Near deaths 0")
		cleric = dict (card, Level = 1, Class = 3, GodName = "Lom")
		self.assertEqual (FunnelChoose.ClassLine (cleric), "Cleric 1, of Lom")
		self.assertEqual (FunnelChoose.OccupationName (TABLES, 39, 2), "farmer (wheat)")
		self.assertEqual (FunnelChoose.OccupationName (TABLES, 39, 2, True), "farmer")
		self.assertEqual (FunnelChoose.OccupationName (TABLES, 0), "")

	def test_classes_and_gods (self):
		classes, gods = TABLES["dccclass"], TABLES["dccgods"]
		self.assertEqual ([name for ident, name in FunnelChoose.ClassesFor (classes, 1)], ["Wizard", "Warrior", "Cleric", "Thief"])
		self.assertEqual (FunnelChoose.ClassesFor (classes, 4), [(5, "Dwarf")])
		self.assertEqual (FunnelChoose.ClassesFor (classes, 2), [(6, "Elf")])
		self.assertEqual (FunnelChoose.ClassesFor (classes, 5), [(7, "Halfling")])
		self.assertIn ("hit die d8", FunnelChoose.ClassInfo (classes, 3))
		self.assertIn ("deed die", FunnelChoose.ClassInfo (classes, 2))
		# the Estaronan pantheon, by Ethren's alignments
		lawful = FunnelChoose.GodsFor (gods, 1)
		self.assertEqual (len (lawful), 6)
		self.assertEqual (lawful[0], (1, "Lom"))
		self.assertEqual ([name for ident, name in FunnelChoose.GodsFor (gods, 2)], ["Maelor", "Aigeya", "Turil", "Mascref", "Hudrey"])
		self.assertEqual (FunnelChoose.GodsFor (gods, 3)[-1], (17, "Morgen Windwalker"))
		self.assertEqual (len (FunnelChoose.GodsFor (gods, 3)), 6)
		# the domain, for the class pick's hover and info line
		self.assertEqual (FunnelChoose.GodDomain (gods, 14), "death, dark magic")
		self.assertEqual (FunnelChoose.GodLine (gods, 14), "Wolaeth, god of death, dark magic")
		self.assertEqual (FunnelChoose.GodDomain (gods, 0), "")
		self.assertEqual (FunnelChoose.GodLine (gods, 99), "")
		# the god follows the alignment
		self.assertEqual (FunnelChoose.GodFollows (gods, 1, 1), 1)
		self.assertEqual (FunnelChoose.GodFollows (gods, 1, 3), 0)
		# a valid pick: a class of the race; a cleric needs a god of its alignment
		self.assertTrue (FunnelChoose.PickValid (classes, gods, 1, 1, 3, 0))
		self.assertFalse (FunnelChoose.PickValid (classes, gods, 1, 5, 2, 0)) # a human dwarf
		self.assertFalse (FunnelChoose.PickValid (classes, gods, 4, 2, 2, 0)) # a dwarf warrior
		self.assertFalse (FunnelChoose.PickValid (classes, gods, 1, 3, 1, 0)) # a cleric without a god
		self.assertFalse (FunnelChoose.PickValid (classes, gods, 1, 3, 1, 14)) # a lawful cleric of Wolaeth
		self.assertTrue (FunnelChoose.PickValid (classes, gods, 1, 3, 3, 14))
		self.assertTrue (FunnelChoose.PickValid (classes, gods, 1, 3, 2, 9)) # Turil is neutral
		self.assertFalse (FunnelChoose.PickValid (classes, gods, 1, 1, 0, 0)) # no alignment

	def test_grave_lines (self):
		grave = {"Name": "Gristle", "Killer": "Marauder", "Cause": "crossbow bolt", "Place": "the wooded trail", "Level": 0, "Class": 8}
		self.assertEqual (FunnelChoose.GraveLine (grave, "cordwainer"), "Gristle, cordwainer: crossbow bolt (Marauder), the wooded trail")
		self.assertEqual (FunnelChoose.GraveLine (dict (grave, Killer = ""), ""), "Gristle: crossbow bolt, the wooded trail")
		self.assertEqual (FunnelChoose.GraveLine ({"Name": "Mira", "Cause": "bled out", "DiedIn": "fnt02"}, "wizard's apprentice"), "Mira, wizard's apprentice: bled out, fnt02")
		self.assertEqual (FunnelChoose.GraveLine ({"Name": "Odo"}, "woodcutter"), "Odo, woodcutter: unknown causes")

if __name__ == "__main__":
	unittest.main ()
