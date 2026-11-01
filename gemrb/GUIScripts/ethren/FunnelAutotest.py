# SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The funnel readiness checks (M2.9) of the funneltest autotest (testdata/funneltest,
# scripts/run.sh funneltest-auto). Nothing runs unless the game's FNT_AUTOTEST global is 1, which
# only the autotest variant of the FNT01 area script sets once the party of 16 has formed. Then a
# step a tick (500 ms):
#   1. the party: 16 level-0 peasants (MaxPartySize 16), each with its occupation (OCCU) and its
#      place in the funnel (FUNL); Player16 as a script object
#   2. the roster: 16 slots shown, select one, toggle, select all
#   3. a formation move of all 16 (GameControlMoveSelected): everyone arrives, none stuck, no two
#      on the same spot, everyone near the place it was given (M2.10); again in a column
#   4. withdrawal on a formation move and a scripted move (no action of its own): the giant rat's
#      free attack
#   5. the level-1 class picks with forced dice: a wizard (Table 5-8 rows without content
#      substituted, slots left open), an elf (its lucky spell through the learn window), a cleric
#      (the god's list), a low-Int wizard swapping one of two (the optional rule) and refused a second;
#      spells known enforced
#   6. the prompt load: level-0 saves vs death follow the policy without asking; the host's
#      level-0 option makes them ask; auto-resolve answers with the policy; two at once wait in
#      line, one window at a time; the party settings panel sets every character's policy
#   7. travel: the whole party of 16 to the cave (FNT02) and back
#   8. save and load: 16 party members with their OCCU/FUNL records (a death's time and place
#      included), spells and lucky spell; the protagonist's name
# M2.10 (funnel risks), after 4:
#   a. the DCC party AI (DCCPARTY): sixteen near two marauders: those in reach fight, nobody chases
#      or runs across the map; fighting back against an attacker; flee, passive and engage modes;
#      with the party AI off nobody moves on their own
#   b. encumbrance by body weight: everyone's weight rolled by race; light, heavy (half speed) and
#      over the cap (can't move); the record sheet's line
#   c. travel with 16 strung out in a single file (wider than MAX_TRAVELING_DISTANCE): one click
#      takes everyone to the cave; back with only one selected
# M3.1/M3.2 (Phase 3):
#   d. the roster (roster): tiles, summary, hover line, groups, select mine, the formation buttons; a fight
#      with deaths (a marauder kills two, a level-1 wizard bleeds out): the dying, the dead greyed, the
#      graveyard with each cause and place; screenshots
#   e. rolling the sixteen (rolling, after everything else): a new game through the rolling screen (the
#      cards, renaming, reroll all, the house rules, accept) and the sixteen it makes: abilities, Luck and
#      augur, HP, species, OCCU/FUNL, items, body weight, looks, the purse; screenshots
# M3.3:
#   f. the funnel's end (choose, in the rolled game): a little history (two killed, a kill, a near death),
#      the funnel ended by a script action, the selection screen (cards, the record, sorting, alignment,
#      four picks), the engine's end (the party of four, ten reserves off the map, level-1 XP for every
#      survivor, class picks pending, the dead out of the party and in the graveyard), the class picks
#      (a cleric with its god, a wizard's learn window), the end screen; screenshots
#   g. the reserves (reserves): a death among the four, the camp's roster, a swap and the newcomer's
#      class pick, a dismissal; save and load (the reserves, the graveyard, the party size)
# Every check logs "[FNT] ok ..." or "[FNT] FAIL ...", the end "[FNT] autotest done: N checks,
# F failed"; FNT_AT_DONE becomes 1 if all passed, 2 if not.

import GemRB
import DCCSheet
from GUIDefines import *
from ie_stats import *

STATE_DEAD = 0x800
SaveName = "funneltest"

Step = "wait"
Token = 0
Checks = 0
Failures = 0
Steps = None
Saved = {} # what the save should bring back

# the sixteen in roster order: script name, Table 1-3 occupation, race, the skill tags
PEASANTS = [
	("protagon", 39, 1, ["ANIMALS", "FARMING", "OUTDOORS"]),
	("fntmira", 97, 1, ["ALCHEMY", "LORE_ARCANE", "LETTERS"]),
	("fntlael", 37, 2, ["LORE_RELIGION", "LORE_ARCANE", "LETTERS"]),
	("fnttobb", 65, 1, ["HERBALISM", "MEDICINE", "LORE_RELIGION"]),
	("fntgorm", 23, 4, ["MASONRY", "MINING", "DIGGING"]),
	("fntpell", 58, 5, ["CRAFT_FINE", "HAGGLE"]),
	("fntbrin", 8, 1, ["SMITHING", "APPRAISAL"]),
	("fntodo", 98, 1, ["CARPENTRY", "OUTDOORS", "CLIMB"]),
	("fntsash", 53, 1, ["STEALTH_STREET", "DECEPTION"]),
	("fntwyn", 88, 1, ["MILITARY"]),
	("fnthild", 11, 1, ["HAGGLE", "COOKING"]),
	("fntrolf", 51, 1, ["LORE_RELIGION", "DIGGING"]),
	("fntessa", 84, 1, ["LORE_ARCANE", "LETTERS"]),
	("fntcorb", 89, 1, ["ANIMALS", "NOBILITY", "MILITARY"]),
	("fntmags", 66, 1, ["HERBALISM", "MEDICINE"]),
	("fntpip", 94, 1, ["STEALTH_STREET", "CLIMB"]),
]

def _Log (text):
	print ("[FNT] " + text)

def _Check (what, got, expected):
	global Checks, Failures
	Checks += 1
	if got == expected:
		_Log ("ok %s = %s" % (what, got))
	else:
		Failures += 1
		_Log ("FAIL %s = %s, expected %s" % (what, got, expected))

def _Slot (name):
	for pc in range (1, GemRB.GetPartySize () + 1):
		if GemRB.GetPlayerName (pc, 2).lower () == name:
			return pc
	return 0

def _Dead (pc):
	return bool (GemRB.GetPlayerStat (pc, IE_STATE_ID) & STATE_DEAD)

def _Luck (pc):
	return GemRB.GetDCCStats (pc)["Luck"]

def _Wait (done, most = 40):
	waited = 0
	while not done () and waited < most:
		waited += 1
		yield

def _Logged (text):
	return any (text in line for line in GemRB.GetDCCCombatLog ())

def _LogCount (text):
	return sum (1 for line in GemRB.GetDCCCombatLog () if text in line)

def _Selected ():
	return [pc for pc in range (1, GemRB.GetPartySize () + 1) if GemRB.GameIsPCSelected (pc)]

def _Positions (pcs):
	return [GemRB.GetActorPosition (pc) for pc in pcs]

def _Spread (where):
	"""the smallest distance between any two of the positions"""
	least = 10 ** 6
	for i in range (len (where)):
		for j in range (i + 1, len (where)):
			dx = where[i]["X"] - where[j]["X"]
			dy = where[i]["Y"] - where[j]["Y"]
			least = min (least, int ((dx * dx + dy * dy) ** 0.5))
	return least

def _Spells (pc):
	return sorted (spell["Spell"] for spell in GemRB.GetDCCMagic (pc)["Spells"])

# ---------------------------------------------------------------------------------------------

def _Party ():
	"""1: sixteen peasants with their occupations and funnel records"""
	size = GemRB.GetPartySize ()
	_Check ("MaxPartySize", GemRB.GetVar ("MaxPartySize"), 16)
	_Check ("party of 16", size, 16)
	_Check ("Player16 and Player16Fill as script objects", GemRB.GetGameVar ("FNT_P16"), 1)
	for order, (name, occupation, race, tags) in enumerate (PEASANTS, 1):
		pc = _Slot (name)
		_Check ("%s in slot %d" % (name, order), pc, order)
		if not pc:
			continue
		stats = GemRB.GetDCCStats (pc)
		funnel = GemRB.GetDCCFunnel (pc)
		occ, place = funnel["Occupation"], funnel["Funnel"]
		_Check ("%s level 0" % name, [stats["Class"], stats["Level"]], [8, 0])
		_Check ("%s occupation" % name, [occ["Id"], sorted (occ["Skills"]), GemRB.GetPlayerStat (pc, IE_RACE)], [occupation, sorted (tags), race])
		_Check ("%s funnel record" % name, [place["StatusName"], place["Order"], place["Player"]], ["alive", order, -1])
	farmer = GemRB.GetDCCFunnel (_Slot ("protagon"))["Occupation"]
	_Check ("the farmer's items and sub-roll", [farmer["Weapon"], farmer["TradeGood"], farmer["Sub"]], ["fntpfork", "fnthen", 2])
	_Check ("class picks pending", [GemRB.GetDCCFunnel (_Slot (n))["Funnel"]["ClassPending"] for n in ("fntmira", "fntlael", "fnttobb", "fntodo")], [1, 1, 1, 0])
	# the weapon a swing uses is the occupation's, the protagonist's too (made by CreatePlayer, its quick
	# weapon slot was the fists until the engine lined it up with the equipped weapon)
	unarmed = [(pc, _SwungItem (pc)) for pc in range (1, size + 1) if GemRB.GetDCCFunnel (pc)["Occupation"]["Weapon"] and _SwungItem (pc) != GemRB.GetDCCFunnel (pc)["Occupation"]["Weapon"]]
	_Check ("everyone swings its occupation's weapon", unarmed, [])
	_HeldStances ()
	yield

	# 2: the roster: sixteen slots; one, a toggle, everyone
	import Roster
	Roster.UpdateRoster ()
	_Check ("roster shows 16", len (Roster.Shown), 16)
	_Check ("roster names", Roster.Shown.get (16, "").split ("\n")[0], "Pip")
	Roster.SelectOnly (5)
	yield
	_Check ("select one", _Selected (), [5])
	Roster.ToggleSelect (9)
	_Check ("toggle adds", _Selected (), [5, 9])
	Roster.ToggleSelect (5)
	_Check ("toggle takes away", _Selected (), [9])
	Roster.SelectAll ()
	yield
	_Check ("select all", len (_Selected ()), 16)
	_Check ("the default formation is the block", Roster.CurrentFormation (), 0)

	# 3: a formation move of all sixteen, then in a column: everyone gets there, nobody stuck
	for formation, target in ((0, (1250, 900)), (1, (1450, 1050))):
		GemRB.GameSetFormation (formation, GemRB.GameGetFormation ())
		GemRB.GameControlMoveSelected (target[0], target[1])
		yield
		places = [(p["DestX"], p["DestY"]) for p in _Positions (range (1, 17))] # each one's formation place, before any bump
		yield from _Wait (lambda: not any (p["Moving"] for p in _Positions (range (1, 17))), 60)
		where = _Positions (range (1, 17))
		_Check ("formation %d: all 16 stopped" % formation, sum (1 for p in where if not p["Moving"]), 16)
		# each at its own place in the formation, none stuck on the way; bumping can leave the odd one a body
		# width or two off its place (stock pathfinding), which is fine
		off = [(pc, p["X"], p["Y"], p["DestX"], p["DestY"]) for pc, p in zip (range (1, 17), where) if abs (p["X"] - p["DestX"]) + abs (p["Y"] - p["DestY"]) > 160]
		_Check ("formation %d: all at their places" % formation, off, [])
		far = [pc for pc, p in zip (range (1, 17), where) if abs (p["X"] - target[0]) + abs (p["Y"] - target[1]) > 600]
		_Check ("formation %d: all near the goal" % formation, far, [])
		_Check ("formation %d: no two on one spot" % formation, _Spread (where) >= 8, True)
		_Log ("formation %d positions: %s" % (formation, " ".join ("%d,%d" % (p["X"], p["Y"]) for p in where)))
		offsets = sorted ((abs (p["X"] - place[0]) + abs (p["Y"] - place[1]) for p, place in zip (where, places)), reverse = True)
		_Log ("formation %d: off their formation places by %s px" % (formation, offsets[:4]))
		# M2.10: a walker a bump left short tries again once the crowd settles (DCC games), so
		# everyone ends within a body or two of the place it was given (stock: up to ~350 px)
		_Check ("formation %d: all within 80 px of the place given" % formation, offsets[0] <= 80, True)
	GemRB.GameSetFormation (0, GemRB.GameGetFormation ())

