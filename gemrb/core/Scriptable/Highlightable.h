// SPDX-FileCopyrightText: 2024 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef H_HIGHLIGHTABLE
#define H_HIGHLIGHTABLE

#include "ie_cursors.h"
#include "strrefs.h"

#include "Scriptable.h"

namespace GemRB {

class Gem_Polygon;

class GEM_EXPORT Highlightable : public Scriptable {
public:
	using Scriptable::Scriptable;
	virtual int TrapResets() const = 0;
	virtual bool CanDetectTrap() const { return true; }
	virtual bool PossibleToSeeTrap() const;
	virtual void TryBashLock(Actor* actor) = 0;
	virtual void TryPickLock(Actor* actor) = 0;
	virtual bool IsLocked() const { return false; }

public:
	std::shared_ptr<Gem_Polygon> outline = nullptr;
	Color outlineColor = ColorBlack;
	ieDword Cursor = IE_CURSOR_NORMAL;
	bool Highlight = false;
	Point TrapLaunch = Point(-1, -1);
	ieWord TrapDetectionDiff = 0;
	ieWord TrapRemovalDiff = 0;
	ieWord Trapped = 0;
	ieWord TrapDetected = 0;
	ResRef KeyResRef;
	// play this wav file when stepping on the trap (on PST)
	ResRef EnterWav;
	ieVariable OriginalName;
	// DCC (traps and thief skills, Scriptable/ActorDCCThief.cpp): the party slots that have rolled to find
	// this trap, to disarm it and to pick this lock (one try each; DCC::TrapSlotBit), and DCC_NOTICE_SAID
	// once its notice line floated; saved with the area (regions and containers in their records' unused
	// bytes, doors in area variables)
	struct DCCRolls {
		uint16_t found = 0;
		uint16_t disarm = 0;
		uint16_t lock = 0;
		uint16_t flags = 0;
	};
	static constexpr uint16_t DCC_NOTICE_SAID = 1;
	DCCRolls dccRolls;

public:
	bool IsOver(const Point& place) const;
	void DrawOutline(Point origin) const;
	void SetCursor(unsigned char cursorIndex);

	void SetTrapDetected(int detected);
	void TryDisarm(Actor* actor);
	// detect trap, set skill to 256 if you want sure fire
	void DetectTrap(int skill, ieDword actorID);
	// returns true if trap is visible
	bool VisibleTrap(int seeAll) const;
	// returns true if trap has been triggered
	virtual bool TriggerTrap(int skill, ieDword ID);
	bool TryUnlock(Actor* actor, bool removekey) const;
	bool TryBashLock(Actor* actor, ieWord lockDifficulty, HCStrings failStr);
	bool TryPickLock(Actor* actor, ieWord lockDifficulty, ieStrRef customFailStr, HCStrings failStr);
	const ieVariable& GetOriginalName() const { return OriginalName; };

	// DCC rules (DCCRules), beside the stock ones above; Scriptable/ActorDCCThief.cpp
	/** a searcher's find roll (d20 + Int, or a thief's find trap), once per party slot; true if found */
	bool DCCDetectTrap(Actor* searcher);
	/** a disarm: a natural 1 or a miss by 5 or more springs it, a smaller miss only fails; once per slot */
	void DCCTryDisarm(Actor* actor);
	/** picking the lock: d20 + pick lock + Agi with tools, else by the occupation rules; once per slot */
	bool DCCTryPickLock(Actor* actor, ieWord lockDifficulty, ieStrRef customFailStr, HCStrings failStr);
	/** found for the party by a script or a spell: the outline shows, Detected is sent */
	void DCCRevealTrap(ieDword actorID);
	/** the party slots' masks changed (a door keeps them in an area variable) */
	virtual void DCCRollsChanged() {}
	/** the point of the outline (or the position) nearest to p */
	Point DCCNearestPoint(const Point& p) const;
};

}

#endif
