// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../core/Net/Prompt.h"

#include <gtest/gtest.h>

namespace GemRB {

static PromptQuestion YesNo(int32_t fallback = 0, uint32_t timeout = 10000)
{
	PromptQuestion q;
	q.kind = PromptKind::YesNo;
	q.tag = "test";
	q.title = "Go?";
	q.defaultChoice = fallback;
	q.timeout = timeout;
	return q;
}

static PromptQuestion LuckBurn(int32_t luck, uint32_t timeout = 10000)
{
	PromptQuestion q;
	q.kind = PromptKind::Integer;
	q.tag = "luckburn";
	q.title = "Burn Luck?";
	q.fields = { { "Luck", 0, luck, 0 } };
	q.timeout = timeout;
	return q;
}

static PromptQuestion Vote(uint32_t timeout = 10000)
{
	PromptQuestion q = YesNo(1, timeout);
	q.flags = PromptQuestion::VOTE | PromptQuestion::STOP_ON_NO;
	q.tag = "vote";
	return q;
}

TEST(NetPromptTest, WellFormedAndLegal)
{
	EXPECT_TRUE(PromptWellFormed(YesNo()));
	PromptQuestion bad = YesNo(2);
	EXPECT_FALSE(PromptWellFormed(bad));

	PromptQuestion choice;
	choice.kind = PromptKind::Choice;
	choice.choices = { "Counter", "Ignore" };
	choice.defaultChoice = 1;
	EXPECT_TRUE(PromptWellFormed(choice));
	choice.defaultChoice = 2;
	EXPECT_FALSE(PromptWellFormed(choice));
	choice.defaultChoice = 0;
	choice.choices = { "alone" };
	EXPECT_FALSE(PromptWellFormed(choice));
	choice.choices = std::vector<std::string>(PROMPT_MAX_CHOICES + 1, "x");
	EXPECT_FALSE(PromptWellFormed(choice));

	PromptQuestion luck = LuckBurn(3);
	EXPECT_TRUE(PromptWellFormed(luck));
	EXPECT_TRUE(PromptLegal(luck, { 0 }));
	EXPECT_TRUE(PromptLegal(luck, { 3 }));
	EXPECT_FALSE(PromptLegal(luck, { 4 }));
	EXPECT_FALSE(PromptLegal(luck, { -1 }));
	EXPECT_FALSE(PromptLegal(luck, { 1, 1 }));
	EXPECT_EQ(PromptAnswerText(luck, { 2 }), "Luck 2");
	luck.fields[0].value = 5;
	EXPECT_FALSE(PromptWellFormed(luck));

	PromptQuestion burn;
	burn.kind = PromptKind::Integer;
	burn.fields = { { "Str", 0, 3, 0 }, { "Agi", 0, 3, 1 }, { "Sta", 0, 3, 0 } };
	EXPECT_TRUE(PromptWellFormed(burn));
	EXPECT_EQ(PromptDefault(burn), (std::vector<int32_t> { 0, 1, 0 }));
	EXPECT_TRUE(PromptLegal(burn, { 1, 2, 3 }));
	EXPECT_FALSE(PromptLegal(burn, { 1, 2 }));
	EXPECT_EQ(PromptAnswerText(burn, { 1, 0, 2 }), "Str 1, Agi 0, Sta 2");

	// votes are yes/no only
	PromptQuestion vote = Vote();
	EXPECT_TRUE(PromptWellFormed(vote));
	burn.flags = PromptQuestion::VOTE;
	EXPECT_FALSE(PromptWellFormed(burn));
	EXPECT_FALSE(PromptLegal(YesNo(), { 2 }));
	EXPECT_EQ(PromptAnswerText(YesNo(), { 1 }), "Yes");
}

TEST(NetPromptTest, OwnerAnswers)
{
	PromptQueue queue;
	uint32_t id = queue.Ask(LuckBurn(3), { 1 }, 1000);
	ASSERT_NE(id, 0U);
	ASSERT_NE(queue.Active(), nullptr);
	EXPECT_EQ(queue.Active()->voters[0].by, 1);
	EXPECT_EQ(queue.Remaining(1000), 10000U);

	// only the asked player, a legal value, the open prompt
	EXPECT_EQ(queue.Answer(id, 2, { 1 }, 2000), PromptQueue::Status::NotAsked);
	EXPECT_EQ(queue.Answer(id, 0, { 1 }, 2000), PromptQueue::Status::NotAsked);
	EXPECT_EQ(queue.Answer(id, 1, { 9 }, 2000), PromptQueue::Status::Illegal);
	EXPECT_EQ(queue.Answer(id + 1, 1, { 1 }, 2000), PromptQueue::Status::Closed);
	EXPECT_TRUE(queue.TakeFinished().empty());

	EXPECT_EQ(queue.Answer(id, 1, { 2 }, 2000), PromptQueue::Status::Accepted);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_EQ(done[0].id, id);
	EXPECT_EQ(done[0].Value(), 2);
	EXPECT_EQ(done[0].slot, 1);
	EXPECT_EQ(done[0].outcome, PromptOutcome::Answered);
	EXPECT_EQ(queue.Active(), nullptr);
	// late answers find it closed
	EXPECT_EQ(queue.Answer(id, 1, { 1 }, 2100), PromptQueue::Status::Closed);
}

TEST(NetPromptTest, TimeoutGivesTheDefault)
{
	PromptQueue queue;
	uint32_t id = queue.Ask(YesNo(1, 5000), { 2 }, 0);
	queue.Update(4999);
	EXPECT_NE(queue.Active(), nullptr);
	EXPECT_EQ(queue.Remaining(4000), 1000U);
	queue.Update(5000);
	EXPECT_EQ(queue.Active(), nullptr);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_EQ(done[0].outcome, PromptOutcome::TimedOut);
	EXPECT_TRUE(done[0].Yes());
	EXPECT_EQ(done[0].voters[0].state, PromptVoter::State::Defaulted);
	// an answer that comes in after the deadline is too late, even before Update noticed
	uint32_t second = queue.Ask(LuckBurn(2, 1000), { 2 }, 10000);
	EXPECT_EQ(queue.Answer(second, 2, { 2 }, 11000), PromptQueue::Status::Closed);
	done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_EQ(done[0].outcome, PromptOutcome::TimedOut);
	EXPECT_EQ(done[0].Value(), 0);
	EXPECT_NE(id, second);
}

TEST(NetPromptTest, AwayOwnerFallsBackToTheHost)
{
	PromptQueue queue;
	queue.SetPresent(1, false);
	uint32_t id = queue.Ask(LuckBurn(3), { 1 }, 0);
	EXPECT_EQ(queue.Active()->voters[0].by, PROMPT_LOCAL_SLOT);
	EXPECT_EQ(queue.Answer(id, 1, { 1 }, 100), PromptQueue::Status::NotAsked);

	// the owner comes back: theirs again, the clock keeps running
	uint32_t before = queue.Revision();
	queue.SetPresent(1, true);
	EXPECT_GT(queue.Revision(), before);
	EXPECT_EQ(queue.Active()->voters[0].by, 1);
	EXPECT_EQ(queue.Remaining(4000), 6000U);
	EXPECT_EQ(queue.Answer(id, 0, { 1 }, 4000), PromptQueue::Status::NotAsked);

	// and leaves again mid-prompt: the host answers for them
	queue.SetPresent(1, false);
	EXPECT_EQ(queue.Answer(id, 0, { 3 }, 5000), PromptQueue::Status::Accepted);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_EQ(done[0].outcome, PromptOutcome::HostAnswered);
	EXPECT_EQ(done[0].slot, PROMPT_LOCAL_SLOT);
	EXPECT_EQ(done[0].Value(), 3);
	EXPECT_EQ(done[0].voters[0].slot, 1);
}

TEST(NetPromptTest, OneAtATimeInOrder)
{
	PromptQueue queue;
	uint32_t a = queue.Ask(YesNo(0, 1000), { 1 }, 0);
	uint32_t b = queue.Ask(LuckBurn(2, 1000), { 1 }, 500);
	EXPECT_EQ(queue.Queued(), 1U);
	EXPECT_EQ(queue.Active()->id, a);
	EXPECT_EQ(queue.QueuedBehind(a), 1U);
	EXPECT_EQ(queue.Answer(b, 1, { 1 }, 600), PromptQueue::Status::Closed);
	EXPECT_EQ(queue.Answer(a, 1, { 1 }, 700), PromptQueue::Status::Accepted);
	// the next one opens on the next update, with its full time from then
	queue.Update(800);
	ASSERT_NE(queue.Active(), nullptr);
	EXPECT_EQ(queue.Active()->id, b);
	EXPECT_EQ(queue.Remaining(800), 1000U);
	EXPECT_TRUE(queue.Cancel(b));
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 2U);
	EXPECT_EQ(done[0].id, a);
	EXPECT_EQ(done[1].id, b);
	EXPECT_EQ(done[1].outcome, PromptOutcome::Cancelled);
	EXPECT_FALSE(queue.Busy());
}