def _Withdrawal ():
	"""4: a peasant engaged with the rat walks off: a formation move, then a scripted move"""
	rat = GemRB.DCCFindActor ("fntrat")
	corb = _Slot ("fntcorb")
	for how in ("formation", "scripted"):
		GemRB.ClearActions (corb)
		GemRB.ExecuteString ("JumpToPoint([1180.1010])", corb)
		for i in range (3):
			yield
		GemRB.DCCForceDice (corb, [10, 1])
		GemRB.DCCAttack (corb, rat)
		GemRB.DCCForceDice (corb, [])
		before = _LogCount ("Giant rat gets a free attack: Corbin withdraws")
		GemRB.DCCForceDice (rat, [2, 1])
		if how == "formation":
			GemRB.GameSelectPC (corb, 1, SELECT_REPLACE)
			GemRB.GameControlMoveSelected (1180, 820)
		else:
			GemRB.DCCWalk (corb, 1180, 820)
		yield from _Wait (lambda: _LogCount ("Giant rat gets a free attack: Corbin withdraws") > before, 30)
		GemRB.DCCForceDice (rat, [])
		_Check ("withdrawal on a %s move" % how, _LogCount ("Giant rat gets a free attack: Corbin withdraws"), before + 1)
	_Check ("corbin alive", _Dead (corb), False)
	_Check ("level 0: the last weapon it fought with", GemRB.GetDCCFunnel (corb)["Occupation"]["LastWeapon"], "fntlswd")
	GemRB.GameSelectPC (0, 1)

def _Fighting (pc):
	"""whom it is attacking now (an attack action in reach), 0 none"""
	return GemRB.GetActorPosition (pc)["Attacking"]

def _Watch (pcs, ticks):
	"""watches for a while: {pc: whom it attacked meanwhile}"""
	seen = dict ((pc, set ()) for pc in pcs)
	for i in range (ticks):
		yield
		for pc in pcs:
			target = _Fighting (pc)
			if target:
				seen[pc].add (target)
	return seen

def _Moved (start, pcs):
	"""how far each one got from where it was"""
	where = _Positions (pcs)
	return dict ((pc, int (((p["X"] - start[pc][0]) ** 2 + (p["Y"] - start[pc][1]) ** 2) ** 0.5)) for pc, p in zip (pcs, where))

BLOCK = (1400, 850) # the AI checks' block of 16, well away from the rat

def _Block (pcs):
	"""everyone back to a 4 x 4 block, no orders; returns the spots"""
	spots = {}
	for i, pc in enumerate (pcs):
		column, row = i % 4, i // 4
		spots[pc] = (BLOCK[0] + column * 48, BLOCK[1] + row * 40)
		GemRB.ClearActions (pc)
		GemRB.ExecuteString ("JumpToPoint([%d.%d])" % spots[pc], pc)
	return spots

def _Put (actor, x, y):
	GemRB.ClearActions (actor)
	GemRB.ExecuteString ("JumpToPoint([%d.%d])" % (x, y), actor)

def _PartyAI ():
	"""a: the DCC party AI with sixteen near two marauders"""
	pcs = list (range (1, 17))
	_Check ("party AI script for everyone", sorted (set ((GemRB.GetPlayerScript (pc, 7) or "").lower () for pc in pcs)), ["dccparty"])
	_Check ("party AI: everyone defends by default", sorted (set (GemRB.GetDCCStats (pc)["PartyAI"] for pc in pcs)), [DCC_AI_DEFEND])
	wasOn = GemRB.GetGUIFlags () & GS_PARTYAI
	hitPoints = [GemRB.GetPlayerStat (pc, IE_HITPOINTS) for pc in pcs]
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	# the peasants roll 10s (no fumbles to hurt them, no crits), the marauders 2s (misses)
	for pc in pcs:
		GemRB.DCCForceDice (pc, [10] * 80)
	spots = _Block (pcs)
	near = GemRB.CreateCreature (1, "FNTGOB", BLOCK[0] + 3 * 48 + 56, BLOCK[1] + 40)
	far = GemRB.CreateCreature (1, "FNTGOB", BLOCK[0] - 300, BLOCK[1])
	for gob in (near, far):
		GemRB.DCCForceDice (gob, [2] * 200)
	_Put (near, BLOCK[0] + 3 * 48 + 56, BLOCK[1] + 40)
	_Put (far, BLOCK[0] - 300, BLOCK[1])
	reach = [4, 8, 12] # the east column's top three: the near marauder is a body's width away
	seen = yield from _Watch (pcs, 8)
	moved = _Moved (spots, pcs)
	fought = sorted (pc for pc in pcs if near in seen[pc])
	_Log ("AI: fought the near marauder: %s; moved %s" % (fought, moved))
	_Check ("AI: some in reach fight the near marauder", len (fought) > 0, True)
	_Check ("AI: only those in reach", [pc for pc in fought if pc not in reach], [])
	_Check ("AI: nobody goes for the far one", [pc for pc in pcs if far in seen[pc]], [])
	_Check ("AI: nobody leaves its place", [pc for pc in pcs if moved[pc] > 100], [])

	# fighting back: the far marauder comes and attacks Jora (slot 1), then is gone in a blink
	jora = 1
	GemRB.ExecuteString ("Attack(Player1)", far)
	yield from _Wait (lambda: _Fighting (jora) == far, 24)
	_Check ("AI: Jora fights back", _Fighting (jora), far)
	_Put (far, BLOCK[0] - 300, BLOCK[1])
	spots = _Block (pcs)
	seen = yield from _Watch (pcs, 6)
	moved = _Moved (spots, pcs)
	_Check ("AI: nobody chases the attacker across the map", [pc for pc in pcs if moved[pc] > 100 or far in seen[pc]], [])

	# passive: Odo (slot 8, in reach) leaves the marauder alone; flee: Rolf (slot 12) at 1 hp runs
	odo, rolf = 8, 12
	GemRB.DCCSetPartyAI (odo, DCC_AI_PASSIVE)
	GemRB.DCCSetPartyAI (rolf, DCC_AI_FLEE)
	spots = _Block (pcs)
	yield # ExecuteString puts its action in front: Rolf's own jump comes after the block's
	# Rolf below the block, where he has room to run, a marauder of his own next to him; then hurt
	spots[rolf] = (1430, 1060)
	hp = GemRB.GetPlayerStat (rolf, IE_HITPOINTS)
	GemRB.SetPlayerStat (rolf, IE_HITPOINTS, 1)
	_Put (rolf, spots[rolf][0], spots[rolf][1])
	_Put (far, 1478, 1026)
	yield
	gob = GemRB.GetActorPosition (far)
	_Log ("AI: Rolf at %s, the marauder at %d,%d" % (GemRB.GetActorPosition (rolf), gob["X"], gob["Y"]))
	before = ((spots[rolf][0] - gob["X"]) ** 2 + (spots[rolf][1] - gob["Y"]) ** 2) ** 0.5
	after = before
	seen = dict ((pc, set ()) for pc in pcs)
	for i in range (10):
		part = yield from _Watch (pcs, 1)
		for pc in pcs:
			seen[pc] |= part[pc]
		rolfAt = GemRB.GetActorPosition (rolf)
		after = max (after, ((rolfAt["X"] - gob["X"]) ** 2 + (rolfAt["Y"] - gob["Y"]) ** 2) ** 0.5)
	_Check ("AI: passive Odo stays out of it", sorted (seen[odo]), [])
	_Log ("AI: Rolf fled from %d to %d px" % (before, after))
	_Check ("AI: hurt Rolf flees", after > before + 60, True)
	_Check ("AI: Rolf didn't fight", far in seen[rolf], False)
	GemRB.SetPlayerStat (rolf, IE_HITPOINTS, hp)
	GemRB.ClearActions (rolf)

	# engage: Jora goes for an enemy close by (inside the leash), but not one further off
	GemRB.DCCSetPartyAI (jora, DCC_AI_ENGAGE)
	spots = _Block (pcs)
	_Put (far, BLOCK[0] - 170, BLOCK[1])
	seen = yield from _Watch ([jora], 8)
	_Check ("AI: engaging Jora goes for the one close by", far in seen[jora], True)
	_Check ("AI: on a short leash", _Moved (spots, [jora])[jora] < 220, True)
	GemRB.ClearActions (jora)
	spots = _Block (pcs)
	_Put (far, BLOCK[0] - 420, BLOCK[1])
	seen = yield from _Watch ([jora], 6)
	_Check ("AI: but not one further off", far in seen[jora], False)
	for pc in pcs:
		GemRB.DCCSetPartyAI (pc, DCC_AI_DEFEND)

	# the party AI off: nobody moves or fights on their own, even with a marauder in reach
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND)
	spots = _Block (pcs)
	_Put (near, BLOCK[0] + 3 * 48 + 56, BLOCK[1] + 40)
	seen = yield from _Watch (pcs, 6)
	moved = _Moved (spots, pcs)
	_Check ("AI off: nobody fights", [pc for pc in pcs if seen[pc]], [])
	_Check ("AI off: nobody moves", [pc for pc in pcs if moved[pc] > 10], [])

	# clean up: the marauders go, the dice are random again, the party AI as it was
	for gob in (near, far):
		GemRB.DCCForceDice (gob, [])
		GemRB.ExecuteString ("DestroySelf()", gob)
	for pc in pcs:
		GemRB.DCCForceDice (pc, [])
		GemRB.ClearActions (pc)
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR if wasOn else OP_NAND)
	_Check ("AI: nobody hurt", [pc for pc, hp in zip (pcs, hitPoints) if GemRB.GetPlayerStat (pc, IE_HITPOINTS) < hp], [])
	yield

def _Encumbrance ():
	"""b: encumbrance by body weight"""
	ranges = {1: (115, 160), 2: (84, 120), 4: (124, 160), 5: (43, 70)}
	brin = _Slot ("fntbrin")
	stats = GemRB.GetDCCStats (brin)
	_Check ("brin's body weight from his CRE", [stats["BodyWeight"], stats["BodyRolled"]], [120, 1])
	_Check ("brin's limits: a third and a half", [stats["LoadHalf"], stats["LoadCap"]], [39, 60])
	bad = []
	for pc in range (1, 17):
		if pc == brin:
			continue
		s = GemRB.GetDCCStats (pc)
		low, high = ranges[GemRB.GetPlayerStat (pc, IE_RACE)]
		if not s["BodyRolled"] or not low <= s["BodyWeight"] <= high:
			bad.append ((pc, s["BodyWeight"]))
	_Check ("everyone else's body weight rolled for the race", bad, [])
	GemRB.DCCForceDice (16, [1, 2, 3, 4, 5])
	_Check ("a forced roll: 110 + 5d10", GemRB.DCCSetBodyWeight (16), 125)
	GemRB.DCCForceDice (16, [])
	_Check ("the max encumbrance is the cap", GemRB.GetMaxEncumbrance (brin), 60)

	start, goal = (1250, 900), (1450, 1050)
	def walk ():
		_Put (brin, start[0], start[1])
		yield
		GemRB.DCCWalk (brin, goal[0], goal[1])
		yield
		yield
		yield
		where = GemRB.GetActorPosition (brin)
		GemRB.ClearActions (brin)
		return int (((where["X"] - start[0]) ** 2 + (where["Y"] - start[1]) ** 2) ** 0.5)

	carried = GemRB.GetDCCStats (brin)["Carried"]
	_Check ("light: Str 15 and a light load walk (no strmod.2da gap)", GemRB.GetDCCStats (brin)["LoadName"], "light")
	light = yield from walk ()
	speed = GemRB.GetDCCStats (brin)["WalkSpeed"]
	# stones up to just past a third of 120 lb: half speed
	stones = 0
	while carried + 20 * stones <= 39:
		GemRB.CreateItem (brin, "FNTSTONE")
		stones += 1
	stats = GemRB.GetDCCStats (brin)
	_Check ("heavy: past a third", [stats["Carried"], stats["LoadName"]], [carried + 20 * stones, "heavy" if carried + 20 * stones <= 60 else "over"])
	_Check ("heavy: half speed", stats["WalkSpeed"] >= 2 * speed - 1, True)
	heavy = yield from walk ()
	_Log ("encumbrance: %d px light, %d px heavy (carrying %d lb)" % (light, heavy, stats["Carried"]))
	_Check ("heavy: about half as far", light > 20 and 0.3 * light <= heavy <= 0.7 * light, True)
	sheet = __import__ ("DCCSheet").Lines (brin)
	_Check ("the record sheet shows the load", "Weight 120 lb   Load %d lb: half speed (half speed past 39, can't move past 60)" % stats["Carried"] in sheet, True)
	while GemRB.GetDCCStats (brin)["Carried"] <= 60:
		GemRB.CreateItem (brin, "FNTSTONE")
		stones += 1
	stats = GemRB.GetDCCStats (brin)
	_Check ("over the cap: can't move", [stats["LoadName"], stats["WalkSpeed"]], ["over", 0])
	over = yield from walk ()
	_Check ("over the cap: stays put", over <= 2, True)
	_Check ("the record sheet says so", any ("Load %d lb: can't move" % stats["Carried"] in line for line in __import__ ("DCCSheet").Lines (brin)), True)
	for i in range (stones):
		GemRB.ExecuteString ('DestroyItem("FNTSTONE")', brin)
	yield
	_Check ("unloaded: light again", [GemRB.GetDCCStats (brin)["Carried"], GemRB.GetDCCStats (brin)["LoadName"]], [carried, "light"])

