# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# M3.1: the funnel's rolls (GUIScripts/ethren/FunnelRoll.py) with ethren's shipped tables, outside the
# engine: python3 gemrb/tests/GUIScripts/test_funnelroll.py (ctest runs it as Test_FunnelRoll).

import os
import random
import sys
import unittest

HERE = os.path.dirname (os.path.abspath (__file__))
GEMRB = os.path.normpath (os.path.join (HERE, "..", ".."))
sys.path.insert (0, os.path.join (GEMRB, "GUIScripts", "ethren"))
import FunnelRoll # noqa: E402

TAGS = ["ANIMALS", "FARMING", "HERBALISM", "MEDICINE", "ALCHEMY", "SMITHING", "MASONRY", "CARPENTRY", "MINING", "CRAFT_FINE",
	"APPRAISAL", "HAGGLE", "LOCKS", "STEALTH_STREET", "DECEPTION", "PERFORMANCE", "LORE_RELIGION", "LORE_ARCANE", "LORE_STARS",
	"LETTERS", "NOBILITY", "MILITARY", "OUTDOORS", "SEA", "CLIMB", "DIGGING", "FILTH", "BUTCHERY", "ROPE", "COOKING"]

def ReadTable (name):
	with open (os.path.join (GEMRB, "unhardcoded", "ethren", name + ".2da")) as f:
		return FunnelRoll.ParseTwoDA (f.read ())

TABLES = FunnelRoll.LoadTables (ReadTable)

class Scripted (object):
	"""dice in a given order; each must fit its die"""
	def __init__ (self, test, values):
		self.test = test
		self.values = list (values)
		self.asked = []

	def __call__ (self, sides):
		self.test.assertTrue (self.values, "ran out of dice at a d%d (asked %s)" % (sides, self.asked))
		value = self.values.pop (0)
		self.asked.append (sides)
		self.test.assertTrue (1 <= value <= sides, "%d on a d%d" % (value, sides))
		return value

def Seeded (seed):
	rng = random.Random (seed)
	return lambda sides: rng.randint (1, sides)

