// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Net/Prompt.h"

#include <algorithm>

namespace GemRB {

bool PromptWellFormed(const PromptQuestion& question)
{
	if (question.kind >= PromptKind::count || question.timeout > PROMPT_MAX_TIMEOUT) return false;
	if (question.flags & ~(PromptQuestion::VOTE | PromptQuestion::STOP_ON_NO)) return false;
	if ((question.flags & PromptQuestion::VOTE) && question.kind != PromptKind::YesNo) return false;
	switch (question.kind) {
		case PromptKind::YesNo:
			return question.choices.empty() && question.fields.empty() && (question.defaultChoice == 0 || question.defaultChoice == 1);
		case PromptKind::Choice:
			return question.choices.size() >= 2 && question.choices.size() <= PROMPT_MAX_CHOICES && question.fields.empty() && question.defaultChoice >= 0 && size_t(question.defaultChoice) < question.choices.size();
		case PromptKind::Integer:
			if (question.fields.empty() || question.fields.size() > PROMPT_MAX_FIELDS || !question.choices.empty()) return false;
			for (const auto& field : question.fields) {
				if (field.min > field.max || field.value < field.min || field.value > field.max) return false;
			}
			return true;
		default:
			return false;
	}
}

std::vector<int32_t> PromptDefault(const PromptQuestion& question)
{
	if (question.kind != PromptKind::Integer) return { question.defaultChoice };
	std::vector<int32_t> values;
	for (const auto& field : question.fields) {
		values.push_back(field.value);
	}
	return values;
}

bool PromptLegal(const PromptQuestion& question, const std::vector<int32_t>& values)
{
	switch (question.kind) {
		case PromptKind::YesNo:
			return values.size() == 1 && (values[0] == 0 || values[0] == 1);
		case PromptKind::Choice:
			return values.size() == 1 && values[0] >= 0 && size_t(values[0]) < question.choices.size();
		case PromptKind::Integer:
			if (values.size() != question.fields.size()) return false;
			for (size_t i = 0; i < values.size(); ++i) {
				if (values[i] < question.fields[i].min || values[i] > question.fields[i].max) return false;
			}
			return true;
		default:
			return false;
	}
}

std::string PromptAnswerText(const PromptQuestion& question, const std::vector<int32_t>& values)
{
	if (!PromptLegal(question, values)) return "?";
	switch (question.kind) {
		case PromptKind::YesNo:
			return values[0] ? "Yes" : "No";
		case PromptKind::Choice:
			return question.choices[size_t(values[0])];
		default:
			break;
	}
	std::string text;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i) text += ", ";
		const std::string& label = question.fields[i].label;
		text += label.empty() ? std::to_string(values[i]) : label + " " + std::to_string(values[i]);
	}
	return text;
}

const char* PromptOutcomeName(PromptOutcome outcome)
{
	switch (outcome) {
		case PromptOutcome::Answered:
			return "answered";
		case PromptOutcome::TimedOut:
			return "timed out";
		case PromptOutcome::HostAnswered:
			return "host answered";
		case PromptOutcome::Cancelled:
			return "cancelled";
		default:
			return "?";
	}
}

const char* PromptQueue::StatusName(Status status)
{
	switch (status) {
		case Status::Accepted:
			return "accepted";
		case Status::Closed:
			return "not open (answered, timed out or unknown)";
		case Status::NotAsked:
			return "not this player's to answer";
		case Status::Already:
			return "answered already";
		case Status::Illegal:
			return "not a legal answer";
		default:
			return "?";
	}
}

uint32_t PromptQueue::Ask(PromptQuestion question, std::vector<uint8_t> asked, uint64_t now)
{
	if (!PromptWellFormed(question)) return 0;
	bool vote = question.flags & PromptQuestion::VOTE;
	if (!vote) {
		// one player: the owner (the host for nobody's)
		uint8_t owner = asked.empty() ? PROMPT_LOCAL_SLOT : asked[0];
		asked = { owner };
	} else {
		std::sort(asked.begin(), asked.end());
		asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
		if (asked.size() > PROMPT_MAX_VOTERS) asked.resize(PROMPT_MAX_VOTERS);
	}
	Pending pending { nextID++, std::move(question), std::move(asked) };
	uint32_t id = pending.id;
	queue.push_back(std::move(pending));
	Pump(now);
	return id;
}