def _LineTravel ():
	"""c: sixteen in a single file far longer than the gather distance travel at one click"""
	pcs = list (range (1, 17))
	for i, pc in enumerate (pcs):
		GemRB.ClearActions (pc)
		GemRB.ExecuteString ("JumpToPoint([%d.%d])" % (760 + i * 44, 520 + i * 30), pc)
	for i in range (3):
		yield
	where = _Positions (pcs)
	span = int (((where[0]["X"] - where[-1]["X"]) ** 2 + (where[0]["Y"] - where[-1]["Y"]) ** 2) ** 0.5)
	_Check ("line: wider than the gather distance (400)", span > 600, True)
	GemRB.GameSetFormation (2, GemRB.GameGetFormation ())
	GemRB.GameSelectPC (0, 1)
	GemRB.GameSelectPC (1, 1, SELECT_REPLACE)
	for pc in pcs:
		GemRB.GameSelectPC (pc, 1)
	_Check ("line: a click on ToCave", GemRB.GameControlTravel ("ToCave"), True)
	yield from _Wait (lambda: all (p["Area"].lower () == "fnt02" for p in _Positions (pcs)), 60)
	_Check ("line: all 16 in the cave", sorted (set (p["Area"].lower () for p in _Positions (pcs))), ["fnt02"])
	for i in range (4):
		yield
	# back with only Pip selected: the click brings everyone
	GemRB.GameSelectPC (0, 1)
	GemRB.GameSelectPC (16, 1, SELECT_REPLACE)
	_Check ("line: back, a click with one selected", GemRB.GameControlTravel ("ToGlade"), True)
	yield from _Wait (lambda: all (p["Area"].lower () == "fnt01" for p in _Positions (pcs)), 60)
	_Check ("line: all 16 back in the glade", sorted (set (p["Area"].lower () for p in _Positions (pcs))), ["fnt01"])
	GemRB.GameSetFormation (0, GemRB.GameGetFormation ())
	GemRB.GameSelectPC (0, 1)
	for i in range (2):
		yield

# --- M2.11: the peasant sprites in the engine -------------------------------------------------

IE_ANI_AWAKE, IE_ANI_READY, IE_ANI_TWITCH = 1, 7, 9
# script name -> (animation id, hair overlay, beard overlay) as funneltest's CREs give them (Rolf
# wears a cap: his hair is hidden, his beard isn't)
LOOKS = {
	"protagon": (0x6500, "", ""), "fntbrin": (0x6501, "HP", "BD"), "fntodo": (0x6501, "HP", ""),
	"fntpell": (0x6501, "", "BD"), "fntrolf": (0x6501, "", "BD"), "fntmira": (0x6510, "", ""),
}

def _Shot (name, center):
	"""the game window as GemRB draws it, centred on an actor, next to the autotest's saves"""
	path = GemRB.GetSystemVariable (SV_SAVEPATH) + "/sprites_%s.bmp" % name
	_Check ("screenshot %s" % name, GemRB.SaveScreenshot (path, center), True)

def _Stances (pcs):
	return [GemRB.GetDCCStats (pc)["Stance"] for pc in pcs]

def _Sprites ():
	"""M2.11: the Ethren peasants: looks, the relaxed and combat stances, walk speed, recolouring,
	the club overlay and its colour, a death; screenshots of each (sprites_*.bmp in the save path)"""
	pcs = list (range (1, 17))
	wasOn = GemRB.GetGUIFlags () & GS_PARTYAI
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND)
	spots = _Block (pcs)
	brin, gorm = _Slot ("fntbrin"), _Slot ("fntgorm")
	yield
	# a step south, so they face the camera
	for pc in pcs:
		GemRB.GameSelectPC (pc, 1)
	GemRB.GameControlMoveSelected (BLOCK[0] + 72, BLOCK[1] + 140)
	GemRB.GameSelectPC (0, 1)
	yield from _Wait (lambda: not any (stance == 10 for stance in _Stances (pcs)), 40)
	for name, (anim, hair, beard) in LOOKS.items ():
		pc = _Slot (name)
		stats = GemRB.GetDCCStats (pc)
		_Check ("looks: %s's animation, hair and beard" % name, [GemRB.GetPlayerStat (pc, IE_ANIMATION_ID), stats["HairOverlay"], stats["BeardOverlay"]], [anim, hair, beard])
	# relaxed: nobody fighting, everyone in the awake idle (G17) once the last fight is over
	yield from _Wait (lambda: _Stances (pcs) == [IE_ANI_AWAKE] * 16, 60)
	_Check ("stances: everyone relaxed (awake)", _Stances (pcs), [IE_ANI_AWAKE] * 16)
	_Shot ("idle", brin)
	# the looks side by side, facing the camera: CPM1 (baked in), hair and beard, hair, beard, a cap
	# and a beard, the female (sprites_looks.bmp; they stand 60 px apart, Brin second)
	row = [_Slot (name) for name in ("protagon", "fntbrin", "fntodo", "fntpell", "fntrolf", "fntmira")]
	for i, pc in enumerate (row):
		_Put (pc, BLOCK[0] - 330 + i * 60, BLOCK[1] + 60)
	yield
	for i, pc in enumerate (row):
		GemRB.DCCWalk (pc, BLOCK[0] - 330 + i * 60, BLOCK[1] + 140)
	yield
	yield from _Wait (lambda: not any (stance == 10 for stance in _Stances (row)), 40)
	_Shot ("looks", brin)

	# the walk: rate 9 (moverate.2da), 2 x 566 x 9 / 1500 = 6.8 px a tick straight across
	_Check ("walk: movement rate 9", GemRB.GetPlayerStat (brin, IE_MOVEMENTRATE), 9)
	GemRB.ClearActions (brin)
	here = GemRB.GetActorPosition (brin)
	GemRB.DCCWalk (brin, here["X"] - 300, here["Y"])
	yield
	start, t0 = GemRB.GetActorPosition (brin), GemRB.GetDCCStats (brin)["GameTicks"]
	yield
	yield
	end, t1 = GemRB.GetActorPosition (brin), GemRB.GetDCCStats (brin)["GameTicks"]
	speed = (start["X"] - end["X"]) / max (1, t1 - t0)
	_Log ("walk: %d px in %d ticks, %.2f px a tick" % (start["X"] - end["X"], t1 - t0, speed))
	_Check ("walk: 6-7.6 px a tick (the feet don't slide at 7)", 6.0 <= speed <= 7.6, True)
	spots = _Block (pcs)
	yield
	for pc in pcs:
		GemRB.GameSelectPC (pc, 1)
	GemRB.GameControlMoveSelected (BLOCK[0] - 260, BLOCK[1] + 60)
	yield
	_Shot ("walking", brin)
	yield from _Wait (lambda: not any (GemRB.GetDCCStats (pc)["Stance"] == 10 for pc in pcs), 40)
	GemRB.GameSelectPC (0, 1)

	# recolouring: Brin's major colour madder -> woad blue, his hair black -> white (the hair overlay too)
	before = [GemRB.GetPlayerStat (brin, stat, 1) for stat in (IE_MAJOR_COLOR, IE_HAIR_COLOR)]
	GemRB.SetPlayerStat (brin, IE_MAJOR_COLOR, 19)
	GemRB.SetPlayerStat (brin, IE_HAIR_COLOR, 45)
	yield
	_Check ("recolour: brin's colours", [GemRB.GetPlayerStat (brin, stat) & 0xff for stat in (IE_MAJOR_COLOR, IE_HAIR_COLOR)], [19, 45])
	yield
	_Shot ("recoloured", brin)

	# a fight: a marauder (the bald male body) at Brin and a female one at Gorm, clubs on both sides.
	# The club overlay takes byte 1 of the leather colour: the cudgel's equipping effect sets wood (51)
	_Check ("club: the weapon colour byte (wood)", (GemRB.GetPlayerStat (brin, IE_LEATHER_COLOR) >> 8) & 0xff, 51)
	spots = _Block (pcs)
	yield
	at = lambda pc, dx: (spots[pc][0] + dx, spots[pc][1])
	male = GemRB.CreateCreature (1, "FNTGOB", *at (brin, -50))
	female = GemRB.CreateCreature (1, "FNTGOBF", *at (gorm, -50))
	for gob, pc in ((male, brin), (female, gorm)):
		_Put (gob, *at (pc, -50))
		GemRB.DCCForceDice (gob, [2] * 100)
		GemRB.DCCForceDice (pc, [10] * 100)
	GemRB.ExecuteString ('Attack("fntbrin")', male)
	GemRB.ExecuteString ('Attack("fntgorm")', female)
	# the party AI (defend) has them fight back
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	yield from _Wait (lambda: GemRB.GetDCCStats (brin)["Engaged"], 30)
	_Check ("fight: brin engaged", GemRB.GetDCCStats (brin)["Engaged"], 1)
	# the rest of the party, not in reach, take the combat stance (G1) while the party fights
	gobs = [at (brin, -50), at (gorm, -50)]
	far = [pc for pc in pcs if min ((spots[pc][0] - x) ** 2 + (spots[pc][1] - y) ** 2 for x, y in gobs) > 100 ** 2]
	_Log ("fight: %d of the party out of reach: %s" % (len (far), far))
	yield from _Wait (lambda: all (stance == IE_ANI_READY for stance in _Stances (far)), 40)
	_Check ("stances: the party in the combat stance (ready) in a fight", sorted (set (_Stances (far))), [IE_ANI_READY])
	for i in range (3):
		yield
	_Shot ("combat", brin)

	# both marauders die: the die animation, then the twitch (lying)
	for gob in (male, female):
		GemRB.ApplyEffect (gob, "Death", 0, 0)
	yield from _Wait (lambda: [GemRB.GetDCCStats (gob)["Stance"] for gob in (male, female)] == [IE_ANI_TWITCH] * 2, 40)
	_Check ("death: both marauders lying (twitch)", [GemRB.GetDCCStats (gob)["Stance"] for gob in (male, female)], [IE_ANI_TWITCH] * 2)
	_Shot ("dead", brin)

	# as it was: colours, no forced dice, the party AI, no orders
	GemRB.SetPlayerStat (brin, IE_MAJOR_COLOR, before[0])
	GemRB.SetPlayerStat (brin, IE_HAIR_COLOR, before[1])
	for pc in pcs:
		GemRB.DCCForceDice (pc, [])
		GemRB.ClearActions (pc)
	if not wasOn:
		GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND)
	_Block (pcs)
	yield