// M2.10: each player's prompts open alongside the others'; one at a time per player, in order
TEST(NetPromptTest, OnePerPlayerAtOnce)
{
	PromptQueue queue;
	uint32_t host = queue.Ask(YesNo(0, 1000), { 0 }, 0);
	uint32_t one = queue.Ask(LuckBurn(2, 1000), { 1 }, 100);
	uint32_t two = queue.Ask(YesNo(0, 1000), { 2 }, 200);
	uint32_t oneAgain = queue.Ask(YesNo(1, 1000), { 1 }, 300);
	ASSERT_EQ(queue.Open().size(), 3U);
	EXPECT_EQ(queue.Open()[0].id, host);
	EXPECT_EQ(queue.Open()[1].id, one);
	EXPECT_EQ(queue.Open()[2].id, two);
	EXPECT_EQ(queue.Queued(), 1U);
	EXPECT_EQ(queue.QueuedBehind(one), 1U);
	EXPECT_EQ(queue.QueuedBehind(two), 0U);
	EXPECT_EQ(queue.QueuedFor(1), 1U);
	EXPECT_EQ(queue.QueuedFor(2), 0U);
	// each clock started when it opened
	EXPECT_EQ(queue.Remaining(two, 500), 700U);
	// player 2 answers its own while the others are still open; nobody answers another's
	EXPECT_EQ(queue.Answer(one, 2, { 1 }, 400), PromptQueue::Status::NotAsked);
	EXPECT_EQ(queue.Answer(two, 2, { 1 }, 400), PromptQueue::Status::Accepted);
	EXPECT_EQ(queue.Open().size(), 2U);
	// player 1's second waits for its first, then opens with its own full time
	EXPECT_EQ(queue.Answer(oneAgain, 1, { 1 }, 450), PromptQueue::Status::Closed);
	EXPECT_EQ(queue.Answer(one, 1, { 2 }, 500), PromptQueue::Status::Accepted);
	queue.Update(600);
	ASSERT_NE(queue.Find(oneAgain), nullptr);
	EXPECT_EQ(queue.Remaining(oneAgain, 600), 1000U);
	EXPECT_EQ(queue.Queued(), 0U);
	// the host's timed out meanwhile
	queue.Update(1000);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 3U);
	EXPECT_EQ(done[0].id, two);
	EXPECT_EQ(done[1].id, one);
	EXPECT_EQ(done[1].Value(), 2);
	EXPECT_EQ(done[2].id, host);
	EXPECT_EQ(done[2].outcome, PromptOutcome::TimedOut);
	EXPECT_TRUE(queue.Busy());
	EXPECT_TRUE(queue.Cancel(oneAgain));
	EXPECT_FALSE(queue.Busy());
}

