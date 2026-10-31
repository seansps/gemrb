// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file Prompts.h
 * "Ask the owning player": the game (rules code, scripts, travel) asks the player who
 * owns a party member a question and carries on with the answer on a later frame.
 *
 * The same in single player (the local player owns everyone, no network) and in co-op,
 * where the host asks and the owner's client answers. The game pauses while any prompt
 * is open. Each player has one open at a time and the rest of theirs wait in order (M2.10:
 * in co-op, different players' prompts are open at the same time, see PromptQueue). Timeouts
 * answer with the default, and the host answers for an owner who is away (BG: a leaving
 * player's characters are the host's until they return).
 *
 *     PromptQuestion q;
 *     q.kind = PromptKind::Integer;
 *     q.tag = "luckburn";
 *     q.title = "Burn Luck?";
 *     q.text = "Burn Luck to turn this miss into a hit?";
 *     q.fields = { { "Luck", 0, luck, 0 } };
 *     core->GetPrompts().Ask(attacker, std::move(q), [attackerID, targetID](const PromptResult& r) {
 *         if (r.Value() > 0) ...; // look both actors up again by ID, they may be gone
 *     });
 */

#ifndef PROMPTS_H
#define PROMPTS_H

#include "exports.h"

#include "Net/Prompt.h"
#include "Net/Protocol.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GemRB {

class Actor;
class Game;
class InfoPoint;
struct NetPrompt;
struct NetPromptAnswer;

class GEM_EXPORT PromptManager {
public:
	using Callback = std::function<void(const PromptResult&)>;

	// the prompt our player sees: the oldest open one it answers, else (read only) the oldest open one
	struct View {
		uint32_t id = 0;
		PromptQuestion question; // actorID is a local global ID
		bool mine = false; // we answer it: the prompt window; else read only, "Waiting for X to decide"
		bool sent = false; // client: our answer is on its way to the host
		std::string waitingFor; // who decides, eg. "client-one" or "client-one, client-two" (read only: of every open prompt)
		std::string askedName; // whose decision it is, when the host answers for someone who is away
		uint64_t deadline = 0; // local clock (ms)
		size_t queued = 0; // ours: our other prompts, open or waiting behind this one (one window at a time)
		size_t open = 0; // prompts open in the game now (M2.10: one per player at most)
	};

	PromptManager() = default;
	PromptManager(const PromptManager&) = delete;
	PromptManager& operator=(const PromptManager&) = delete;

	/** host or single player: ask the player who owns actor (nullptr or a non party member:
	 * the host, the local player in single player). done runs once, on a later tick, with the
	 * answer, the default on a timeout, or the host's answer for an owner who is away; it
	 * doesn't run if the game it was asked in is gone. Returns the prompt's id, 0 if refused
	 * (a malformed question, or a client: only the host asks). */
	uint32_t Ask(const Actor* actor, PromptQuestion question, Callback done);
	/** a yes/no vote of these players (Players() for everyone); the result is yes only if
	 * every one of them says yes, no answer counts as the default */
	uint32_t AskPlayers(std::vector<uint8_t> slots, PromptQuestion question, Callback done);
	/** everyone in the game: the connected players, host first; {0} in single player */
	static std::vector<uint8_t> Players();
	bool Cancel(uint32_t id);
	/** a prompt is open or waiting */
	bool Busy() const;

	/** once per game loop tick, after the network's PreTick (so on the host its callbacks
	 * run in the simulation window and what they do is replicated) */
	void Update();

	/** what our player sees, nullptr if no prompt is open */
	const View* Current() const;
	uint32_t Remaining() const;
	/** the prompt window is up and has the keys: numbers pick its choices, not dialog replies */
	bool TakesKeys() const;
	/** our player's answer, from the prompt window; false if it isn't ours or isn't legal */
	bool AnswerLocal(uint32_t id, const std::vector<int32_t>& values);

	// co-op
	/** client: the host's word on the open prompt (actorID already local) */
	void OnRemotePrompt(const NetPrompt& msg);
	/** host: a player's answer */
	void OnRemoteAnswer(uint8_t slot, const NetPromptAnswer& msg);
	/** host: the open prompts as messages, for a player who just loaded the game */
	std::vector<NetPrompt> OpenMessages() const;
	/** client: we left the game or the session */
	void Reset();
	/** the engine shuts down: everything is dropped, no callbacks run */
	void Shutdown();

	/** host: a party member is about to travel (Map::UseExit); true holds it back while the
	 * travel prompts (NetTravelConfirm, NetTravelVote) are asked, and the answer moves the party on */
	bool HoldTravel(Actor* actor, InfoPoint* exit);

	/** test orders "ask ..." and "prompt ..." (NetTestOrders in co-op, TestPrompts in single
	 * player); false if the order isn't one of these */
	bool RunTestOrder(const std::string& order);

private:
	struct Asked {
		Callback done;
		const Game* game = nullptr;
	};
	struct Travel {
		bool pending = false;
		bool approved = false;
		const Game* game = nullptr;
		uint32_t actorID = 0;
		uint32_t exitID = 0;
		uint8_t owner = 0;
		std::string area;
		std::string destination;
	};

	uint32_t AskSlots(std::vector<uint8_t> slots, PromptQuestion question, Callback done);
	void UpdatePresence();
	void UpdatePause();
	void Finished(const PromptResult& result);
	void Broadcast();
	void BuildView();
	void ViewFrom(const std::vector<NetPrompt>& prompts, const std::vector<uint64_t>& deadlines, uint8_t me);
	NetPrompt Message(const PromptActive& active) const;
	std::string OutcomeText(const PromptResult& result) const;
	static std::string Name(uint8_t slot);
	void ShowLine(const std::string& text) const;
	void Notify();
	void RunTestPrompts();
	void TestKey(const std::string& key) const;
	void TravelVote();
	void ResumeTravel(bool go, const std::string& why);

	PromptQueue queue;
	std::map<uint32_t, Asked> asked;
	uint32_t sentRevision = 0;
	size_t sentQueued = 0;
	// client: the open prompts as the host sent them, with their local deadlines; ours answered
	struct Remote {
		NetPrompt msg;
		uint64_t deadline = 0;
	};
	std::map<uint32_t, Remote> remote;
	std::set<uint32_t> answered;
	View view;
	bool haveView = false;
	bool guiDirty = false;
	bool pausedByUs = false;
	// the key that answered still goes to the event taps after the window (the dialog's numbered
	// replies): we keep the keys until the next tick
	bool keyGrace = false;
	Travel travel;
	unsigned int testTicks = 0;
};

}

#endif