def _ClassPicks ():
	"""5: the level-1 class picks: spells by class and level, substitutes, open slots, choices"""
	import DCCLearn
	mira, lael, tobb, pip = _Slot ("fntmira"), _Slot ("fntlael"), _Slot ("fnttobb"), _Slot ("fntpip")

	# the elf has to take the elf class
	_Check ("an elf can't pick wizard", GemRB.DCCLevelUp (lael, 1), -1)

	# wizard, Int 16: 4 + 1 spells. Rows 4 (chill touch: no content, substituted by the first of the
	# three wizard spells, flaming hands), 16 (magic missile), 23 (sleep), then 1 and 2 (no content,
	# nothing left to substitute: two open slots)
	GemRB.DCCForceDice (mira, [3, 4, 1, 16, 23, 1, 2])
	hp = GemRB.DCCLevelUp (mira, 1)
	GemRB.DCCForceDice (mira, [])
	magic = GemRB.GetDCCMagic (mira)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("wizard notes: " + notes)
	_Check ("wizard: level 1, d4 3", [hp, GemRB.GetDCCStats (mira)["Class"], GemRB.GetDCCStats (mira)["Level"]], [3, 1, 1])
	_Check ("wizard: spells known 5, 3 learned", [magic["Allowed"][0], magic["Counted"][0]], [5, 3])
	_Check ("wizard: the three with content", _Spells (mira), ["dwflam", "dwmmis", "dwslee"])
	_Check ("wizard: chill touch substituted", "Chill touch has no content yet; substituted dwflam" in notes, True)
	_Check ("wizard: two slots open", [notes.count ("the slot stays open"), "2 slot(s) stay open" in notes], [2, True])
	_Check ("wizard: mercurial magic on each", all (spell["Mercurial"] for spell in magic["Spells"]), True)
	_Check ("wizard: fresh until confirmed", magic["Fresh"], 3)
	_Check ("wizard: no class pick pending", GemRB.GetDCCFunnel (mira)["Funnel"]["ClassPending"], 0)
	_Check ("wizard can't learn a cleric spell", GemRB.DCCLearnSpell (mira, "dcbles"), -1)
	DCCLearn.OpenLearnWindow (mira)
	_Check ("learn window lists them", sorted (DCCLearn.Last["Spells"]), ["dwflam", "dwmmis", "dwslee"])
	DCCLearn.Confirm ()
	_Check ("wizard confirmed", GemRB.GetDCCMagic (mira)["Fresh"], 0)
	yield

	# elf, Int 13: 3 spells; invoke patron and patron bond come free but have no content. Row 15
	# (invoke patron: the pair, no content) is substituted (the second of three: magic missile),
	# then 13 (flaming hands) and 23 (sleep). The lucky spell defaults to the first, the window picks
	# sleep: Luck 16, +2, frozen
	GemRB.DCCForceDice (lael, [4, 15, 2, 13, 23])
	hp = GemRB.DCCLevelUp (lael, 6)
	GemRB.DCCForceDice (lael, [])
	magic = GemRB.GetDCCMagic (lael)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("elf notes: " + notes)
	_Check ("elf: level 1", [hp, GemRB.GetDCCStats (lael)["Class"]], [4, 6])
	_Check ("elf: 3 spells", [magic["Allowed"][0], _Spells (lael)], [3, ["dwflam", "dwmmis", "dwslee"]])
	_Check ("elf: the free patron spells have no content", "free Invoke patron has no content yet" in notes and "free Patron bond has no content yet" in notes, True)
	_Check ("elf: the default lucky spell", [s["Spell"] for s in magic["Spells"] if s["Lucky"]], ["dwmmis"])
	DCCLearn.OpenLearnWindow (lael)
	_Check ("elf's learn window offers the lucky pick", GemRB.GetDCCMagic (lael)["PicksLucky"], 1)
	DCCLearn.PickLucky ("dwslee")
	DCCLearn.Confirm ()
	magic = GemRB.GetDCCMagic (lael)
	_Check ("elf: sleep is lucky, +2 frozen", [(s["Spell"], s["LuckyMod"]) for s in magic["Spells"] if s["Lucky"]], [("dwslee", 2)])
	Saved["lucky"] = "dwslee"
	yield

	# cleric, Per 14: Table 1-5 gives 4 first-level spells; three have content, chosen (the
	# default: dice 2 of 3, paralysis, then 1 of 2, blessing, then word of command), one open
	GemRB.DCCForceDice (tobb, [5, 2, 1])
	hp = GemRB.DCCLevelUp (tobb, 3)
	GemRB.DCCForceDice (tobb, [])
	magic = GemRB.GetDCCMagic (tobb)
	notes = " | ".join (magic["LearnNotes"])
	_Log ("cleric notes: " + notes)
	_Check ("cleric: level 1", [hp, GemRB.GetDCCStats (tobb)["Class"]], [5, 3])
	_Check ("cleric: 4 allowed, 3 known", [magic["Allowed"][0], magic["Counted"][0]], [4, 3])
	_Check ("cleric: the god's list", _Spells (tobb), ["dcbles", "dcpara", "dcword"])
	_Check ("cleric: no mercurial magic", any (spell["Mercurial"] for spell in magic["Spells"]), False)
	_Check ("cleric: one open", "every spell with content is known; the slot stays open" in notes, True)
	_Check ("cleric: swap refused (knows it)", GemRB.DCCReplaceSpell (tobb, "dcpara", "dcbles"), False)
	_Check ("cleric can't learn a wizard spell", GemRB.DCCLearnSpell (tobb, "dwmmis"), -1)
	GemRB.DCCConfirmSpells (tobb)
	yield

	# a wizard with Int 5: 4 - 2 = 2 spells, rows 16 and 23; the optional rule swaps one of the two
	# (sleep for flaming hands) and refuses a second
	GemRB.DCCForceDice (pip, [2, 16, 23])
	GemRB.DCCLevelUp (pip, 1)
	GemRB.DCCForceDice (pip, [])
	magic = GemRB.GetDCCMagic (pip)
	_Check ("low-Int wizard: 2 spells", [magic["Allowed"][0], _Spells (pip)], [2, ["dwmmis", "dwslee"]])
	_Check ("low-Int wizard: one swap, flaming hands offered", [magic["Swaps"], [c["Spell"] for c in magic["Choices"][0]]], [1, ["dwflam"]])
	_Check ("can't learn a third", GemRB.DCCLearnSpell (pip, "dwflam"), -1)
	DCCLearn.OpenLearnWindow (pip)
	_Check ("swap sleep for flaming hands", DCCLearn.Swap ("dwslee", "dwflam"), True)
	_Check ("after the swap", _Spells (pip), ["dwflam", "dwmmis"])
	_Check ("a second swap refused", GemRB.DCCReplaceSpell (pip, "dwmmis", "dwslee"), False)
	_Check ("the chosen one rolled its own mercurial magic", [bool (s["Mercurial"]) and s["Replaced"] for s in GemRB.GetDCCMagic (pip)["Spells"] if s["Spell"] == "dwflam"], [True])
	DCCLearn.Confirm ()
	_Check ("pip confirmed", GemRB.GetDCCMagic (pip)["Fresh"], 0)
	Saved["spells"] = {"fntmira": _Spells (mira), "fntlael": _Spells (lael), "fnttobb": _Spells (tobb), "fntpip": _Spells (pip)}

def _Prompts ():
	"""6: the prompt load: level-0 policies, the host's options, prompts in line, the panel"""
	import DCCParty
	hild, rolf, odo, wyn, brin = _Slot ("fnthild"), _Slot ("fntrolf"), _Slot ("fntodo"), _Slot ("fntwyn"), _Slot ("fntbrin")
	_Check ("prompt options off", [GemRB.GetGameVar ("DCC_PROMPT_AUTO"), GemRB.GetGameVar ("DCC_PROMPT_LOUD0")], [0, 0])

	# Hilde (Luck 10, avoid death) fails a save vs death by 3: no prompt for a peasant, the policy burns 3
	GemRB.DCCForceDice (hild, [8])
	GemRB.ApplySpell (hild, "FNTSLAY")
	GemRB.DCCForceDice (hild, [])
	yield
	_Check ("level 0: no prompt", GemRB.GetPrompt (), None)
	_Check ("level 0: the policy burns 3 and survives", [_Dead (hild), _Luck (hild)], [False, 7])

	# Rolf's player set "never": he dies without a word; the funnel records where and when
	GemRB.DCCSetLuckPolicy (rolf, DCC_BURN_NEVER, 10, DCC_LUCK_PAUSE)
	GemRB.DCCForceDice (rolf, [3])
	GemRB.ApplySpell (rolf, "FNTSLAY")
	GemRB.DCCForceDice (rolf, [])
	yield from _Wait (lambda: _Dead (rolf), 10)
	place = GemRB.GetDCCFunnel (rolf)["Funnel"]
	_Check ("rolf dies in the funnel", [_Dead (rolf), place["StatusName"], place["DiedIn"], place["DiedAt"] > 0], [True, "dead", "fnt01", True])
	_Check ("no prompt for rolf", GemRB.GetPrompt (), None)
	Saved["dead"] = (rolf, place["DiedAt"])

	# the host lets peasants ask: Odo's failed save waits for his player
	_Check ("level-0 option on", GemRB.DCCSetPromptOptions (0, 1), True)
	GemRB.DCCForceDice (odo, [9])
	GemRB.ApplySpell (odo, "FNTSLAY")
	GemRB.DCCForceDice (odo, [])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("level 0 asks now", prompt and [prompt["Tag"], prompt["Actor"] == GemRB.DCCFindActor ("fntodo")], ["luckburn", True])
	if prompt:
		GemRB.AnswerPrompt (prompt["ID"], [2])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 10)
	_Check ("odo burns 2 and lives", [_Dead (odo), _Luck (odo)], [False, 8])

	# auto-resolve after 2 s; two fail at once: one window, the other waits in line, both answer
	# themselves with the policy (Wyn Luck 10 and Brin 8, both Sta +1: 9 vs 11, burn 2)
	_Check ("auto-resolve 2 s", GemRB.DCCSetPromptOptions (2, 1), True)
	GemRB.DCCForceDice (wyn, [8])
	GemRB.ApplySpell (wyn, "FNTSLAY")
	GemRB.DCCForceDice (brin, [8])
	GemRB.ApplySpell (brin, "FNTSLAY")
	GemRB.DCCForceDice (wyn, [])
	GemRB.DCCForceDice (brin, [])
	yield
	prompt = GemRB.GetPrompt ()
	_Check ("two at once: one open, one waiting", prompt and [prompt["Tag"], prompt["Queued"]], ["luckburn", 1])
	_Check ("auto-resolve caps the wait", prompt and prompt["Remaining"] <= 2000, True)
	yield from _Wait (lambda: GemRB.GetPrompt () is not None and GemRB.GetPrompt ()["Queued"] == 0, 12)
	second = GemRB.GetPrompt ()
	_Check ("then the second, alone", second and [second["Queued"], second["ID"] != prompt["ID"]], [0, True])
	yield from _Wait (lambda: GemRB.GetPrompt () is None, 12)
	_Check ("both answered by their policy", [_Dead (wyn), _Luck (wyn), _Dead (brin), _Luck (brin)], [False, 8, False, 6])
	_Check ("options off again", GemRB.DCCSetPromptOptions (0, 0), True)

	# the party settings: everyone's Luck policy and pause at once
	count = DCCParty.SetLuckPolicy (DCC_BURN_MISS_TO_HIT, False)
	_Check ("panel: all 16 set", count, 16)
	stats = [GemRB.GetDCCStats (pc) for pc in range (1, 17)]
	_Check ("panel: misses too, no pause", sorted (set ((s["LuckPolicy"], s["LuckFlags"] & DCC_LUCK_PAUSE) for s in stats)), [(DCC_BURN_MISS_TO_HIT, 0)])
	DCCParty.SetLuckPolicy (DCC_BURN_AVOID_DISASTER, True)
	_Check ("panel: back to avoid death, pausing", sorted (set ((s["LuckPolicy"], s["LuckFlags"] & DCC_LUCK_PAUSE) for s in (GemRB.GetDCCStats (pc) for pc in range (1, 17)))), [(DCC_BURN_AVOID_DISASTER, DCC_LUCK_PAUSE)])
	_Check ("panel: spellburn for the three wizards and the elf", DCCParty.SetSpellburn (0), 3)
	DCCParty.OpenPartySettings ()
	yield
	DCCParty.ClosePartySettings ()

def _Travel ():
	"""7: the whole party of 16 (the living) to the cave and back"""
	living = [pc for pc in range (1, 17) if not _Dead (pc)]
	for i, pc in enumerate (living):
		row, column = divmod (i, 4)
		GemRB.ClearActions (pc)
		GemRB.ExecuteString ("JumpToPoint([%d.%d])" % (560 + column * 40, 420 + row * 34), pc)
	for i in range (4):
		yield
	GemRB.GameSelectPC (0, 1)
	GemRB.GameSelectPC (living[0], 1, SELECT_REPLACE)
	_Check ("travel: a click on ToCave", GemRB.GameControlTravel ("ToCave"), True)
	yield from _Wait (lambda: GemRB.GetGameVar ("FNT_CAVE") == 1, 60)
	for i in range (4):
		yield
	where = _Positions (living)
	_Check ("travel: the whole party in the cave", sorted (set (p["Area"].lower () for p in where)), ["fnt02"])
	_Check ("travel: %d arrived apart" % len (living), _Spread (where) >= 8, True)
	_Check ("travel: Player16 there (the area script saw it)", GemRB.GetGameVar ("FNT_CAVE"), 1)
	_Log ("arrival: " + " ".join ("%d,%d" % (p["X"], p["Y"]) for p in where))

# --- M3.2: the roster ----------------------------------------------------------------------------

def _ShotScreen (name, center = 0):
	"""the whole screen (the GUI too), next to the autotest's saves"""
	path = GemRB.GetSystemVariable (SV_SAVEPATH) + "/%s.bmp" % name
	_Check ("screenshot %s" % name, GemRB.SaveScreenshot (path, center, 1), True)
	_Log ("screenshot: " + path)

def _ExpectedSummary ():
	dead = sum (1 for pc in range (1, 17) if _Dead (pc))
	dying = sum (1 for pc in range (1, 17) if not _Dead (pc) and GemRB.GetPlayerStat (pc, IE_DCC_BLEED) > 0)
	text = "Alive %d of 16" % (16 - dead - dying)
	return text + (", %d dying" % dying if dying else "") + (", %d dead" % dead if dead else "")

