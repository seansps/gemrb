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
	if (!active.id && queue.empty()) {
		Open(std::move(pending), now);
	} else {
		queue.push_back(std::move(pending));
	}
	return id;
}

const PromptActive* PromptQueue::Active() const
{
	return active.id ? &active : nullptr;
}

void PromptQueue::Open(Pending pending, uint64_t now)
{
	active = PromptActive();
	active.id = pending.id;
	active.question = std::move(pending.question);
	uint32_t timeout = active.question.timeout ? active.question.timeout : PROMPT_DEFAULT_TIMEOUT;
	active.deadline = now + timeout;
	for (uint8_t slot : pending.asked) {
		PromptVoter voter;
		voter.slot = slot;
		active.voters.push_back(voter);
	}
	Retarget();
	revision++;
	// a vote nobody else is in (or everyone is away) is decided at once
	if (Decided()) Finish(PromptOutcome::Answered);
}

// who answers for whom: single prompts go to the host while their owner is away, voters
// who are away are just marked so (they don't hold the vote up)
void PromptQueue::Retarget()
{
	bool vote = active.question.flags & PromptQuestion::VOTE;
	for (auto& voter : active.voters) {
		if (voter.state != PromptVoter::State::Waiting) continue;
		bool away = !Present(voter.slot);
		voter.away = away;
		voter.by = vote || !away ? voter.slot : PROMPT_LOCAL_SLOT;
	}
}

bool PromptQueue::Decided() const
{
	if (!active.id) return false;
	bool vote = active.question.flags & PromptQuestion::VOTE;
	bool stopOnNo = active.question.flags & PromptQuestion::STOP_ON_NO;
	for (const auto& voter : active.voters) {
		if (vote && stopOnNo && voter.state == PromptVoter::State::Answered && voter.values == std::vector<int32_t> { 0 }) return true;
	}
	for (const auto& voter : active.voters) {
		if (voter.state != PromptVoter::State::Waiting) continue;
		if (vote && voter.away) continue;
		return false;
	}
	return true;
}

void PromptQueue::Finish(PromptOutcome outcome)
{
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
	active = PromptActive();
	revision++;
}

PromptQueue::Status PromptQueue::Answer(uint32_t id, uint8_t slot, const std::vector<int32_t>& values, uint64_t now)
{
	if (!active.id || id != active.id) return Status::Closed;
	if (now >= active.deadline) {
		// too late, even if Update hasn't noticed yet
		Update(now);
		return Status::Closed;
	}
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
	if (Decided()) Finish(PromptOutcome::Answered);
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
	if (was == present || !active.id) return;
	Retarget();
	revision++;
}

bool PromptQueue::Present(uint8_t slot) const
{
	return slot == PROMPT_LOCAL_SLOT || !absent.count(slot);
}

void PromptQueue::Update(uint64_t now)
{
	if (active.id && Decided()) {
		Finish(PromptOutcome::Answered);
	} else if (active.id && now >= active.deadline) {
		Finish(PromptOutcome::TimedOut);
	}
	while (!active.id && !queue.empty()) {
		Pending next = std::move(queue.front());
		queue.pop_front();
		Open(std::move(next), now);
	}
}

bool PromptQueue::Cancel(uint32_t id)
{
	if (active.id && active.id == id) {
		Finish(PromptOutcome::Cancelled);
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
	if (active.id) revision++;
	active = PromptActive();
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
	if (!active.id || now >= active.deadline) return 0;
	return uint32_t(active.deadline - now);
}

}