TEST(NetPromptTest, LanesKeepTheirOrder)
{
	PromptQueue queue;
	// a vote takes every voter's lane: it waits for player 1's open prompt, and player 2's next
	// prompt waits behind the vote, not before it
	uint32_t one = queue.Ask(YesNo(0, 1000), { 1 }, 0);
	uint32_t vote = queue.Ask(Vote(1000), { 1, 2 }, 0);
	uint32_t two = queue.Ask(YesNo(0, 1000), { 2 }, 0);
	uint32_t three = queue.Ask(YesNo(0, 1000), { 3 }, 0);
	ASSERT_EQ(queue.Open().size(), 2U);
	EXPECT_EQ(queue.Open()[0].id, one);
	EXPECT_EQ(queue.Open()[1].id, three);
	EXPECT_EQ(queue.QueuedBehind(one), 1U); // the vote
	EXPECT_EQ(queue.QueuedFor(2), 2U); // the vote, and player 2's own behind it
	EXPECT_EQ(queue.Answer(one, 1, { 1 }, 10), PromptQueue::Status::Accepted);
	queue.Update(20);
	ASSERT_EQ(queue.Open().size(), 2U);
	EXPECT_EQ(queue.Open()[1].id, vote);
	EXPECT_EQ(queue.Find(two), nullptr);
	EXPECT_EQ(queue.Answer(vote, 1, { 1 }, 30), PromptQueue::Status::Accepted);
	EXPECT_EQ(queue.Answer(vote, 2, { 1 }, 40), PromptQueue::Status::Accepted);
	queue.Update(50);
	EXPECT_NE(queue.Find(two), nullptr);
	// an owner who is away: the host answers that lane's prompt alongside its own
	uint32_t hosts = queue.Ask(YesNo(0, 1000), { 0 }, 60);
	queue.SetPresent(2, false);
	EXPECT_EQ(queue.Find(two)->voters[0].by, PROMPT_LOCAL_SLOT);
	EXPECT_EQ(queue.Answer(two, 0, { 1 }, 70), PromptQueue::Status::Accepted);
	EXPECT_EQ(queue.Answer(hosts, 0, { 0 }, 80), PromptQueue::Status::Accepted);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 4U);
	EXPECT_EQ(done[2].id, two);
	EXPECT_EQ(done[2].outcome, PromptOutcome::HostAnswered);
	EXPECT_EQ(done[3].id, hosts);
	EXPECT_EQ(queue.Open().size(), 1U); // player 3's
}