def _HeldStances ():
	"""a weapon held in both hands though one-handed in DCC (dcchold.2da; the autotest's copy lists
	FNTSPEAR): Wyn's spear takes the two-handed stances with her off hand free, its ITM's 100% thrust
	picking the two-handed jab (A6); the rules still see a one-handed weapon (d20 initiative). A club
	keeps the one-handed files"""
	wyn = _Slot ("fntwyn")
	stats = GemRB.GetDCCStats (wyn)
	files = stats["StanceFiles"]
	_Log ("held stances: Wyn %s %s %s" % (stats["WeaponAnim"], stats["AttackWeights"], files))
	_Check ("held: the spear in both hands (two-handed animation, ITM weights all thrust)", [stats["WeaponAnim"], stats["AttackWeights"]], [1, [0, 0, 100]])
	_Check ("held: ready is G13, the thrust A6 (body and overlay)", [files["Ready"][0][-3:].upper (), files["Ready"][1].upper (), files["Thrust"][0][-2:].upper (), files["Thrust"][1].upper ()], ["G13", "G1", "A6", "A6"])
	spear = GemRB.GetSlotItem (wyn, GemRB.GetCombatDetails (wyn, 0)["Slot"], 1)
	_Check ("held: the rules see a one-handed spear (no two-handed flag: d20 initiative, a shield allowed)", [spear["ItemResRef"].lower (), bool (spear["Flags"] & 0x200)], ["fntspear", False])
	# a one-handed weapon not in the table: the one-handed files
	club = [pc for pc in range (1, GemRB.GetPartySize () + 1) if _SwungItem (pc) == "fntclub"]
	if club:
		other = GemRB.GetDCCStats (club[0])
		_Check ("held: a club stays one-handed (G1, A1 A3 A5)", [other["WeaponAnim"], other["StanceFiles"]["Ready"][0][-2:].upper (), [other["StanceFiles"][s][0][-2:].upper () for s in ("Backhand", "Overhand", "Thrust")]], [0, "G1", ["A1", "A3", "A5"]])

def _RosterIsParty (what):
	"""the roster's tiles are the party as it is: one tile per member (no stale ones from a bigger
	party), in slot order, no name twice"""
	import Roster
	Roster.UpdateRoster ()
	size = GemRB.GetPartySize ()
	names = [GemRB.GetPlayerName (pc, 0) for pc in range (1, size + 1)]
	shown = [Roster.Shown.get (pc, "").split ("\n")[0].split ("  ")[0] for pc in range (1, size + 1)]
	_Check ("%s: the roster's tiles are the party (tiles, built for, names)" % what,
		[Roster.TileCount (), Roster.Built == tuple (_Id (pc) for pc in range (1, size + 1)), shown, len (set (shown))],
		[size, True, names, size])

def _Kill (attacker, victim):
	"""the marauder hits a peasant (a natural 20: the weak marauder needs it) until it is down"""
	for i in range (4):
		if _Dead (victim) or GemRB.GetPlayerStat (victim, IE_HITPOINTS) < 1:
			break
		GemRB.DCCForceDice (attacker, [20, 4, 1, 1, 1])
		GemRB.DCCAttack (attacker, victim)
		GemRB.DCCForceDice (attacker, [])
		yield

def _Roster ():
	"""d: the real roster (M3.2) with a fight, deaths and the graveyard"""
	import Roster
	essa, mags, mira, jora = _Slot ("fntessa"), _Slot ("fntmags"), _Slot ("fntmira"), _Slot ("protagon")
	area = GemRB.GetActorPosition (jora)["Area"].lower ()
	placeName = {"fnt01": "the glade", "fnt02": "the cave"}.get (area, area)
	Roster.UpdateRoster ()
	_Check ("roster: 16 tiles", len (Roster.Tiles), 16)
	_Check ("roster: name and occupation", Roster.Shown.get (jora, "").split ("\n"), [GemRB.GetPlayerName (jora, 0), "farmer"])
	tip = Roster.Tiles[jora]["Tooltip"]
	_Log ("roster: Jora's hover line: " + tip)
	held = GemRB.GetSlotItem (jora, GemRB.GetEquippedQuickSlot (jora))
	weapon = "pitchfork" if held and held["ItemResRef"].lower () == "fntpfork" else "bare hands"
	_Check ("roster: the hover line (occupation, HP, Luck, weapon in hand)", [w in tip for w in ("farmer (wheat)", "HP ", "Luck 11", weapon)], [True] * 4)
	_Check ("roster: the summary", Roster.Header["Summary"], _ExpectedSummary ())
	_Check ("roster: no owner colours in single player", Roster.Tiles[jora]["Owner"], None)
	_Check ("roster: select mine is everyone living in single player", len (Roster.SelectMine ()), sum (1 for pc in range (1, 17) if not _Dead (pc)))

	# saved groups: Ctrl+N (here its button) saves the selection, a click on N selects it again
	Roster.SelectOnly (jora)
	Roster.ToggleSelect (_Slot ("fntbrin"))
	Roster.SaveGroup (2)
	Roster.SelectAll ()
	_Check ("groups: 2 saved", Roster.Groups.get (2), [jora, _Slot ("fntbrin")])
	_Check ("groups: 2 selected again", [Roster.SelectGroup (2), _Selected ()], [True, [jora, _Slot ("fntbrin")]])
	_Check ("groups: an empty one selects nothing", Roster.SelectGroup (7), False)

	# the formation buttons act on the selection: the column, lined up where the first one stands
	living = [pc for pc in range (1, 17) if not _Dead (pc)]
	GemRB.GameSelectPC (0, 0)
	for pc in living[:6]:
		GemRB.GameSelectPC (pc, 1)
	lead = GemRB.GetActorPosition (living[0])
	Roster.Formation (1)
	yield
	_Check ("formation button: the column", Roster.CurrentFormation (), 1)
	_Check ("formation button: the selection sets off", any (p["Moving"] for p in _Positions (living[:6])), True)
	yield from _Wait (lambda: not any (p["Moving"] for p in _Positions (living[:6])), 30)
	where = _Positions (living[:6])
	_Log ("formation button: the column at %d,%d: %s" % (lead["X"], lead["Y"], " ".join ("%d,%d" % (p["X"], p["Y"]) for p in where)))
	_Check ("formation button: the six lined up where the first stood", [p for p in where if abs (p["X"] - lead["X"]) + abs (p["Y"] - lead["Y"]) > 220], [])
	Roster.Formation (0)
	_Check ("formation button: the block", Roster.CurrentFormation (), 0)
	GemRB.GameSelectPC (0, 1)

	# a fight: a marauder kills Essa and Mags; Mira (a level-1 wizard since the class picks) drops and bleeds
	jx, jy = GemRB.GetActorPosition (essa)["X"], GemRB.GetActorPosition (essa)["Y"]
	gob = GemRB.CreateCreature (1, "FNTGOB", jx + 40, jy)
	_Put (gob, jx + 40, jy)
	yield
	yield from _Kill (gob, essa)
	yield from _Kill (gob, mags)
	# Mira drops to 0 from a blow nobody struck (5 damage): she bleeds out alone
	GemRB.ApplyEffect (mira, "Damage", GemRB.GetPlayerStat (mira, IE_HITPOINTS) + 1, 0)
	yield
	Roster.UpdateRoster ()
	_Check ("fight: Essa and Mags dead", [_Dead (essa), _Dead (mags)], [True, True])
	_Check ("fight: Mira dying (bleeding out)", [_Dead (mira), Roster.Tiles[mira]["Status"], Roster.Tiles[mira]["Dying"]], [False, "dying", True])
	_Check ("roster: the dead greyed and marked, kept in the list", [Roster.Tiles[essa]["Dead"], Roster.Shown[essa].startswith ("Essa  " + Roster.DEAD_MARK)], [True, True])
	_Check ("roster: the summary mid-fight", Roster.Header["Summary"], _ExpectedSummary ())
	_Log ("roster mid-fight: " + Roster.Header["Summary"])
	_ShotScreen ("roster_fight", essa)
	for name, pc in (("essa", essa), ("mags", mags)):
		place = GemRB.GetDCCFunnel (pc)["Funnel"]
		_Check ("the cause of %s's death" % name, [place["StatusName"], place["Killer"], place["Cause"], place["Place"]], ["dead", "Marauder", "cudgel", placeName])
	Saved["essa"] = GemRB.GetDCCFunnel (essa)["Funnel"]

	# Mira bleeds out (level 1: by the end of the next round)
	GemRB.ExecuteString ("DestroySelf()", gob)
	yield from _Wait (lambda: _Dead (mira), 40)
	place = GemRB.GetDCCFunnel (mira)["Funnel"]
	_Check ("mira bled out", [_Dead (mira), place["StatusName"], place["Killer"], place["Cause"], place["Place"]], [True, "dead", "", "bled out", placeName])

	# the graveyard: each dead peasant, the cause and the place
	Roster.OpenGraveyard ()
	yield
	lines = Roster.GraveLines
	_Log ("graveyard: " + " | ".join (lines))
	expected = ["Essa, scribe: cudgel (Marauder), %s" % placeName, "Mira, wizard's apprentice: bled out, %s" % placeName, "Mags, herbalist: cudgel (Marauder), %s" % placeName]
	if _Dead (_Slot ("fntrolf")):
		expected.insert (0, "Rolf, grave digger: Slay, the glade")
	_Check ("graveyard lines", sorted (lines), sorted (expected))
	_ShotScreen ("roster_graveyard", essa)
	Roster.CloseGraveyard ()
	_Check ("graveyard closed", Roster.GraveyardWindow, None)
	Roster.UpdateRoster ()
	_Check ("roster: the summary after", Roster.Header["Summary"], _ExpectedSummary ())
	# the fight is over once the combat counter runs out (a save waits for it)
	for i in range (30):
		yield

def _SaveGames ():
	return [game for game in GemRB.GetSaveGames () if game.GetName () == SaveName]

def _Records ():
	"""the sixteen's OCCU and FUNL records (without the global IDs, which a load gives anew)"""
	records = []
	for pc in range (1, 17):
		record = GemRB.GetDCCFunnel (pc)
		records.append ({"Occupation": record["Occupation"], "Funnel": record["Funnel"], "Slot": record["Slot"]})
	return records

def _Save ():
	"""8: save 16 with their records (one marked reserve and dealt to player 2), then load"""
	last = _Slot ("fntpip")
	_Check ("pip to the reserve, dealt to player 2", GemRB.DCCSetFunnel (last, 4, -1, 2), True)
	Saved["funnel"] = _Records ()
	Saved["names"] = [GemRB.GetPlayerName (1, which) for which in (0, 1, 2)]
	_Log ("the protagonist's names: %s" % Saved["names"])
	for game in _SaveGames ():
		GemRB.DeleteSaveGame (game)
	_Check ("game saved", GemRB.SaveGame (None, SaveName), 0)

def _StepSave ():
	global Step
	games = _SaveGames ()
	_Check ("save written", len (games), 1)
	if not games:
		return "done"
	_Log ("loading the save '%s'" % SaveName)
	game = games[0]
	def Load ():
		GemRB.LoadGame (game)
		GemRB.EnterGame ()
	GemRB.QuitGame ()
	GemRB.SetTimer (Load, 0, 0)
	return "loading"

def _StepLoaded ():
	_Check ("loaded: party of 16", GemRB.GetPartySize (), 16)
	after = _Records ()
	_Check ("loaded: every OCCU and FUNL record", after, Saved.get ("funnel"))
	pip = GemRB.GetDCCFunnel (_Slot ("fntpip"))["Funnel"]
	_Check ("loaded: pip in the reserve for player 2", [pip["StatusName"], pip["Player"]], ["reserve", 2])
	rolf, diedAt = Saved.get ("dead", (0, 0))
	place = GemRB.GetDCCFunnel (rolf)["Funnel"]
	_Check ("loaded: rolf's death", [_Dead (rolf), place["StatusName"], place["DiedAt"], place["DiedIn"]], [True, "dead", diedAt, "fnt01"])
	_Check ("loaded: the spells", dict ((name, _Spells (_Slot (name))) for name in ("fntmira", "fntlael", "fnttobb", "fntpip")), Saved.get ("spells"))
	lael = GemRB.GetDCCMagic (_Slot ("fntlael"))
	_Check ("loaded: the elf's lucky spell", [(s["Spell"], s["LuckyMod"]) for s in lael["Spells"] if s["Lucky"]], [(Saved.get ("lucky"), 2)])
	_Check ("loaded: in the cave", GemRB.GetActorPosition (1)["Area"].lower (), "fnt02")
	_Check ("loaded: the protagonist's name as set (not the CRE's)", [GemRB.GetPlayerName (1, which) for which in (0, 1, 2)], Saved.get ("names"))
	_Check ("loaded: brin's body weight", GemRB.GetDCCStats (_Slot ("fntbrin"))["BodyWeight"], 120)
	if "essa" in Saved:
		place = GemRB.GetDCCFunnel (_Slot ("fntessa"))["Funnel"]
		_Check ("loaded: essa's cause of death", [place["Killer"], place["Cause"], place["Place"]], [Saved["essa"]["Killer"], Saved["essa"]["Cause"], Saved["essa"]["Place"]])
	import Roster
	Roster.UpdateRoster ()
	_Check ("loaded: the roster", len (Roster.Shown), 16)
	return _After ("save")

