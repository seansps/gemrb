# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# M3.4: the record sheet's occupation skill lines (GUIScripts/ethren/DCCSkills.py) with ethren's shipped tables,
# outside the engine: python3 gemrb/tests/GUIScripts/test_dccskills.py (ctest runs it as Test_DCCSkills).

import os
import sys
import unittest

HERE = os.path.dirname (os.path.abspath (__file__))
GEMRB = os.path.normpath (os.path.join (HERE, "..", ".."))
sys.path.insert (0, os.path.join (GEMRB, "GUIScripts", "ethren"))
import FunnelRoll # noqa: E402
import DCCSkills # noqa: E402

def ReadTable (name):
	with open (os.path.join (GEMRB, "unhardcoded", "ethren", name + ".2da")) as f:
		return FunnelRoll.ParseTwoDA (f.read ())

TABLES = dict ((name, ReadTable (name)) for name in ("dccskill", "dccoccup"))

class DCCSkillsTest (unittest.TestCase):
	def test_labels (self):
		skills = TABLES["dccskill"]
		self.assertEqual (len (skills.order), 30)
		self.assertEqual (skills.order[0], "ANIMALS")
		self.assertEqual (skills.order[-1], "COOKING")
		self.assertEqual (DCCSkills.Label (skills, "CRAFT_FINE"), "Craft (fine)")
		self.assertEqual (DCCSkills.Uses (skills, "SMITHING"), "metal, weapons, armor quality")

	def test_familiar (self):
		skills = TABLES["dccskill"]
		# a blacksmith knows something of mining, locks, the military, appraisal's neighbours...
		familiar = DCCSkills.Familiar (skills, ["SMITHING", "APPRAISAL"])
		for tag in ("MINING", "LOCKS", "MILITARY", "HAGGLE", "CRAFT_FINE"):
			self.assertIn (tag, familiar)
		self.assertNotIn ("SMITHING", familiar)
		self.assertNotIn ("FARMING", familiar)
		# every tag's familiar tags exist
		for tag in skills.order:
			for near in skills.Get (tag, "FAMILIAR").split ("|"):
				self.assertIn (near, skills.order, tag)

	def test_sheet_lines (self):
		record = {"Id": 8, "Tags": ["SMITHING", "APPRAISAL"], "Acquired": []}
		lines = DCCSkills.SheetLines (TABLES, record, 0)
		self.assertEqual (lines[0], "Occupation: Blacksmith")
		self.assertEqual (lines[1], "Trained (d20): Smithing, Appraisal")
		self.assertEqual (lines[2], "  Smithing: metal, weapons, armor quality")
		self.assertTrue (lines[4].startswith ("Some familiarity (d10+2): "))
		self.assertEqual (lines[-1], "Anything else: d10")
		# past level 1 the occupation is a former one, the tags stay
		lines = DCCSkills.SheetLines (TABLES, dict (record, Acquired = ["HERBALISM"], Tags = ["SMITHING", "APPRAISAL", "HERBALISM"]), 1)
		self.assertEqual (lines[0], "Former occupation: Blacksmith")
		self.assertIn ("Acquired: Herbalism", lines)
		# no occupation and no tags: nothing to show
		self.assertEqual (DCCSkills.SheetLines (TABLES, {"Id": 0, "Tags": []}, 1), [])
		self.assertEqual (DCCSkills.Compact ({"Tags": ["SMITHING", "APPRAISAL"]}), "tags SMITHING,APPRAISAL")

if __name__ == "__main__":
	unittest.main ()