class FunnelRollTest (unittest.TestCase):
	def test_tables (self):
		occupations, items = TABLES["dccoccup"], TABLES["dccoccit"]
		covered = []
		for row in occupations.order:
			low, high = occupations.Int (row, "LOW"), occupations.Int (row, "HIGH")
			self.assertEqual (int (row), low)
			covered += range (low, high + 1)
			self.assertIn (occupations.Int (row, "RACE"), (1, 2, 4, 5))
			for column in ("WEAPON", "GOOD"):
				self.assertIn (occupations.Get (row, column), items.rows, row)
			tags = occupations.Get (row, "SKILLS").split ("|")
			self.assertTrue (1 <= len (tags) <= 4, row)
			self.assertEqual ([t for t in tags if t not in TAGS], [], row)
			self.assertIn (occupations.Get (row, "SUB"), (None, "FARMER", "ANIMAL", "CART"))
		self.assertEqual (covered, list (range (1, 101)))
		# Table 1-3's demihumans: 10% each
		races = [occupations.Int (FunnelRoll.FindOccupation (TABLES, d), "RACE") for d in range (1, 101)]
		self.assertEqual ([races.count (r) for r in (1, 2, 4, 5)], [70, 10, 10, 10])
		for key in items.order:
			ammo = items.Get (key, "AMMO")
			if ammo:
				self.assertIn (ammo, items.rows)
		self.assertEqual (FunnelRoll.ItemName (TABLES, "PITCHFORK", True), "Pitchfork (as spear)")
		self.assertEqual (FunnelRoll.ItemName (TABLES, "MONEY62"), "5 gp, 10 sp, 200 cp")
		self.assertEqual (items.Int ("MONEY62", "CP"), 800)

	def test_ability_mods (self):
		self.assertEqual ([FunnelRoll.AbilityMod (TABLES, s) for s in range (3, 19)],
			[-3, -2, -2, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3])

	def test_one_card (self):
		# sex (male), name, 3d6 x 6, d100 42 (a farmer), the crop d8 2 (wheat), the augur d30 25
		# (bountiful harvest), HP d4, 5d12 copper, alignment d3, looks: hair style, bald %, beard %,
		# beard style, skin, hair colour, two dyes, leather, metal
		names = [n for n in TABLES["dccnames"].order if TABLES["dccnames"].Int (n, "SEX") == 1]
		# beard style, skin, hair colour, two dyes, leather, metal; Table 3-4's item d24 14 (a lantern)
		dice = Scripted (self, [1, 1,  6, 6, 6,  1, 1, 2,  5, 5, 4,  3, 3, 3,  1, 1, 1,  6, 6, 5,  42, 2,  25,  1,  12, 12, 1, 1, 1,  3,
			1, 50, 20, 1, 3, 4, 1, 21, 2, 7,  14])
		card = FunnelRoll.RollCard (dice, TABLES, 1)
		self.assertEqual (dice.values, [])
		self.assertEqual (card["Name"], names[0].capitalize ())
		self.assertEqual (card["Sex"], 1)
		self.assertEqual (card["Scores"], [18, 4, 14, 9, 3, 17])
		self.assertEqual (card["Mods"], [3, -2, 1, 0, -3, 2])
		self.assertEqual ([card["D100"], card["Occupation"], card["Sub"], card["Race"]], [42, 39, 2, 1])
		self.assertEqual (card["OccupationName"], "Farmer (wheat)")
		self.assertEqual ([card["Weapon"], card["WeaponName"], card["Good"], card["GoodName"]], ["PITCHFORK", "Pitchfork (as spear)", "HEN", "Hen"])
		self.assertEqual (card["Skills"], ["FARMING", "ANIMALS", "OUTDOORS"])
		self.assertEqual ([card["WeaponItem"], card["GoodItem"]], ["", ""]) # no items in ethren yet
		# bountiful harvest adds the Luck modifier to the level-0 HP: 1 + 1 (Sta) + 2
		self.assertEqual ([card["Augur"], card["AugurName"], card["AugurMod"], card["HP"]], [25, "Bountiful harvest", 2, 4])
		self.assertEqual ([card["Copper"], card["Alignment"]], [27, 3])
		looks = card["Looks"]
		self.assertEqual ([looks["Animation"], looks["Hair"], looks["Beard"]], [0x6501, 1, 1])
		self.assertEqual (looks["Colors"], [6, 7, 27, 30, 47, 6, 39])
		lines = FunnelRoll.CardLines (card)
		self.assertEqual (lines[0], "Str 18 (+3)  Agi 4 (-2)  Sta 14 (+1)")
		self.assertEqual (lines[1], "Per 9 (0)  Int 3 (-3)  Luck 17 (+2)")
		self.assertEqual (lines[2], "Farmer (wheat)")
		self.assertEqual ([card["Equipment"], card["EquipmentName"], card["EquipmentItem"], card["EquipmentCount"]], [14, "Lantern", "", 1])
		self.assertEqual (lines[3], "Pitchfork (as spear); Hen; lantern")
		self.assertEqual (lines[4], "HP 4   27 cp   Chaotic male")
		self.assertEqual (lines[5], "Augur: Bountiful harvest (hit points) +2")

	def test_hp_at_least_one (self):
		# Sta 3 (-3) and a 1 on the d4
		# looks: hair style d6 4 (the female styles 8-13: long), bald d100 50, beard d100 (none for women), colours
		dice = Scripted (self, [2, 1,  3, 3, 3,  3, 3, 3,  1, 1, 1,  3, 3, 3,  3, 3, 3,  3, 3, 3,  1,  1,  1,  1, 1, 1, 1, 1,  2,
			4, 50, 1, 1, 1, 1, 1, 1, 1,  24])
		card = FunnelRoll.RollCard (dice, TABLES, 2)
		self.assertEqual ([card["Sex"], card["Scores"][2], card["Occupation"], card["HP"], card["Copper"]], [2, 3, 1, 1, 5])
		# a woman: the bald female body (CPF2) with a hair layer of her rig, no beard
		self.assertIn (6, dice.asked) # the hair style: one of six
		self.assertEqual ([card["Looks"]["Animation"], card["Looks"]["Hair"], card["Looks"]["Beard"]], [0x6511, 11, 0])

	def test_sub_rolls (self):
		die = Seeded (3)
		# a duplicate farmer gets an animal (d6) after its crop (d8)
		dice = Scripted (self, [44, 5, 3])
		occ = FunnelRoll.RollOccupation (dice, TABLES, [39])
		self.assertEqual ([occ["Occupation"], occ["Sub"], occ["Good"], occ["GoodName"], occ["OccupationName"]], [39, 5 | (3 << 4), "ANIMAL_3", "Cow", "Farmer (rice)"])
		# a herder: the dog unless there is one already
		self.assertEqual (FunnelRoll.RollOccupation (Scripted (self, [67]), TABLES, [])["GoodName"], "Herding dog")
		occ = FunnelRoll.RollOccupation (Scripted (self, [67, 6]), TABLES, [67])
		self.assertEqual ([occ["Sub"], occ["GoodName"]], [6, "Mule"])
		# the wainwright's pushcart
		occ = FunnelRoll.RollOccupation (Scripted (self, [95, 4]), TABLES, [])
		self.assertEqual ([occ["Sub"], occ["GoodName"]], [4, "Pushcart (your dead)"])
		# missile weapons come with 1d6 ammunition
		occ = FunnelRoll.RollOccupation (Scripted (self, [53, 5]), TABLES, [])
		self.assertEqual ([occ["WeaponName"], occ["Ammo"], occ["AmmoName"]], ["Sling", 5, "sling stones"])
		occ = FunnelRoll.RollOccupation (Scripted (self, [30, 2]), TABLES, [])
		self.assertEqual ([occ["WeaponName"], occ["Ammo"], occ["AmmoName"], occ["Race"]], ["Quill (as dart)", 2, "darts", 2])
		self.assertEqual (FunnelRoll.GearLine (occ), "Quill (as dart), 2 darts; Book")
		# money as a trade good
		occ = FunnelRoll.RollOccupation (Scripted (self, [62]), TABLES, [])
		self.assertEqual ([occ["GoodCopper"], occ["Race"]], [800, 5])
		self.assertTrue (die)

	def test_equipment_and_alignment (self):
		# M3.3: Table 3-4 (d24) and the player's choice of alignment
		equipment = TABLES["dccequip"]
		self.assertEqual (equipment.order, [str (n) for n in range (1, 25)])
		names = [FunnelRoll.RollEquipment (Scripted (self, [n]), TABLES)["EquipmentName"] for n in range (1, 25)]
		self.assertEqual ([names[0], names[11], names[13], names[22], names[23]], ["Backpack", "Holy water", "Lantern", "Torch", "Waterskin"])
		self.assertEqual (len (set (names)), 24)
		# without the table (an older game) the die is still rolled, and the card shows no item
		item = FunnelRoll.RollEquipment (Scripted (self, [3]), {})
		self.assertEqual ([item["Equipment"], item["EquipmentName"]], [3, ""])
		card = FunnelRoll.RollCard (Seeded (4), TABLES, 1)
		self.assertTrue (card["EquipmentName"])
		self.assertIn ("; " + card["EquipmentName"][0].lower (), FunnelRoll.GearLine (card))
		self.assertTrue (FunnelRoll.SetAlignment (card, 1))
		self.assertEqual (card["Alignment"], 1)
		self.assertIn ("Lawful", FunnelRoll.CardLines (card)[4])
		self.assertFalse (FunnelRoll.SetAlignment (card, 4))
		self.assertEqual (card["Alignment"], 1)
		self.assertEqual ([FunnelRoll.NextAlignment (code) for code in (1, 2, 3)], [2, 3, 1])

	def test_four_dice (self):
		dice = Scripted (self, [1, 6, 6, 6])
		self.assertEqual (FunnelRoll.RollAbility (dice, True), 18)
		dice = Scripted (self, [2, 2, 2, 1])
		self.assertEqual (FunnelRoll.RollAbility (dice, True), 6)

	def test_looks_by_sex (self):
		# S4: each hair and beard style is rendered on one rig only (art/_cache/sprites/.../engine_rows.md):
		# hair 1-7 and the beards on the male (CPM2, WQL..), hair 8-13 on the female (CPF2, WQF..). Every
		# species and sex only rolls the styles of its body's rig, each of them comes up, and a bald woman
		# keeps the bald female body
		hair = FunnelRoll.ParseTwoDA (open (os.path.join (GEMRB, "unhardcoded", "ethren", "dcchair.2da")).read ())
		kinds = {}
		for row in hair.order:
			kinds[(hair.Get (row, "KIND"), hair.Int (row, "ID"))] = row
		rigs = {0x6501: (set (range (1, 8)), {1, 2, 3}), 0x6511: (set (range (8, 14)), set ())}
		die = Seeded (11)
		seen = {0x6501: (set (), set ()), 0x6511: (set (), set ())}
		bald = {0x6501: 0, 0x6511: 0}
		for race in (1, 2, 4, 5):
			for sex in (1, 2):
				for i in range (400):
					looks = FunnelRoll.RollLooks (die, TABLES, race, sex)
					anim = looks["Animation"]
					self.assertEqual (anim, 0x6511 if sex == 2 else 0x6501)
					styles, beards = rigs[anim]
					if looks["Hair"]:
						self.assertIn (looks["Hair"], styles, (race, sex))
						self.assertIn (("HAIR", looks["Hair"]), kinds)
						seen[anim][0].add (looks["Hair"])
					else:
						bald[anim] += 1
					if looks["Beard"]:
						self.assertIn (looks["Beard"], beards, (race, sex))
						self.assertIn (("BEARD", looks["Beard"]), kinds)
						seen[anim][1].add (looks["Beard"])
		self.assertEqual (seen[0x6501], rigs[0x6501])
		self.assertEqual (seen[0x6511], rigs[0x6511])
		self.assertTrue (bald[0x6501] > 0 and bald[0x6511] > 0, bald)
		# the same dice, the same looks
		self.assertEqual (FunnelRoll.RollLooks (Seeded (4), TABLES, 1, 2), FunnelRoll.RollLooks (Seeded (4), TABLES, 1, 2))

	def test_sixteen (self):
		cards = FunnelRoll.RollAll (Seeded (1), TABLES)
		self.assertEqual ([c["Order"] for c in cards], list (range (1, 17)))
		self.assertEqual (len (set (c["Name"] for c in cards)), 16)
		occupations = TABLES["dccoccup"]
		for card in cards:
			self.assertTrue (all (3 <= s <= 18 for s in card["Scores"]))
			self.assertEqual (card["AugurMod"], card["Mods"][FunnelRoll.LUCK])
			self.assertTrue (1 <= card["HP"] <= 7)
			self.assertTrue (5 <= card["Copper"] <= 60)
			self.assertEqual (card["Race"], occupations.Int (card["Occupation"], "RACE"))
			self.assertTrue (1 <= card["Augur"] <= 30 and card["AugurName"])
			colors = card["Looks"]["Colors"]
			self.assertTrue (28 <= colors[3] <= 35 and 36 <= colors[6] <= 45 and 7 <= colors[1] <= 27)
			self.assertEqual (card["Looks"]["Animation"], 0x6511 if card["Sex"] == 2 else 0x6501)
			text = FunnelRoll.CardText (card)
			self.assertEqual (len (text.split ("\n")), 7)
			self.assertTrue (len (text.encode ("utf-8")) < 1024) # NetSetLobbyCards' limit
		# the same seed, the same sixteen; another seed, others
		self.assertEqual (FunnelRoll.RollAll (Seeded (1), TABLES), cards)
		self.assertNotEqual (FunnelRoll.RollAll (Seeded (2), TABLES), cards)
		# rerolling one keeps the others and their names out of its own
		again = [dict (c) for c in cards]
		card = FunnelRoll.Reroll (Seeded (5), TABLES, again, 3)
		self.assertEqual (card["Order"], 4)
		self.assertEqual (again[:3] + again[4:], cards[:3] + cards[4:])
		self.assertNotIn (card["Name"], [c["Name"] for c in again[:3] + again[4:]])

if __name__ == "__main__":
	unittest.main ()