# Scenarios (scripts/run.sh funneltest-auto --scenario NAMES, default all): the engine sets AT_ONLY
# and AT_<NAME> for the ones picked. Each runs with the ones it needs before it.
SCENARIOS = (
	("party", _Party), # 1-3: the sixteen, the roster, formation moves
	("withdrawal", _Withdrawal), # 4
	("partyai", _PartyAI), # a
	("encumbrance", _Encumbrance), # b
	("linetravel", _LineTravel), # c
	("sprites", _Sprites), # M2.11: the peasant sprites, with screenshots
	("classpicks", _ClassPicks), # 5
	("prompts", _Prompts), # 6
	("travel", _Travel), # 7
	("roster", _Roster), # d (M3.2): the roster, a fight with deaths, the graveyard
	("save", None), # 8
	("rolling", None), # e (M3.1): a new game through the rolling screen
	("choose", None), # f (M3.3): the funnel's end in the rolled game: choose your four, the class picks, the end screen
	("reserves", None), # g (M3.3): the reserves at camp: a death, a swap, a dismissal, save and load
)
NEEDS = {
	"roster": ["classpicks"], # a level-1 character to bleed out
	"prompts": ["classpicks"], # the party settings panel's spellburn lines need the wizards
	# the load checks what these left: the spells, Rolf's death, the party in the cave
	"save": ["classpicks", "prompts", "travel"],
	# M3.3: the funnel's end happens in the game the rolling screen makes
	"choose": ["rolling"],
	"reserves": ["rolling", "choose"],
}
Picked = None

def _PickScenarios ():
	global Picked, Failures
	names = [name for name, run in SCENARIOS]
	if not GemRB.GetGameVar ("AT_ONLY"):
		Picked = set (names)
		return
	asked = [name for name in names if GemRB.GetGameVar ("AT_" + name.upper ())]
	if not asked:
		Failures += 1
		_Log ("FAIL no known scenario picked; they are: " + ", ".join (names))
	Picked = set (asked)
	for name in asked:
		Picked.update (NEEDS.get (name, []))
	_Log ("scenarios: %s (with what they need: %s)" % (", ".join (asked), ", ".join (n for n in names if n in Picked)))

def _Wanted (name):
	return Picked is None or name in Picked

def _Run ():
	for name, run in SCENARIOS:
		if run and _Wanted (name):
			yield from run ()
	if _Wanted ("save"):
		_Save ()

def _StepRun ():
	global Steps
	if Steps is None:
		Steps = _Run ()
	try:
		next (Steps)
		return "run"
	except StopIteration:
		return "save" if _Wanted ("save") else _After ("save")

def _After (step):
	"""what comes after the save (or instead of it): the rolling screen's new game, or the end"""
	return "rolling" if _Wanted ("rolling") else "done"

# --- M3.1: rolling the sixteen ---------------------------------------------------------------------

RollSteps = None

def _StepRolling ():
	"""e: back to the start: the game goes, the rolling screen opens on the next tick"""
	_Log ("a new game through the rolling screen")
	GemRB.QuitGame ()
	return "rollopen"

def _CardChecks (cards, what):
	import FunnelRoll
	tables = FunnelRoll.LoadTables ()
	bad = []
	for card in cards:
		mods = [FunnelRoll.AbilityMod (tables, score) for score in card["Scores"]]
		ok = all (3 <= score <= 18 for score in card["Scores"]) and mods == card["Mods"] and card["HP"] >= 1
		ok = ok and 5 <= card["Copper"] <= 60 and card["AugurMod"] == mods[FunnelRoll.LUCK] and 1 <= card["Augur"] <= 30
		ok = ok and card["Race"] == tables["dccoccup"].Int (card["Occupation"], "RACE")
		if not ok:
			bad.append (FunnelRoll.Summary (card))
	_Check ("%s: every card by the rules" % what, bad, [])
	_Check ("%s: sixteen names, none twice" % what, len (set (card["Name"] for card in cards)), 16)

def _Rolling ():
	"""e: the rolling screen: the cards, renaming, reroll all, the house rules, accept"""
	import RollScreen, Start, FunnelRoll
	RollScreen.Accepted = None
	RollScreen.Cards = None
	RollScreen.Options = FunnelRoll.Options ()
	_Check ("rolling: dccstart says the autotest starts without it", RollScreen.Rolling (), False)
	RollScreen.Open (Start.StartGame, None)
	yield
	cards = RollScreen.Cards
	_Check ("rolling: 16 cards shown", [len (cards), len ([k for k in RollScreen.Shown if isinstance (k, int)])], [16, 16])
	_Check ("rolling: a card's lines (name, abilities, occupation, gear, HP and copper, augur)", len (RollScreen.Shown[1]), 7)
	_Log ("rolling: card 1: " + " | ".join (RollScreen.Shown[1]))
	_CardChecks (cards, "rolling")
	_Check ("rolling: the house rules are off", [RollScreen.Options.rerollOne, RollScreen.Options.fourDice, RollScreen._Control (0, 3).IsVisible ()], [False, False, False])
	_Check ("rolling: no reroll one while it is off", RollScreen.RerollOne (2), None)
	# renaming: through the call, and by typing into the card's name field
	_Check ("rolling: rename", [RollScreen.Rename (0, "Gristlewick"), cards[0]["Name"], RollScreen.Shown[1][0]], [True, "Gristlewick", "Gristlewick"])
	RollScreen._Control (1, 1).SetText ("Hobb")
	yield
	_Check ("rolling: a name typed on the card", RollScreen.Cards[1]["Name"], "Hobb")
	_ShotScreen ("rolling_screen")
	# reroll all: new peasants
	before = [FunnelRoll.Summary (card) for card in cards]
	RollScreen.RerollAll ()
	yield
	after = [FunnelRoll.Summary (card) for card in RollScreen.Cards]
	_Check ("rolling: reroll all makes new peasants", sum (1 for a, b in zip (before, after) if a == b), 0)
	_CardChecks (RollScreen.Cards, "reroll all")
	# the house rules: reroll one card
	RollScreen.SetOption ("rerollOne", True)
	yield
	_Check ("rolling: the reroll button with the house rule", RollScreen._Control (2, 3).IsVisible (), True)
	others = [FunnelRoll.Summary (card) for i, card in enumerate (RollScreen.Cards) if i != 2]
	third = FunnelRoll.Summary (RollScreen.Cards[2])
	RollScreen.RerollOne (2)
	_Check ("rolling: reroll one changes that card only", [FunnelRoll.Summary (RollScreen.Cards[2]) != third, [FunnelRoll.Summary (card) for i, card in enumerate (RollScreen.Cards) if i != 2]], [True, others])
	# 4d6 drop the lowest: higher scores on the whole
	RollScreen.SetOption ("fourDice", True)
	RollScreen.RerollAll ()
	scores = [score for card in RollScreen.Cards for score in card["Scores"]]
	_Log ("rolling: 4d6 drop the lowest, mean %.2f" % (float (sum (scores)) / len (scores)))
	_Check ("rolling: 4d6 drop the lowest (mean above 3d6's 10.5)", float (sum (scores)) / len (scores) > 11, True)
	_CardChecks (RollScreen.Cards, "4d6")
	RollScreen.SetOption ("rerollOne", False)
	RollScreen.SetOption ("fourDice", False)
	RollScreen.RerollAll ()
	RollScreen.Rename (0, "Gristlewick")
	yield
	# M3.3: the alignment is the player's choice; card 3's button cycles it, here to lawful
	while RollScreen.Cards[2]["Alignment"] != 1:
		RollScreen.NextAlignment (2)
	_Check ("rolling: card 3 lawful by its button", [RollScreen.Cards[2]["Alignment"], "Lawful" in RollScreen.Shown[3][5]], [1, True])
	_Check ("rolling: Table 3-4's item on the gear line", all ("; " in RollScreen.Shown[c["Order"]][4] and c["EquipmentName"] for c in RollScreen.Cards), True)
	Saved["rolled"] = [dict (card) for card in RollScreen.Cards]
	_Check ("rolling: accept", RollScreen.Accept (), True)
	_Check ("rolling: the screen closes", RollScreen.Window, None)

def _StepRollOpen ():
	global RollSteps
	if RollSteps is None:
		RollSteps = _Rolling ()
	try:
		next (RollSteps)
		return "rollopen"
	except StopIteration:
		return "rollwait" # the game starts (SetupGame, then OnGameStart)

RolledSteps = None

def _Rolled ():
	"""e: the sixteen the rolling screen made"""
	import Roster, FunnelRoll
	from ie_slots import SLOT_WEAPON
	cards = Saved.get ("rolled", [])
	_Check ("rolled: a party of 16", GemRB.GetPartySize (), 16)
	_Check ("rolled: the test's own fifteen are gone", [GemRB.DCCFindActor (name) for name in ("fntmira", "fntpip")], [0, 0])
	ranges = {1: (115, 160), 2: (84, 120), 4: (124, 160), 5: (43, 70)}
	bad = []
	purse = 0
	for card in cards:
		pc = card["Order"]
		stats = GemRB.GetDCCStats (pc)
		record = GemRB.GetDCCFunnel (pc)
		occ, place = record["Occupation"], record["Funnel"]
		got = [GemRB.GetPlayerName (pc, 0), [GemRB.GetPlayerStat (pc, stat, 1) for stat in (IE_STR, IE_DEX, IE_CON, IE_CHR, IE_INT, IE_DCC_LUCK)],
			stats["Augur"], stats["AugurMod"], GemRB.GetPlayerStat (pc, IE_MAXHITPOINTS, 1), GemRB.GetPlayerStat (pc, IE_RACE), GemRB.GetPlayerStat (pc, IE_SEX),
			GemRB.GetPlayerStat (pc, IE_ALIGNMENT), [stats["Class"], stats["Level"]], occ["Id"], occ["Sub"], sorted (occ["Skills"]), place["StatusName"], place["Order"], place["Player"]]
		want = [card["Name"], card["Scores"], card["Augur"], card["AugurMod"], card["HP"], card["Race"], card["Sex"],
			FunnelRoll.ALIGNMENT_IE[card["Alignment"]], [8, 0], card["Occupation"], card["Sub"], sorted (card["Skills"]), "alive", pc, -1]
		if got != want:
			bad.append ((pc, got, want))
		# the items funneltest has (dccoccit.2da), the weapon in a weapon slot; the others logged
		weapons = [GemRB.GetSlotItem (pc, slot)["ItemResRef"].lower () for slot in GemRB.GetSlots (pc, SLOT_WEAPON, 1)]
		swung = _SwungItem (pc)
		if [occ["Weapon"], occ["TradeGood"]] != [card["WeaponItem"], card["GoodItem"] if not card["GoodCopper"] else ""] or (card["WeaponItem"] and card["WeaponItem"] not in weapons):
			bad.append ((pc, "items", occ["Weapon"], occ["TradeGood"], weapons, card["WeaponItem"], card["GoodItem"]))
		elif card["WeaponItem"] and swung != card["WeaponItem"]:
			# the weapon a swing uses (the engine's cached weapon info), not the slot that reads as equipped
			bad.append ((pc, "not in hand", swung))
		low, high = ranges[card["Race"]]
		if not stats["BodyRolled"] or not low <= stats["BodyWeight"] <= high:
			bad.append ((pc, "weight", stats["BodyWeight"]))
		looks = card["Looks"]
		colours = [GemRB.GetPlayerStat (pc, stat) & 0xff for stat in (IE_METAL_COLOR, IE_MINOR_COLOR, IE_MAJOR_COLOR, IE_SKIN_COLOR, IE_LEATHER_COLOR, IE_ARMOR_COLOR, IE_HAIR_COLOR)]
		if [GemRB.GetPlayerStat (pc, IE_ANIMATION_ID), stats["Hair"], stats["Beard"], colours] != [looks["Animation"], looks["Hair"], looks["Beard"], looks["Colors"]]:
			bad.append ((pc, "looks", GemRB.GetPlayerStat (pc, IE_ANIMATION_ID), stats["Hair"], stats["Beard"], colours, looks))
		purse += card["Copper"] + card["GoodCopper"]
	for entry in bad:
		_Log ("rolled: differs: %s" % (entry,))
	_Check ("rolled: every peasant as its card (abilities, Luck, augur, HP, species, sex, alignment, level 0, OCCU, FUNL, items, weight, looks)", len (bad), 0)
	_Check ("rolled: the first card's new name", GemRB.GetPlayerName (1, 0), "Gristlewick")
	# M3.3: a script name each, the player's alignment from the card, Table 3-4's item where the game has it
	names = [GemRB.GetPlayerName (pc, 2).lower () for pc in range (1, 17)]
	_Check ("rolled: a script name of its own each", names, ["dccpeas%02d" % pc for pc in range (1, 17)])
	_Check ("rolled: card 3's alignment as the player set it (lawful)", GemRB.GetDCCStats (3)["AlignmentCode"], 1)
	missing = []
	for card in cards:
		item = card.get ("EquipmentItem")
		if item and not any (GemRB.GetSlotItem (card["Order"], slot) and GemRB.GetSlotItem (card["Order"], slot)["ItemResRef"].lower () == item for slot in range (0, 60)):
			missing.append ((card["Order"], item))
	_Check ("rolled: Table 3-4's items the game has are in the packs", missing, [])
	_Log ("rolled: Table 3-4: %s" % ", ".join ("%d %s%s" % (c["Order"], c.get ("EquipmentName", "?"), " (%s)" % c["EquipmentItem"] if c.get ("EquipmentItem") else "") for c in cards))
	import SetupGame
	_Check ("rolled: the purse holds everyone's copper (in cp, on top of the game's own)", GemRB.GameGetPartyGold () - SetupGame.StartGold, purse)
	_Check ("rolled: everyone has the party AI", sorted (set ((GemRB.GetPlayerScript (pc, 7) or "").lower () for pc in range (1, 17))), ["dccparty"])
	Roster.UpdateRoster ()
	_Check ("rolled: the roster", [Roster.Shown.get (1, "").split ("\n")[0], len (Roster.Tiles), Roster.Header["Summary"]], ["Gristlewick", 16, "Alive 16 of 16"])
	yield
	_Shot ("rolled_party", 1)
	_ShotScreen ("rolled_roster")
	yield from _ArmedHit (cards)

