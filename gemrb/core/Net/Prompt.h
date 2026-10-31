// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Prompt.h
 * "Ask the owning player": questions the game puts to the player who owns a
 * party member (or to every player, a vote), answered later, on some other frame.
 *
 * This is the bookkeeping only: what was asked, who may answer, which answers
 * are legal, timeouts and players who are away. It has no engine, GUI or
 * transport dependency, so single player uses it as it is (everything is slot 0)
 * and it is unit tested on its own. PromptManager (core/Prompts.h) puts it to
 * work: pausing, the prompt window, the network and the callbacks.
 */

#ifndef NET_PROMPT_H
#define NET_PROMPT_H

#include "exports.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace GemRB {

// the host, or the only player in single player; answers for owners who are away
constexpr uint8_t PROMPT_LOCAL_SLOT = 0;
// number keys 1..9 pick a choice
constexpr size_t PROMPT_MAX_CHOICES = 9;
// integer prompts may have several values, eg. spellburn's Strength, Agility, Stamina
constexpr size_t PROMPT_MAX_FIELDS = 4;
// a vote asks everyone in the session
constexpr size_t PROMPT_MAX_VOTERS = 4;
constexpr uint32_t PROMPT_DEFAULT_TIMEOUT = 30000;
constexpr uint32_t PROMPT_MAX_TIMEOUT = 600000;

enum class PromptKind : uint8_t {
	YesNo,
	Choice,
	Integer,
	count
};

// one integer of an Integer prompt; value is the default
struct PromptField {
	std::string label;
	int32_t min = 0;
	int32_t max = 0;
	int32_t value = 0;
};

struct PromptQuestion {
	enum Flags : uint8_t {
		VOTE = 1, // yes/no to several players; the result is yes only if every one of them says yes
		STOP_ON_NO = 2 // a vote ends at the first no
	};
	PromptKind kind = PromptKind::YesNo;
	uint8_t flags = 0;
	std::string tag; // what it is about, for logs, tests and scripts: "travel", "luckburn"
	std::string title;
	std::string text;
	std::vector<std::string> choices; // Choice: 2..PROMPT_MAX_CHOICES
	std::vector<PromptField> fields; // Integer: 1..PROMPT_MAX_FIELDS
	int32_t defaultChoice = 0; // YesNo: 1 yes, 0 no; Choice: the index
	uint32_t timeout = 0; // ms, 0 for PROMPT_DEFAULT_TIMEOUT
	uint32_t actorID = 0; // the party member it is about (the host's global ID), 0 for none
};

// a question we can put to anyone: the right number of choices or fields, sane ranges and defaults
GEM_EXPORT bool PromptWellFormed(const PromptQuestion& question);
// what timing out (or Esc) answers
GEM_EXPORT std::vector<int32_t> PromptDefault(const PromptQuestion& question);
// an answer the question allows: one value (YesNo 0/1, Choice an index) or one per field, in range
GEM_EXPORT bool PromptLegal(const PromptQuestion& question, const std::vector<int32_t>& values);
// the answer in words, for the outcome line: "Yes", "Burn 2", "Strength 1, Agility 0"
GEM_EXPORT std::string PromptAnswerText(const PromptQuestion& question, const std::vector<int32_t>& values);

enum class PromptOutcome : uint8_t {
	Answered, // the asked player answered (or every voter did)
	TimedOut, // no answer in time (from someone): the default counts
	HostAnswered, // the owner was away, the host answered for them (BG: their characters are the host's meanwhile)
	Cancelled, // the caller withdrew it, or the game it was about is gone
	count
};
GEM_EXPORT const char* PromptOutcomeName(PromptOutcome outcome);

struct PromptVoter {
	enum class State : uint8_t {
		Waiting,
		Answered,
		Defaulted, // timed out, or away when the vote ended
		count
	};
	uint8_t slot = 0; // who is asked: the party member's owner, or one player of a vote
	uint8_t by = 0; // who answers now: the owner, or the host while the owner is away (single prompts)
	State state = State::Waiting;
	bool away = false;
	std::vector<int32_t> values;
};