const PromptActive* PromptQueue::Active() const
{
	return open.empty() ? nullptr : &open.front();
}

const PromptActive* PromptQueue::Find(uint32_t id) const
{
	for (const auto& prompt : open) {
		if (prompt.id == id) return &prompt;
	}
	return nullptr;
}

std::vector<uint8_t> PromptQueue::Lane(const PromptActive& prompt)
{
	std::vector<uint8_t> lane;
	for (const auto& voter : prompt.voters) {
		lane.push_back(voter.slot);
	}
	return lane;
}

static bool Meets(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	for (uint8_t slot : a) {
		if (std::find(b.begin(), b.end(), slot) != b.end()) return true;
	}
	return false;
}

size_t PromptQueue::QueuedBehind(uint32_t id) const
{
	const PromptActive* prompt = Find(id);
	if (!prompt) return 0;
	std::vector<uint8_t> lane = Lane(*prompt);
	size_t count = 0;
	for (const auto& pending : queue) {
		if (Meets(pending.asked, lane)) ++count;
	}
	return count;
}

size_t PromptQueue::QueuedFor(uint8_t slot) const
{
	size_t count = 0;
	for (const auto& pending : queue) {
		if (std::find(pending.asked.begin(), pending.asked.end(), slot) != pending.asked.end()) ++count;
	}
	return count;
}

// opens, in order, every waiting prompt whose lane is free: not taken by an open prompt, nor
// by one waiting ahead of it (so each lane keeps its order)
void PromptQueue::Pump(uint64_t now)
{
	std::vector<uint8_t> taken;
	for (const auto& prompt : open) {
		for (uint8_t slot : Lane(prompt)) {
			taken.push_back(slot);
		}
	}
	for (auto it = queue.begin(); it != queue.end();) {
		// a vote nobody is in has no lane: it is decided as soon as it opens
		if (!Meets(it->asked, taken)) {
			Pending next = std::move(*it);
			it = queue.erase(it);
			taken.insert(taken.end(), next.asked.begin(), next.asked.end());
			Open(std::move(next), now);
			continue;
		}
		taken.insert(taken.end(), it->asked.begin(), it->asked.end());
		++it;
	}
}

void PromptQueue::Open(Pending pending, uint64_t now)
{
	PromptActive prompt;
	prompt.id = pending.id;
	prompt.question = std::move(pending.question);
	uint32_t timeout = prompt.question.timeout ? prompt.question.timeout : PROMPT_DEFAULT_TIMEOUT;
	prompt.deadline = now + timeout;
	for (uint8_t slot : pending.asked) {
		PromptVoter voter;
		voter.slot = slot;
		prompt.voters.push_back(voter);
	}
	Retarget(prompt);
	open.push_back(std::move(prompt));
	revision++;
	// a vote nobody else is in (or everyone is away) is decided at once
	if (Decided(open.back())) Finish(open.size() - 1, PromptOutcome::Answered);
}

// who answers for whom: single prompts go to the host while their owner is away, voters
// who are away are just marked so (they don't hold the vote up)
void PromptQueue::Retarget(PromptActive& prompt)
{
	bool vote = prompt.question.flags & PromptQuestion::VOTE;
	for (auto& voter : prompt.voters) {
		if (voter.state != PromptVoter::State::Waiting) continue;
		bool away = !Present(voter.slot);
		voter.away = away;
		voter.by = vote || !away ? voter.slot : PROMPT_LOCAL_SLOT;
	}
}

bool PromptQueue::Decided(const PromptActive& prompt) const
{
	bool vote = prompt.question.flags & PromptQuestion::VOTE;
	bool stopOnNo = prompt.question.flags & PromptQuestion::STOP_ON_NO;
	for (const auto& voter : prompt.voters) {
		if (vote && stopOnNo && voter.state == PromptVoter::State::Answered && voter.values == std::vector<int32_t> { 0 }) return true;
	}
	for (const auto& voter : prompt.voters) {
		if (voter.state != PromptVoter::State::Waiting) continue;
		if (vote && voter.away) continue;
		return false;
	}
	return true;
}