def _SwungItem (pc):
	"""the item a melee or missile swing of this party member uses (its cached weapon info: the slot
	that GetCombatDetails reports, in the engine's own numbering), lower case; "fist" for none"""
	details = GemRB.GetCombatDetails (pc, 0)
	if "Slot" not in details:
		return ""
	item = GemRB.GetSlotItem (pc, details["Slot"], 1)
	return item["ItemResRef"].lower () if item else ""

def _ArmedHit (cards):
	"""e: a rolled peasant's weapon really strikes: a lethal (not subdual) blow with its occupation's
	weapon (M3.1's weapon check once passed while every swing was the fists')"""
	armed = [c["Order"] for c in cards if c["WeaponItem"] and GemRB.GetCombatDetails (c["Order"], 0).get ("Flags", 0) & 1]
	_Check ("rolled: a peasant with a melee weapon", bool (armed), True)
	if not armed:
		return
	pc = armed[0]
	weapon = [c["WeaponItem"] for c in cards if c["Order"] == pc][0]
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND) # nobody joins in
	where = GemRB.GetActorPosition (pc)
	gob = GemRB.CreateCreature (1, "FNTGOB", where["X"] + 40, where["Y"])
	_Put (gob, where["X"] + 40, where["Y"])
	yield
	before = GemRB.GetPlayerStat (gob, IE_HITPOINTS)
	GemRB.DCCForceDice (pc, [19, 6, 6, 6, 6])
	GemRB.DCCAttack (pc, gob)
	GemRB.DCCForceDice (pc, [])
	attack = (GemRB.GetDCCLastAttack (pc) or [{}])[0]
	yield
	lost = before - GemRB.GetPlayerStat (gob, IE_HITPOINTS)
	_Log ("rolled: %s's armed hit: %s" % (GemRB.GetPlayerName (pc, 0), attack))
	_Check ("rolled: the swing is the occupation's weapon", _SwungItem (pc), weapon)
	_Check ("rolled: an armed hit (hit, not subdual, damage taken)", [attack.get ("Hit"), attack.get ("Subdual"), attack.get ("Damage", 0) > 0 and lost == attack.get ("Damage")], [1, 0, True])
	_Shot ("rolled_armed_hit", pc)
	GemRB.ExecuteString ("DestroySelf()", gob)
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	yield

def _StepRolled ():
	global RolledSteps
	if RolledSteps is None:
		if GemRB.GetGameVar ("FNT_AUTOTEST") != 1:
			return "rolled" # the area script hasn't seen the sixteen yet
		RolledSteps = _Rolled ()
	try:
		next (RolledSteps)
		return "rolled"
	except StopIteration:
		return "choose" if _Wanted ("choose") else "done"

# --- M3.3: the funnel's end and the reserves -----------------------------------------------------

ChooseSteps = None
ReserveSteps = None

WOLAETH = 14 # dccgods.2da: the chaotic god of death and dark magic, the DCC Ahriman

def _Funnel (pc):
	return GemRB.GetDCCFunnel (pc)["Funnel"]

def _Id (pc):
	return GemRB.GetDCCFunnel (pc)["GlobalID"]

def _FunnelHistory ():
	"""f: a little history before the end: a marauder kills two, a peasant kills a rat, one nearly dies"""
	pcs = list (range (1, 17))
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND) # nobody joins in on their own
	spots = _Block (pcs)
	yield
	# the last two die to a marauder
	for victim in (15, 16):
		x, y = spots[victim]
		gob = GemRB.CreateCreature (1, "FNTGOB", x + 40, y)
		_Put (gob, x + 40, y)
		yield
		yield from _Kill (gob, victim)
		GemRB.ExecuteString ("DestroySelf()", gob)
		yield
	_Check ("history: 15 and 16 dead", [_Dead (15), _Dead (16)], [True, True])
	# a kill: the giant rat (worn down to 3 hp first) dies to peasant 3's own weapon (the kill is
	# credited by DCCOnDeath); lethal blows, not the fists' subdual ones (fixed after M3.3)
	killer = 3
	x, y = spots[killer]
	rat = GemRB.CreateCreature (1, "FNTRAT", x + 40, y)
	_Put (rat, x + 40, y)
	GemRB.SetPlayerStat (rat, IE_HITPOINTS, 3)
	yield
	subdual = []
	for i in range (5):
		if GemRB.GetPlayerStat (rat, IE_STATE_ID) & STATE_DEAD:
			break
		GemRB.DCCForceDice (killer, [19, 6, 6, 6, 6])
		GemRB.DCCAttack (killer, rat)
		GemRB.DCCForceDice (killer, [])
		subdual += [a["Subdual"] for a in GemRB.GetDCCLastAttack (killer)]
		yield
	_Check ("history: the rat died to the peasant's weapon (no subdual blows)", [bool (GemRB.GetPlayerStat (rat, IE_STATE_ID) & STATE_DEAD), any (subdual)], [True, False])
	_Check ("history: the killer's kill on its funnel record", _Funnel (killer)["Kills"], 1)
	# one with 2 hp or more nearly dies: down to 1
	near = [pc for pc in range (1, 15) if pc != killer and GemRB.GetPlayerStat (pc, IE_HITPOINTS) >= 2][0]
	GemRB.ApplyEffect (near, "Damage", GemRB.GetPlayerStat (near, IE_HITPOINTS) - 1, 0)
	yield
	_Check ("history: a near death on the record (1 hp left)", [GemRB.GetPlayerStat (near, IE_HITPOINTS), _Funnel (near)["NearDeaths"]], [1, 1])
	Saved["near"] = _Id (near)
	Saved["killer"] = _Id (killer)
	graves = GemRB.GetDCCGraveyard ()
	_Check ("history: two graves in the game's record", [(g["Order"], g["Killer"], g["Cause"], g["Place"]) for g in graves], [(15, "Marauder", "cudgel", "the glade"), (16, "Marauder", "cudgel", "the glade")])
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	for i in range (30): # the fight's counter runs out
		yield

def _Choose ():
	"""f: choose your four, their class picks (a cleric with its god, a wizard and the learn window), the end screen"""
	import ChooseFour, ClassPick, DCCLearn, FunnelEnd, FunnelChoose, Roster
	yield from _FunnelHistory ()
	Saved["rebuilds"] = Roster.Rebuilds
	# the funnel ends by a script action (the adventure's last scene will do the same)
	GemRB.ExecuteString ('SetGlobal("DCC_FUNNEL","GLOBAL",1)')
	yield from _Wait (lambda: ChooseFour.Window, 10)
	_Check ("choose: the screen opens when the funnel ends", bool (ChooseFour.Window), True)
	if not ChooseFour.Window:
		return
	survivors = ChooseFour.Survivors
	_Check ("choose: 14 survivors as cards", [len (survivors), len (ChooseFour.Shown["Cards"])], [14, 14])
	card = ChooseFour.Shown[Saved["killer"]]
	_Log ("choose: a card: " + " | ".join (card))
	_Check ("choose: a card's lines (name, abilities, occupation, HP and alignment, the record)", len (card), 6)
	_Check ("choose: the kill on its card", card[5], "Kills 1   Near deaths 0")
	_Check ("choose: the near death on its card", ChooseFour.Shown[Saved["near"]][5], "Kills 0   Near deaths 1")
	ChooseFour.SortBy ("Str")
	strengths = [ChooseFour.Shown[i][1].split ()[1] for i in ChooseFour.Shown["Cards"]]
	_Check ("choose: sorted by Str", [int (v) for v in strengths], sorted ([int (v) for v in strengths], reverse = True))
	ChooseFour.SortBy ("Kills")
	_Check ("choose: sorted by kills: the killer first", ChooseFour.Shown["Cards"][0], Saved["killer"])
	ChooseFour.SortBy ("Order")
	_Check ("choose: Accept waits for a pick", GemRB.GetPlayerStat (1, IE_HITPOINTS) > 0 and not FunnelChoose.Ready (ChooseFour.Picks, survivors), True)
	# the picks: three humans and a demihuman if there is one (its class is its race's)
	humans = [s for s in survivors if s["Race"] == 1]
	others = [s for s in survivors if s["Race"] != 1]
	picks = humans[:3] + (others[:1] if others else humans[3:4])
	# the cleric-to-be's alignment, the player's choice on the card: chaotic
	ChooseFour.SetAlignment (picks[0]["Id"], 3)
	_Check ("choose: the alignment changed on the card", GemRB.GetDCCStats (picks[0]["Id"])["AlignmentCode"], 3)
	for pick in picks:
		ChooseFour.Pick (pick["Id"])
	fifth = [s for s in survivors if s not in picks][0]
	_Check ("choose: a fifth pick doesn't fit", ChooseFour.Pick (fifth["Id"]), False)
	_Check ("choose: four picked", ChooseFour.Shown["Picks"], [p["Id"] for p in picks])
	_ShotScreen ("choose_screen")
	Saved["picks"] = [p["Id"] for p in picks]
	Saved["survivors"] = [s["Id"] for s in survivors]
	_Check ("choose: accept", ChooseFour.Accept (), True)
	yield
	# the end of the funnel in the engine
	_Check ("funnel's end: the party is the four, in the order picked", [_Id (pc) for pc in range (1, GemRB.GetPartySize () + 1)], Saved["picks"])
	_Check ("funnel's end: a party of four from now on", [GemRB.GetGameVar ("DCC_PARTY_SIZE"), GemRB.GetVar ("MaxPartySize")], [4, 16])
	reserves = GemRB.GetDCCReserves ()
	_Check ("funnel's end: the other ten in the reserves", sorted (reserves), sorted (i for i in Saved["survivors"] if i not in Saved["picks"]))
	Saved["reserves"] = reserves
	xp = dict ((i, GemRB.GetDCCStats (i)["XP"]) for i in Saved["survivors"])
	_Check ("funnel's end: every survivor has the XP for level 1", sorted (set (xp.values ())), [10])
	_Check ("funnel's end: every survivor's class pick pending", [_Funnel (i)["ClassPending"] for i in Saved["survivors"]], [1] * 14)
	_Check ("funnel's end: the reserves stay level 0 and wait off the map", [(GemRB.GetDCCStats (i)["Level"], _Funnel (i)["StatusName"], GemRB.DCCFindActor (GemRB.GetPlayerName (i, 2))) for i in reserves], [(0, "reserve", 0)] * 10)
	_Check ("funnel's end: their script names stay their own", sorted (GemRB.GetPlayerName (i, 2).lower () for i in reserves), sorted ("dccpeas%02d" % _Funnel (i)["Order"] for i in reserves))
	_Check ("funnel's end: the chosen", [_Funnel (i)["StatusName"] for i in Saved["picks"]], ["chosen"] * 4)
	_Check ("funnel's end: the dead are out of the party, in the graveyard", [len (Roster.GraveLinesAll ()), any (_Funnel (pc)["StatusName"] == "dead" for pc in range (1, 5))], [2, False])
	Roster.UpdateRoster ()
	_Check ("funnel's end: the roster shows four", [len (Roster.Tiles), Roster.Header["Summary"]], [4, "Alive 4 of 4"])
	_Check ("funnel's end: the roster was rebuilt for the four", Roster.Rebuilds > Saved["rebuilds"], True)
	_RosterIsParty ("funnel's end")
	yield
	_ShotScreen ("roster_four")
	# the class picks: one window at a time, for each of the four
	classes = {}
	for index, ident in enumerate (Saved["picks"]):
		yield from _Wait (lambda: ClassPick.Window, 10)
		_Check ("class pick %d: its window opens" % (index + 1), [bool (ClassPick.Window), ClassPick.Shown], [True, ident])
		if not ClassPick.Window:
			return
		race = GemRB.GetPlayerStat (ident, IE_RACE)
		if race != 1:
			want, alignment, god = ClassPick.Last["Choice"]["Class"], None, None
			_Check ("class pick: a demihuman takes its own class", len (ClassPick.Last["Classes"]), 1)
		elif index == 0:
			want, alignment, god = 3, 3, WOLAETH # a chaotic cleric of Wolaeth (the Ethren Ahriman)
		elif index == 1:
			want, alignment, god = 1, None, None # a wizard
		else:
			want, alignment, god = 2 if index == 2 else 4, None, None
		valid = ClassPick.Choose (want, alignment, god)
		if index == 0:
			chaotic = FunnelChoose.GodsFor (ChooseFour.Tables ()["dccgods"], 3)
			_Check ("class pick: the cleric's gods are its alignment's (the Ethren pantheon)", ClassPick.Last["Gods"], ["Naseen", "Olyka", "Wolaeth", "Buudred", "Hemliget", "Morgen Windwalker"])
			_Check ("class pick: each god's button names its domain on hover, the info line the chosen one's", [ClassPick.Last["GodLines"][2], "Wolaeth, god of death, dark magic" in ClassPick.Last["Info"]], ["Wolaeth, god of death, dark magic", True])
			ClassPick.SetAlignment (1)
			_Check ("class pick: a lawful cleric can't keep Wolaeth (the god follows)", [ClassPick.Choice["God"], ClassPick.Last["Valid"]], [0, False])
			ClassPick.SetAlignment (3)
			ClassPick.SetGod ([i for i, name in chaotic].index (WOLAETH))
			valid = ClassPick.Last["Valid"]
			_ShotScreen ("class_pick")
		_Check ("class pick %d: valid" % (index + 1), valid, True)
		hp = ClassPick.Confirm ()
		_Check ("class pick %d: level 1" % (index + 1), [hp >= 1, GemRB.GetDCCStats (ident)["Level"], GemRB.GetDCCStats (ident)["Class"], _Funnel (ident)["ClassPending"]], [True, 1, want, 0])
		classes[ident] = want
		yield
		if DCCLearn.Window:
			_Check ("class pick %d: the learn window for a caster" % (index + 1), want in FunnelChoose.CASTERS, True)
			DCCLearn.Confirm ()
			yield
	cleric = Saved["picks"][0]
	stats = GemRB.GetDCCStats (cleric)
	if GemRB.GetPlayerStat (cleric, IE_RACE) == 1:
		_Check ("the cleric: chaotic, of Wolaeth (death, dark magic; the DCC Ahriman's mechanics)", [stats["AlignmentCode"], stats["God"], stats["GodName"], stats["GodDomain"], stats["GodDCC"]], [3, WOLAETH, "Wolaeth", "death, dark magic", "Ahriman"])
		_Check ("the cleric's sheet names its god", "serves Wolaeth" in DCCSheet.Lines (cleric)[0], True)
		_Check ("alignment is fixed past level 0", GemRB.DCCSetAlignment (cleric, 1), False)
	Saved["classes"] = classes
	# the end screen, once every pick is made
	yield from _Wait (lambda: FunnelEnd.Window, 10)
	_Check ("end screen opens", [bool (FunnelEnd.Window), ChooseFour.State ()], [True, 3])
	_Log ("end screen: party %s" % FunnelEnd.Shown.get ("Party"))
	_Check ("end screen: the four, the ten, the two graves", [len (FunnelEnd.Shown.get ("Party", [])), len (FunnelEnd.Shown.get ("Reserves", [])), len (FunnelEnd.Shown.get ("Graves", []))], [4, 10, 2])
	_Check ("end screen: a reserve's line (level-1 XP, class pending)", FunnelEnd.Shown["Reserves"][0].endswith ("10 XP, class pick pending"), True)
	_ShotScreen ("funnel_end")
	FunnelEnd.Leave ()
	_Check ("end screen: closed, the funnel done", [FunnelEnd.Window, ChooseFour.State ()], [None, 4])