struct GEM_EXPORT PromptResult {
	uint32_t id = 0;
	PromptQuestion question;
	// the answer; a vote's is {1} when everyone said yes (or let it time out), else {0}
	std::vector<int32_t> values;
	uint8_t slot = PROMPT_LOCAL_SLOT; // who answered (single prompts)
	PromptOutcome outcome = PromptOutcome::Answered;
	std::vector<PromptVoter> voters;

	int32_t Value(size_t index = 0) const { return index < values.size() ? values[index] : 0; }
	bool Yes() const { return Value() != 0; }
};

struct PromptActive {
	uint32_t id = 0;
	PromptQuestion question;
	std::vector<PromptVoter> voters;
	uint64_t deadline = 0;
};

/**
 * Each player has a lane: a single prompt's lane is its owner (the player it asks), a vote's
 * the players it asks. Prompts whose lanes don't meet are open at the same time (M2.10: four
 * players each answer their own); in one lane they wait in order, one at a time, and their
 * clocks start when they open. While its owner is away a single prompt is the host's to answer
 * (it keeps its lane, so the owner gets it back on return); a vote doesn't wait for a voter who
 * is away, who counts as the default unless they come back in time. Timeouts answer with the
 * default. Nothing waits forever. In single player everything is slot 0: one lane, one at a time.
 */
class GEM_EXPORT PromptQueue {
public:
	enum class Status : uint8_t {
		Accepted,
		Closed, // not an open prompt: answered already, timed out, cancelled or never asked
		NotAsked, // not this player's to answer
		Already, // this player answered already (votes)
		Illegal // not an answer the question allows
	};

	/** asked: the owner (one slot) or the voters; returns the prompt's id, 0 if the question is malformed */
	uint32_t Ask(PromptQuestion question, std::vector<uint8_t> asked, uint64_t now);
	/** the open prompts, oldest first */
	const std::vector<PromptActive>& Open() const { return open; }
	/** the oldest open prompt, nullptr if none */
	const PromptActive* Active() const;
	const PromptActive* Find(uint32_t id) const;
	/** prompts waiting to open, in every lane */
	size_t Queued() const { return queue.size(); }
	/** prompts waiting behind an open one, in its lane */
	size_t QueuedBehind(uint32_t id) const;
	/** prompts waiting in a player's lane */
	size_t QueuedFor(uint8_t slot) const;
	bool Busy() const { return !open.empty() || !queue.empty(); }
	Status Answer(uint32_t id, uint8_t slot, const std::vector<int32_t>& values, uint64_t now);
	/** players come and go; the host is always there */
	void SetPresent(uint8_t slot, bool present);
	bool Present(uint8_t slot) const;
	/** timeouts and opening the prompts next in their lanes */
	void Update(uint64_t now);
	bool Cancel(uint32_t id);
	void Clear();
	/** finished prompts since the last call, in order */
	std::vector<PromptResult> TakeFinished();
	/** ms left on the oldest open prompt, or on this one */
	uint32_t Remaining(uint64_t now) const;
	uint32_t Remaining(uint32_t id, uint64_t now) const;
	/** bumped whenever an open prompt changes (opened, answered in part, its answerer changed, closed) */
	uint32_t Revision() const { return revision; }

	static const char* StatusName(Status status);

private:
	struct Pending {
		uint32_t id;
		PromptQuestion question;
		std::vector<uint8_t> asked;
	};

	static std::vector<uint8_t> Lane(const PromptActive& prompt);
	void Pump(uint64_t now);
	void Open(Pending pending, uint64_t now);
	void Retarget(PromptActive& prompt);
	void Finish(size_t index, PromptOutcome outcome);
	bool Decided(const PromptActive& prompt) const;

	uint32_t nextID = 1;
	uint32_t revision = 0;
	std::vector<PromptActive> open;
	std::deque<Pending> queue;
	std::set<uint8_t> absent;
	std::vector<PromptResult> finished;
};

}

#endif