void PromptQueue::Finish(size_t index, PromptOutcome outcome)
{
	PromptActive& active = open[index];
	PromptResult result;
	result.id = active.id;
	result.question = active.question;
	result.outcome = outcome;
	std::vector<int32_t> fallback = PromptDefault(active.question);
	for (auto& voter : active.voters) {
		if (voter.state == PromptVoter::State::Waiting) {
			voter.state = PromptVoter::State::Defaulted;
			voter.values = fallback;
		}
	}
	result.voters = active.voters;

	if (active.question.flags & PromptQuestion::VOTE) {
		bool yes = true;
		for (const auto& voter : active.voters) {
			yes = yes && !voter.values.empty() && voter.values[0] != 0;
		}
		result.values = { yes ? 1 : 0 };
		result.slot = PROMPT_LOCAL_SLOT;
	} else if (!active.voters.empty()) {
		const PromptVoter& voter = active.voters[0];
		result.values = voter.values;
		result.slot = voter.by;
		if (outcome == PromptOutcome::Answered && voter.state == PromptVoter::State::Answered && voter.by != voter.slot) {
			result.outcome = PromptOutcome::HostAnswered;
		}
	} else {
		result.values = fallback;
	}
	finished.push_back(std::move(result));
	open.erase(open.begin() + std::ptrdiff_t(index));
	revision++;
}

PromptQueue::Status PromptQueue::Answer(uint32_t id, uint8_t slot, const std::vector<int32_t>& values, uint64_t now)
{
	size_t index = 0;
	while (index < open.size() && open[index].id != id) ++index;
	if (index == open.size()) return Status::Closed;
	if (now >= open[index].deadline) {
		// too late, even if Update hasn't noticed yet
		Update(now);
		return Status::Closed;
	}
	PromptActive& active = open[index];
	PromptVoter* mine = nullptr;
	bool answered = false;
	for (auto& voter : active.voters) {
		if (voter.by != slot) continue;
		if (voter.state == PromptVoter::State::Waiting) {
			mine = &voter;
			break;
		}
		answered = true;
	}
	if (!mine) return answered ? Status::Already : Status::NotAsked;
	if (!PromptLegal(active.question, values)) return Status::Illegal;
	mine->state = PromptVoter::State::Answered;
	mine->values = values;
	mine->away = false;
	revision++;
	if (Decided(active)) Finish(index, PromptOutcome::Answered);
	return Status::Accepted;
}

void PromptQueue::SetPresent(uint8_t slot, bool present)
{
	if (slot == PROMPT_LOCAL_SLOT) return;
	bool was = Present(slot);
	if (present) {
		absent.erase(slot);
	} else {
		absent.insert(slot);
	}
	if (was == present || open.empty()) return;
	for (auto& prompt : open) {
		Retarget(prompt);
	}
	revision++;
}

bool PromptQueue::Present(uint8_t slot) const
{
	return slot == PROMPT_LOCAL_SLOT || !absent.count(slot);
}

void PromptQueue::Update(uint64_t now)
{
	for (size_t i = 0; i < open.size();) {
		if (Decided(open[i])) {
			Finish(i, PromptOutcome::Answered);
		} else if (now >= open[i].deadline) {
			Finish(i, PromptOutcome::TimedOut);
		} else {
			++i;
		}
	}
	Pump(now);
}

bool PromptQueue::Cancel(uint32_t id)
{
	for (size_t i = 0; i < open.size(); ++i) {
		if (open[i].id != id) continue;
		Finish(i, PromptOutcome::Cancelled);
		return true;
	}
	for (auto it = queue.begin(); it != queue.end(); ++it) {
		if (it->id != id) continue;
		PromptResult result;
		result.id = it->id;
		result.question = it->question;
		result.values = PromptDefault(it->question);
		result.outcome = PromptOutcome::Cancelled;
		finished.push_back(std::move(result));
		queue.erase(it);
		return true;
	}
	return false;
}

void PromptQueue::Clear()
{
	if (!open.empty()) revision++;
	open.clear();
	queue.clear();
	finished.clear();
}

std::vector<PromptResult> PromptQueue::TakeFinished()
{
	std::vector<PromptResult> out;
	out.swap(finished);
	return out;
}

uint32_t PromptQueue::Remaining(uint64_t now) const
{
	return open.empty() ? 0 : Remaining(open.front().id, now);
}

uint32_t PromptQueue::Remaining(uint32_t id, uint64_t now) const
{
	const PromptActive* prompt = Find(id);
	if (!prompt || now >= prompt->deadline) return 0;
	return uint32_t(prompt->deadline - now);
}

}