def _StepChoose ():
	global ChooseSteps
	if ChooseSteps is None:
		ChooseSteps = _Choose ()
	try:
		next (ChooseSteps)
		return "choose"
	except StopIteration:
		return "reserves" if _Wanted ("reserves") else "done"

ReserveSave = "funnelend"

def _Reserves ():
	"""g: a party member dies, the camp's roster: bury it, a reserve joins and picks its class; a dismissal"""
	import ChooseFour, ClassPick, DCCLearn, Reserves, Roster
	# the party's second dies (level 1: it bleeds out)
	victim = 2
	Saved["victim"] = GemRB.GetPlayerName (victim, 0)
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_NAND)
	GemRB.ApplyEffect (victim, "Damage", GemRB.GetPlayerStat (victim, IE_HITPOINTS) + 2, 0)
	yield from _Wait (lambda: _Dead (victim), 60)
	GemRB.GameSetScreenFlags (GS_PARTYAI, OP_OR)
	_Check ("reserves: the second of the four died", _Dead (victim), True)
	graves = GemRB.GetDCCGraveyard ()
	_Check ("reserves: its grave, with its class", [(g["Name"], g["Level"], g["Class"], g["Cause"]) for g in graves][-1], (Saved["victim"], 1, Saved["classes"][Saved["picks"][1]], "bled out"))
	# a camp script opens the roster
	GemRB.ExecuteString ('SetGlobal("DCC_CAMP","GLOBAL",1)')
	yield from _Wait (lambda: Reserves.Window, 10)
	_Check ("reserves: the camp's roster opens", bool (Reserves.Window), True)
	if not Reserves.Window:
		return
	_Check ("reserves: ten waiting, the party of four", [len (Reserves.Shown["Reserves"]), len (Reserves.Shown["Party"])], [10, 4])
	_Check ("reserves: the dead one's row", Reserves.Shown["Party"][1].endswith ("(dead)"), True)
	_ShotScreen ("reserves_roster")
	newcomer = Reserves.Shown["Reserves"][0]
	_Check ("reserves: none joins a full party", Reserves.Join (newcomer), -1)
	# bury the dead, the first reserve joins
	_Check ("reserves: swap the dead for a reserve", Reserves.Swap (victim, newcomer) >= 0, True)
	yield
	party = [_Id (pc) for pc in range (1, GemRB.GetPartySize () + 1)]
	_Check ("reserves: the newcomer is the fourth", [len (party), party[-1]], [4, newcomer])
	_Check ("reserves: it is chosen, level 0, 10 XP, its pick pending", [_Funnel (newcomer)["StatusName"], GemRB.GetDCCStats (newcomer)["Level"], GemRB.GetDCCStats (newcomer)["XP"], _Funnel (newcomer)["ClassPending"]], ["chosen", 0, 10, 1])
	_Check ("reserves: nine wait now", len (GemRB.GetDCCReserves ()), 9)
	_Check ("reserves: the dead one is buried, its grave kept", [Saved["victim"] in [GemRB.GetPlayerName (pc, 0) for pc in range (1, 5)], len (Roster.GraveLinesAll ())], [False, 3])
	_RosterIsParty ("reserves: after the swap")
	# its class pick comes up
	yield from _Wait (lambda: ClassPick.Window, 10)
	_Check ("reserves: the newcomer's class pick opens", [bool (ClassPick.Window), ClassPick.Shown], [True, newcomer])
	if ClassPick.Window:
		race = GemRB.GetPlayerStat (newcomer, IE_RACE)
		want = ClassPick.Last["Choice"]["Class"] if race != 1 else 2
		ClassPick.Choose (want)
		ClassPick.Confirm ()
		yield
		if DCCLearn.Window:
			DCCLearn.Confirm ()
		_Check ("reserves: the newcomer is level 1", GemRB.GetDCCStats (newcomer)["Level"], 1)
	# a living party member goes back to the reserves, level 1 and all
	first = _Id (1)
	Reserves.Open ()
	_Check ("reserves: dismiss the first", Reserves.Dismiss (1), 0)
	_Check ("reserves: dismissed, in the reserves, still level 1", [GemRB.GetPartySize (), first in GemRB.GetDCCReserves (), GemRB.GetDCCStats (first)["Level"], _Funnel (first)["StatusName"]], [3, True, 1, "reserve"])
	_RosterIsParty ("reserves: a party of three")
	_Check ("reserves: and back again (room for one)", Reserves.Join (first) >= 0, True)
	Reserves.Leave ()
	_Check ("reserves: closed, the camp's global cleared", [Reserves.Window, GemRB.GetGameVar ("DCC_CAMP")], [None, 0])
	yield
	Saved["reserves2"] = sorted (GemRB.GetDCCReserves ())
	Saved["reserveNames"] = sorted (GemRB.GetPlayerName (i, 0) for i in GemRB.GetDCCReserves ())
	Saved["graves"] = GemRB.GetDCCGraveyard ()
	Saved["party"] = [GemRB.GetPlayerName (pc, 0) for pc in range (1, GemRB.GetPartySize () + 1)]
	for game in [g for g in GemRB.GetSaveGames () if g.GetName () == ReserveSave]:
		GemRB.DeleteSaveGame (game)
	_Check ("reserves: game saved", GemRB.SaveGame (None, ReserveSave), 0)

def _StepReserves ():
	global ReserveSteps
	if ReserveSteps is None:
		ReserveSteps = _Reserves ()
	try:
		next (ReserveSteps)
		return "reserves"
	except StopIteration:
		games = [g for g in GemRB.GetSaveGames () if g.GetName () == ReserveSave]
		if not games:
			return "done"
		_Log ("loading the save '%s'" % ReserveSave)
		def Load ():
			GemRB.LoadGame (games[0])
			GemRB.EnterGame ()
		GemRB.QuitGame ()
		GemRB.SetTimer (Load, 0, 0)
		return "rsvloading"

def _StepReservesLoaded ():
	_Check ("loaded: the party of four", [GemRB.GetPlayerName (pc, 0) for pc in range (1, GemRB.GetPartySize () + 1)], Saved.get ("party"))
	_Check ("loaded: the party size stays four", [GemRB.GetGameVar ("DCC_PARTY_SIZE"), GemRB.GetGameVar ("DCC_FUNNEL")], [4, 4])
	_Check ("loaded: the reserves with their records", sorted (GemRB.GetPlayerName (i, 0) for i in GemRB.GetDCCReserves ()), Saved.get ("reserveNames"))
	_Check ("loaded: the reserves' pick still pending, level 0", sorted (set ((GemRB.GetDCCStats (i)["Level"], GemRB.GetDCCStats (i)["XP"], _Funnel (i)["ClassPending"], _Funnel (i)["StatusName"]) for i in GemRB.GetDCCReserves ())), [(0, 10, 1, "reserve")])
	_Check ("loaded: the graveyard (the game's record)", GemRB.GetDCCGraveyard (), Saved.get ("graves"))
	import Roster
	Roster.UpdateRoster ()
	_Check ("loaded: the roster of four", len (Roster.Tiles), 4)
	_RosterIsParty ("loaded")
	return "done"

StepTable = {
	"run": _StepRun,
	"save": _StepSave,
	"loaded": _StepLoaded,
	"rolling": _StepRolling,
	"rollopen": _StepRollOpen,
	"rolled": _StepRolled,
	"choose": _StepChoose,
	"reserves": _StepReserves,
	"rsvloaded": _StepReservesLoaded,
}

def _Tick (token):
	global Step, Failures
	if token != Token or Step in ("loading", "finished", "rollwait", "rsvloading"):
		return
	try:
		if Step == "wait":
			if GemRB.GetGameVar ("FNT_AUTOTEST") == 1:
				_Log ("autotest start")
				_PickScenarios ()
				Step = "run"
			return
		if Step == "done":
			_Log ("autotest done: %d checks, %d failed" % (Checks, Failures))
			GemRB.SetGlobal ("FNT_AT_DONE", "GLOBAL", 1 if Failures == 0 else 2)
			Step = "finished"
			return
		Step = StepTable[Step] ()
	except Exception as error:
		import traceback
		Failures += 1
		_Log ("FAIL step %s raised %s: %s" % (Step, type (error).__name__, error))
		for line in traceback.format_exc ().splitlines ():
			_Log ("  " + line)
		Step = "done"

def OnGameStart ():
	"""MessageWindow.OnLoad: (re)starts the ticks; a load in the middle continues"""
	global Token, Step
	Token += 1
	if Step == "loading":
		Step = "loaded"
	elif Step == "rollwait":
		Step = "rolled"
	elif Step == "rsvloading":
		Step = "rsvloaded"
	token = Token
	GemRB.SetTimer (lambda: _Tick (token), 500, -1)