TEST(NetPromptTest, Votes)
{
	PromptQueue queue;
	// everyone says yes
	uint32_t id = queue.Ask(Vote(), { 2, 1, 1 }, 0);
	EXPECT_EQ(queue.Active()->voters.size(), 2U);
	EXPECT_EQ(queue.Answer(id, 1, { 1 }, 10), PromptQueue::Status::Accepted);
	EXPECT_EQ(queue.Answer(id, 1, { 0 }, 20), PromptQueue::Status::Already);
	EXPECT_EQ(queue.Answer(id, 3, { 1 }, 20), PromptQueue::Status::NotAsked);
	EXPECT_NE(queue.Active(), nullptr);
	EXPECT_EQ(queue.Answer(id, 2, { 1 }, 30), PromptQueue::Status::Accepted);
	auto done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_TRUE(done[0].Yes());

	// one no ends it at once
	id = queue.Ask(Vote(), { 1, 2 }, 100);
	EXPECT_EQ(queue.Answer(id, 2, { 0 }, 110), PromptQueue::Status::Accepted);
	done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_FALSE(done[0].Yes());
	EXPECT_EQ(done[0].voters[0].state, PromptVoter::State::Defaulted);

	// no answer counts as the default (yes), someone away doesn't hold it up
	id = queue.Ask(Vote(1000), { 1, 2 }, 200);
	queue.SetPresent(2, false);
	EXPECT_EQ(queue.Answer(id, 1, { 1 }, 300), PromptQueue::Status::Accepted);
	done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_TRUE(done[0].Yes());
	EXPECT_EQ(done[0].outcome, PromptOutcome::Answered);

	id = queue.Ask(Vote(1000), { 1 }, 400);
	queue.Update(1400);
	done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_EQ(done[0].outcome, PromptOutcome::TimedOut);
	EXPECT_TRUE(done[0].Yes());

	// nobody to ask: decided right away
	id = queue.Ask(Vote(), {}, 2000);
	EXPECT_NE(id, 0U);
	done = queue.TakeFinished();
	ASSERT_EQ(done.size(), 1U);
	EXPECT_TRUE(done[0].Yes());
}

TEST(NetPromptTest, MalformedIsNotAsked)
{
	PromptQueue queue;
	EXPECT_EQ(queue.Ask(YesNo(3), { 1 }, 0), 0U);
	PromptQuestion slow = YesNo();
	slow.timeout = PROMPT_MAX_TIMEOUT + 1;
	EXPECT_EQ(queue.Ask(slow, { 1 }, 0), 0U);
	EXPECT_FALSE(queue.Busy());
}

}
