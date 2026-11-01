# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ethren (M3.4): occupation skills on the record sheet. A character's skill tags (its occupation's, plus any
# acquired) are trained: d20; tags next to them (dccskill.2da's FAMILIAR) give some familiarity: d10 + 2;
# anything else is d10 (docs/rules/funnel.md, "Ethren design proposal: occupation skills"). The engine rolls
# the checks (DCC/Skills.h); this only words the sheet's lines. No GemRB import at module level, like
# FunnelRoll.py, so it is tested outside the engine (gemrb/tests/GUIScripts/test_dccskills.py).

import FunnelRoll

TableCache = None

def Tables ():
	"""dccskill.2da and dccoccup.2da through the engine (cached)"""
	global TableCache
	if TableCache is None:
		TableCache = dict ((name, FunnelRoll.ReadGemRBTable (name)) for name in ("dccskill", "dccoccup"))
	return TableCache

def Label (skills, tag):
	"""a tag's name on the sheet: "Craft (fine)" for CRAFT_FINE"""
	return skills.Text (tag, "NAME", tag.replace ("_", " ").capitalize ())

def Uses (skills, tag):
	return skills.Text (tag, "USES")

def Familiar (skills, tags):
	"""the tags a character with these tags has some familiarity with (d10 + 2), in the table's order"""
	have = set (tags)
	familiar = []
	for tag in skills.order:
		if tag in have:
			continue
		near = (skills.Get (tag, "FAMILIAR") or "").split ("|")
		if have.intersection (near):
			familiar.append (tag)
	return familiar

def OccupationTitle (occupations, ident, level):
	"""'Occupation: Blacksmith', from level 1 'Former occupation: Blacksmith'"""
	if not ident:
		return ""
	name = occupations.Text (ident, "NAME", "occupation %d" % ident)
	return ("Former occupation: " if level >= 1 else "Occupation: ") + name

def SheetLines (tables, record, level):
	"""the record sheet's skill lines for a GetDCCFunnel ()["Occupation"] record"""
	skills = tables["dccskill"]
	tags = record.get ("Tags", record.get ("Skills", []))
	lines = []
	title = OccupationTitle (tables["dccoccup"], record.get ("Id", 0), level)
	if title:
		lines.append (title)
	if not tags:
		if lines:
			lines.append ("Skills: none, every check d10")
		return lines
	lines.append ("Trained (d20): " + ", ".join (Label (skills, tag) for tag in tags))
	for tag in tags:
		uses = Uses (skills, tag)
		if uses:
			lines.append ("  %s: %s" % (Label (skills, tag), uses))
	acquired = record.get ("Acquired", [])
	if acquired:
		lines.append ("Acquired: " + ", ".join (Label (skills, tag) for tag in acquired))
	familiar = Familiar (skills, tags)
	if familiar:
		lines.append ("Some familiarity (d10+2): " + ", ".join (Label (skills, tag) for tag in familiar))
	lines.append ("Anything else: d10")
	return lines

def Compact (record):
	"""one word for logs: 'tags SMITHING,APPRAISAL'"""
	return "tags " + (",".join (record.get ("Tags", [])) or "-")
