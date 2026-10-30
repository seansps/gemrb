// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Prompts.h"

#include "DisplayMessage.h"
#include "Game.h"
#include "Interface.h"
#include "Map.h"
#include "ScriptEngine.h"

#include "GUI/EventMgr.h"
#include "GUI/GameControl.h"
#include "GUI/WindowManager.h"
#include "Logging/Logging.h"
#include "Net/NetSession.h"
#include "Net/Protocol.h"
#include "Scriptable/Actor.h"
#include "Scriptable/InfoPoint.h"
#include "Strings/StringConversion.h"

#include <algorithm>
#include <sstream>

namespace GemRB {

// the outcome line in everyone's message window
static constexpr const char* OUTCOME_COLOR = "E0C890FF";

static uint64_t Now()
{
	return GetMilliseconds();
}

static NetSession* Host()
{
	NetSession* net = core->GetNetSession();
	return net && net->IsHost() ? net : nullptr;
}

static bool IsClient()
{
	const NetSession* net = core->GetNetSession();
	return net && !net->IsHost();
}

static std::string ActorName(const Actor* actor)
{
	if (!actor) return "nobody";
	return RecodedStringFromWideStringBytes(actor->GetShortName().data(), actor->GetShortName().size() * sizeof(char16_t), "UTF-8");
}

uint32_t PromptManager::Ask(const Actor* actor, PromptQuestion question, Callback done)
{
	if (IsClient()) {
		Log(WARNING, "Prompt", "Only the host asks; ignoring prompt '{}'.", question.tag);
		return 0;
	}
	uint8_t owner = PROMPT_LOCAL_SLOT;
	if (actor) {
		question.actorID = actor->GetGlobalID();
		if (NetSession* net = Host()) owner = net->OwnerSlot(*actor);
	}
	question.flags &= ~PromptQuestion::VOTE;
	return AskSlots({ owner }, std::move(question), std::move(done));
}

uint32_t PromptManager::AskPlayers(std::vector<uint8_t> slots, PromptQuestion question, Callback done)
{
	if (IsClient()) {
		Log(WARNING, "Prompt", "Only the host asks; ignoring vote '{}'.", question.tag);
		return 0;
	}
	question.flags |= PromptQuestion::VOTE;
	return AskSlots(std::move(slots), std::move(question), std::move(done));
}

std::vector<uint8_t> PromptManager::Players()
{
	if (const NetSession* net = Host()) return net->ConnectedSlots();
	return { PROMPT_LOCAL_SLOT };
}

uint32_t PromptManager::AskSlots(std::vector<uint8_t> slots, PromptQuestion question, Callback done)
{
	if (!question.timeout) question.timeout = uint32_t(Clamp<int>(core->config.PromptTimeout, 1000, int(PROMPT_MAX_TIMEOUT)));
	UpdatePresence();
	std::string tag = question.tag;
	std::string title = question.title;
	uint32_t id = queue.Ask(std::move(question), slots, Now());
	if (!id) {
		Log(ERROR, "Prompt", "Malformed prompt '{}' ('{}'), not asked.", tag, title);
		return 0;
	}
	asked[id] = Asked { std::move(done), core->GetGame() };
	std::string who;
	for (uint8_t slot : slots) {
		who += fmt::format("{}{}", who.empty() ? "" : ", ", slot);
	}
	const PromptActive* active = queue.Active();
	Log(MESSAGE, "Prompt", "Prompt #{} '{}' for player slot(s) {}: '{}'{}.", id, tag, who, title, active && active->id == id ? "" : fmt::format(", waits behind #{}", active ? active->id : 0));
	UpdatePause();
	return id;
}

bool PromptManager::Cancel(uint32_t id)
{
	if (IsClient()) return false;
	return queue.Cancel(id);
}

bool PromptManager::Busy() const
{
	if (IsClient()) return haveView;
	return queue.Busy();
}

void PromptManager::Update()
{
	keyGrace = false;
	if (IsClient()) {
		if (guiDirty) Notify();
		return;
	}
	if (!core->GetNetSession()) {
		RunTestPrompts();
	}
	// prompts belong to the game they were asked in
	if (queue.Busy()) {
		const Game* game = core->GetGame();
		std::vector<uint32_t> stale;
		for (const auto& entry : asked) {
			if (entry.second.game != game) stale.push_back(entry.first);
		}
		for (uint32_t id : stale) {
			Log(MESSAGE, "Prompt", "Prompt #{}: its game is gone, called off.", id);
			queue.Cancel(id);
		}
	}
	UpdatePresence();
	queue.Update(Now());
	for (const auto& result : queue.TakeFinished()) {
		Finished(result);
	}
	if (queue.Revision() != sentRevision) {
		sentRevision = queue.Revision();
		BuildView();
		Broadcast();
		guiDirty = true;
	}
	UpdatePause();
	if (guiDirty) Notify();
}

// the host: who is connected decides who answers (an absent owner's prompt is the host's)
void PromptManager::UpdatePresence()
{
	const NetSession* net = Host();
	if (!net) return;
	std::vector<uint8_t> connected = net->ConnectedSlots();
	for (uint8_t slot = 1; slot <= NET_MAX_PEERS; ++slot) {
		bool present = std::find(connected.begin(), connected.end(), slot) != connected.end();
		if (queue.Present(slot) != present && queue.Active()) {
			Log(MESSAGE, "Prompt", "Player slot {} {} during prompt #{}.", slot, present ? "is back" : "is away", queue.Active()->id);
		}
		queue.SetPresent(slot, present);
	}
}

// the game holds still while it waits; only what we paused is unpaused again
void PromptManager::UpdatePause()
{
	if (queue.Busy()) {
		const GameControl* gc = core->GetGameControl();
		if (pausedByUs || !gc || (gc->GetDialogueFlags() & DF_FREEZE_SCRIPTS)) return;
		pausedByUs = core->SetPause(PauseState::On, PF_QUIET);
		if (pausedByUs) Log(MESSAGE, "Prompt", "Paused the game while prompt #{} waits.", queue.Active() ? queue.Active()->id : 0);
	} else if (pausedByUs) {
		pausedByUs = false;
		if (core->GetGameControl()) {
			core->SetPause(PauseState::Off, PF_QUIET);
			Log(MESSAGE, "Prompt", "No prompt waits any more, unpaused the game.");
		}
	}
}

std::string PromptManager::Name(uint8_t slot)
{
	const NetSession* net = core->GetNetSession();
	if (!net) return "You";
	std::string name = net->PlayerName(slot);
	return name.empty() ? fmt::format("Player {}", slot) : name;
}

std::string PromptManager::OutcomeText(const PromptResult& result) const
{
	const PromptQuestion& q = result.question;
	std::string answer = PromptAnswerText(q, result.values);
	std::string title = q.title.empty() ? q.tag : q.title;
	bool session = core->GetNetSession() != nullptr;
	if (q.flags & PromptQuestion::VOTE) {
		std::string votes;
		for (const auto& voter : result.voters) {
			std::string said = voter.state == PromptVoter::State::Answered ? PromptAnswerText(q, voter.values) : fmt::format("no answer ({})", PromptAnswerText(q, voter.values));
			votes += fmt::format("{}{} {}", votes.empty() ? "" : ", ", Name(voter.slot), said);
		}
		if (result.outcome == PromptOutcome::Cancelled) return fmt::format("{}: called off.", title);
		return fmt::format("{}: {} ({}).", title, result.Yes() ? "everyone agreed" : "not everyone agreed", votes.empty() ? "nobody else to ask" : votes);
	}
	uint8_t owner = result.voters.empty() ? PROMPT_LOCAL_SLOT : result.voters[0].slot;
	switch (result.outcome) {
		case PromptOutcome::Answered:
			return session ? fmt::format("{}: {} decided {}.", title, Name(result.slot), answer) : fmt::format("{}: {}.", title, answer);
		case PromptOutcome::HostAnswered:
			return fmt::format("{}: {} is away, {} decided {}.", title, Name(owner), Name(result.slot), answer);
		case PromptOutcome::TimedOut:
			return session ? fmt::format("{}: no answer from {} in time, {} (the default).", title, Name(owner), answer) : fmt::format("{}: no answer in time, {} (the default).", title, answer);
		case PromptOutcome::Cancelled:
		default:
			return fmt::format("{}: called off.", title);
	}
}

void PromptManager::Finished(const PromptResult& result)
{
	std::string text = OutcomeText(result);
	Log(MESSAGE, "Prompt", "Prompt #{} '{}' {}: {} -> {}", result.id, result.question.tag, PromptOutcomeName(result.outcome), PromptAnswerText(result.question, result.values), text);
	if (NetSession* net = Host()) {
		NetPrompt msg;
		msg.id = result.id;
		msg.state = NetPrompt::CLOSED;
		msg.question = result.question;
		for (const auto& voter : result.voters) {
			NetPromptVoter entry;
			entry.slot = voter.slot;
			entry.by = voter.by;
			entry.state = uint8_t(voter.state);
			entry.away = voter.away;
			entry.name = Name(voter.slot);
			entry.byName = Name(voter.by);
			msg.voters.push_back(entry);
		}
		msg.outcome = uint8_t(result.outcome);
		msg.values = result.values;
		msg.outcomeText = text;
		net->SendPrompt(msg);
	}
	ShowLine(text);

	auto it = asked.find(result.id);
	if (it == asked.end()) return;
	Asked entry = std::move(it->second);
	asked.erase(it);
	// what the prompt was about may be gone with its game
	if (entry.done && entry.game == core->GetGame()) {
		entry.done(result);
	}
}

NetPrompt PromptManager::Message() const
{
	NetPrompt msg;
	const PromptActive* active = queue.Active();
	if (!active) return msg;
	msg.id = active->id;
	msg.state = NetPrompt::OPEN;
	msg.remaining = queue.Remaining(Now());
	msg.question = active->question;
	for (const auto& voter : active->voters) {
		NetPromptVoter entry;
		entry.slot = voter.slot;
		entry.by = voter.by;
		entry.state = uint8_t(voter.state);
		entry.away = voter.away;
		entry.name = Name(voter.slot);
		entry.byName = Name(voter.by);
		msg.voters.push_back(entry);
	}
	return msg;
}

bool PromptManager::OpenMessage(NetPrompt& msg) const
{
	if (!queue.Active()) return false;
	msg = Message();
	return true;
}

void PromptManager::Broadcast()
{
	NetSession* net = Host();
	if (!net || !queue.Active()) return;
	NetPrompt msg = Message();
	net->SendPrompt(msg);
	std::string list;
	for (const auto& voter : msg.voters) {
		list += fmt::format(" [slot {} answered by {}{}, {}]", voter.slot, voter.by, voter.away ? ", away" : "", voter.state == 0 ? "waiting" : voter.state == 1 ? "answered" : "defaulted");
	}
	Log(MESSAGE, "Prompt", "Prompt #{} '{}' sent to everyone, {} ms left:{}", msg.id, msg.question.tag, msg.remaining, list);
}

// host and single player: what our own player sees of the open prompt
void PromptManager::BuildView()
{
	const PromptActive* active = queue.Active();
	if (!active) {
		haveView = false;
		view = View();
		return;
	}
	bool vote = active->question.flags & PromptQuestion::VOTE;
	view = View();
	view.id = active->id;
	view.question = active->question;
	view.deadline = Now() + queue.Remaining(Now());
	for (const auto& voter : active->voters) {
		if (voter.state != PromptVoter::State::Waiting || (vote && voter.away)) continue;
		if (voter.by == PROMPT_LOCAL_SLOT) view.mine = true;
		view.waitingFor += fmt::format("{}{}", view.waitingFor.empty() ? "" : ", ", Name(voter.by));
		if (!vote && voter.by != voter.slot) view.askedName = Name(voter.slot);
	}
	haveView = true;
}

void PromptManager::OnRemotePrompt(const NetPrompt& msg)
{
	const NetSession* net = core->GetNetSession();
	if (!net) return;
	if (msg.state == NetPrompt::CLOSED) {
		Log(MESSAGE, "Prompt", "Prompt #{} '{}' is over: {}", msg.id, msg.question.tag, msg.outcomeText);
		ShowLine(msg.outcomeText);
		if (haveView && view.id == msg.id) {
			haveView = false;
			view = View();
			guiDirty = true;
		}
		return;
	}
	uint8_t me = net->GetSlot();
	bool vote = msg.question.flags & PromptQuestion::VOTE;
	View next;
	next.id = msg.id;
	next.question = msg.question;
	next.deadline = Now() + msg.remaining;
	for (const auto& voter : msg.voters) {
		if (voter.state != uint8_t(PromptVoter::State::Waiting) || (vote && voter.away)) continue;
		if (voter.by == me) next.mine = true;
		next.waitingFor += fmt::format("{}{}", next.waitingFor.empty() ? "" : ", ", voter.byName);
		if (!vote && voter.by != voter.slot) next.askedName = voter.name;
	}
	Log(MESSAGE, "Prompt", "Prompt #{} '{}' from the host: '{}', {}, {} ms left.", msg.id, msg.question.tag, msg.question.title, next.mine ? "ours to answer" : fmt::format("waiting for {}", next.waitingFor), msg.remaining);
	view = std::move(next);
	haveView = true;
	guiDirty = true;
}

void PromptManager::OnRemoteAnswer(uint8_t slot, const NetPromptAnswer& msg)
{
	NetSession* net = Host();
	if (!net) return;
	PromptQueue::Status status = queue.Answer(msg.id, slot, msg.values, Now());
	std::string values;
	for (int32_t value : msg.values) {
		values += fmt::format("{}{}", values.empty() ? "" : " ", value);
	}
	if (status == PromptQueue::Status::Accepted) {
		Log(MESSAGE, "Prompt", "Slot {} answered prompt #{}: {}.", slot, msg.id, values);
		return;
	}
	Log(WARNING, "Prompt", "Slot {} answered prompt #{} with '{}': refused, {}.", slot, msg.id, values, PromptQueue::StatusName(status));
	std::string why = status == PromptQueue::Status::NotAsked ? "That isn't yours to decide." : status == PromptQueue::Status::Illegal ? "The host didn't take that answer." : "Too late, that was decided already.";
	net->TellPlayer(slot, why);
	// so a confused window gets the truth again
	if (queue.Active() && queue.Active()->id == msg.id) Broadcast();
}

void PromptManager::Reset()
{
	if (haveView) guiDirty = true;
	haveView = false;
	view = View();
}

void PromptManager::Shutdown()
{
	queue.Clear();
	asked.clear();
	Reset();
	guiDirty = false;
}

const PromptManager::View* PromptManager::Current() const
{
	return haveView ? &view : nullptr;
}

uint32_t PromptManager::Remaining() const
{
	if (!haveView) return 0;
	uint64_t now = Now();
	return view.deadline > now ? uint32_t(view.deadline - now) : 0;
}

bool PromptManager::TakesKeys() const
{
	return haveView && view.mine && (!view.sent || keyGrace);
}

bool PromptManager::AnswerLocal(uint32_t id, const std::vector<int32_t>& values)
{
	if (!haveView || view.id != id || !view.mine || view.sent) {
		Log(MESSAGE, "Prompt", "Not answering prompt #{}: it isn't ours to answer (now).", id);
		return false;
	}
	if (!PromptLegal(view.question, values)) {
		Log(WARNING, "Prompt", "Not answering prompt #{}: {} isn't a legal answer.", id, PromptAnswerText(view.question, values));
		return false;
	}
	NetSession* net = core->GetNetSession();
	if (net && !net->IsHost()) {
		NetPromptAnswer answer;
		answer.id = id;
		answer.values = values;
		if (!net->SendPromptAnswer(answer)) return false;
		view.sent = true;
		keyGrace = true;
		guiDirty = true;
		Log(MESSAGE, "Prompt", "Answered prompt #{}: {} (sent to the host).", id, PromptAnswerText(view.question, values));
		return true;
	}
	PromptQueue::Status status = queue.Answer(id, PROMPT_LOCAL_SLOT, values, Now());
	Log(MESSAGE, "Prompt", "Answered prompt #{}: {} ({}).", id, PromptAnswerText(view.question, values), PromptQueue::StatusName(status));
	if (status != PromptQueue::Status::Accepted) return false;
	view.sent = true;
	keyGrace = true;
	guiDirty = true;
	return true;
}

// the outcome, for our own player only (the host sends it to everyone in the CLOSED message)
void PromptManager::ShowLine(const std::string& text) const
{
	if (text.empty() || !core->GetGame() || !displaymsg) return;
	std::string clean = text;
	std::replace(clean.begin(), clean.end(), '[', '(');
	std::replace(clean.begin(), clean.end(), ']', ')');
	std::string markup = fmt::format("[p][color={}]* {}[/color][/p]", OUTCOME_COLOR, clean);
	if (NetSession* net = core->GetNetSession()) {
		net->DisplayLocal(markup);
	} else {
		displaymsg->DisplayMarkupString(StringFromUtf8(markup.c_str()));
	}
}

// the prompt window (GUIScripts/Prompt.py) reads Current () again
void PromptManager::Notify()
{
	guiDirty = false;
	if (!core->GetGame()) return;
	auto gs = core->GetGUIScriptEngine();
	if (gs) gs->RunFunction("Prompt", "OnChange");
}

// travel ---------------------------------------------------------------------

bool PromptManager::HoldTravel(Actor* actor, InfoPoint* exit)
{
	NetSession* net = Host();
	if (!net || !actor || !exit) return false;
	bool confirm = core->config.NetTravelConfirm != 0;
	bool vote = core->config.NetTravelVote != 0;
	if (!confirm && !vote) return false;
	if (travel.approved && travel.actorID == actor->GetGlobalID() && travel.exitID == exit->GetGlobalID()) {
		return false;
	}
	// asking already: nobody leaves meanwhile (the game is paused anyway)
	if (travel.pending) return true;

	uint8_t owner = actor->InParty ? net->OwnerSlot(*actor) : PROMPT_LOCAL_SLOT;
	std::vector<uint8_t> others;
	for (uint8_t slot : net->ConnectedSlots()) {
		if (slot != owner) others.push_back(slot);
	}
	if (!confirm && others.empty()) return false;

	travel = Travel();
	travel.pending = true;
	travel.actorID = actor->GetGlobalID();
	travel.exitID = exit->GetGlobalID();
	travel.owner = owner;
	travel.area = std::string(actor->GetCurrentArea()->GetScriptRef());
	travel.destination = std::string(exit->Destination);
	Log(MESSAGE, "Prompt", "{} (slot {}) wants to travel from {} to {} by '{}': {}.", ActorName(actor), owner, travel.area, travel.destination, exit->GetScriptName(), confirm ? "asking them to confirm" : "asking the others");

	if (!confirm) {
		TravelVote();
		return true;
	}
	PromptQuestion q;
	q.kind = PromptKind::YesNo;
	q.tag = "travel";
	q.title = "Leave the area?";
	q.text = fmt::format("{} is at the exit to {}. Travel there now? The whole party goes.", ActorName(actor), travel.destination);
	q.defaultChoice = 0; // no answer: nobody leaves
	Ask(actor, std::move(q), [this](const PromptResult& result) {
		if (!result.Yes()) {
			ResumeTravel(false, "the traveller didn't confirm");
			return;
		}
		TravelVote();
	});
	return true;
}

// everyone else agrees to go (BG's "ready" check); silence counts as yes, one no stops it
void PromptManager::TravelVote()
{
	NetSession* net = Host();
	if (!net || !core->config.NetTravelVote) {
		ResumeTravel(true, "confirmed");
		return;
	}
	std::vector<uint8_t> others;
	for (uint8_t slot : net->ConnectedSlots()) {
		if (slot != travel.owner) others.push_back(slot);
	}
	if (others.empty()) {
		ResumeTravel(true, "nobody else to ask");
		return;
	}
	const Game* game = core->GetGame();
	const Actor* actor = game ? game->GetActorByGlobalID(travel.actorID) : nullptr;
	PromptQuestion q;
	q.kind = PromptKind::YesNo;
	q.flags = PromptQuestion::STOP_ON_NO;
	q.tag = "travelvote";
	q.title = "Ready to travel?";
	q.text = fmt::format("{} ({}) wants the party to travel to {}. Ready to go?", Name(travel.owner), ActorName(actor), travel.destination);
	q.defaultChoice = 1; // no answer counts as ready
	q.actorID = travel.actorID;
	AskPlayers(std::move(others), std::move(q), [this](const PromptResult& result) {
		ResumeTravel(result.Yes(), result.Yes() ? "everyone is ready" : "someone isn't ready");
	});
}

void PromptManager::ResumeTravel(bool go, const std::string& why)
{
	Travel trip = travel;
	travel = Travel();
	Game* game = core->GetGame();
	Actor* actor = game ? game->GetActorByGlobalID(trip.actorID) : nullptr;
	Map* map = actor ? actor->GetCurrentArea() : nullptr;
	InfoPoint* exit = map ? map->GetInfoPointByGlobalID(trip.exitID) : nullptr;
	if (!go || !exit) {
		Log(MESSAGE, "Prompt", "Travel to {} called off: {}.", trip.destination, exit ? why : "the traveller or the exit is gone");
		// they stay; the exit fires again only for a new click
		for (int i = 0; game && i < game->GetPartySize(false); ++i) {
			Actor* pc = game->GetPC(i, false);
			if (pc && pc->GetCurrentArea() == map && (pc->GetInternalFlag() & IF_USEEXIT)) {
				pc->SetInternalFlag(IF_USEEXIT, BitOp::NAND);
			}
		}
		return;
	}
	Log(MESSAGE, "Prompt", "Travel to {} goes ahead: {}.", trip.destination, why);
	travel.approved = true;
	travel.actorID = trip.actorID;
	travel.exitID = trip.exitID;
	map->UseExit(actor, exit);
	travel = Travel();
}

// test hooks -------------------------------------------------------------------

void PromptManager::RunTestPrompts()
{
	const std::string& orders = core->config.TestPrompts;
	const Game* game = core->GetGame();
	if (orders.empty() || !game || !core->GetGameControl() || !game->GetPartySize(false)) return;
	if (core->QuitFlag & (QF_QUITGAME | QF_EXITGAME | QF_LOADGAME | QF_ENTERGAME)) return;
	testTicks++;
	size_t start = 0;
	while (start < orders.size()) {
		size_t end = orders.find(';', start);
		if (end == std::string::npos) end = orders.size();
		std::string spec = orders.substr(start, end - start);
		start = end + 1;
		size_t colon = spec.find(':');
		if (colon == std::string::npos || unsigned(atoi(spec.c_str())) != testTicks) continue;
		Log(MESSAGE, "Prompt", "Test order at tick {}: {}", testTicks, spec.substr(colon + 1));
		if (!RunTestOrder(spec.substr(colon + 1))) {
			Log(WARNING, "Prompt", "Unknown prompt test order '{}'.", spec.substr(colon + 1));
		}
	}
}

// a key as the video driver sends it: down, then up, through the event manager
void PromptManager::TestKey(const std::string& name) const
{
	const WindowManager* wm = core->GetWindowManager();
	if (!wm || name.empty()) return;
	static const std::map<std::string, KeyboardKey> named = {
		{ "enter", GEM_RETURN }, { "esc", GEM_ESCAPE }, { "up", GEM_UP }, { "down", GEM_DOWN },
		{ "left", GEM_LEFT }, { "right", GEM_RIGHT }, { "plus", KeyboardKey('+') }, { "minus", KeyboardKey('-') },
		{ "space", KeyboardKey(' ') }, { "tab", GEM_TAB }
	};
	auto it = named.find(name);
	KeyboardKey key = it != named.end() ? it->second : KeyboardKey(name[0]);
	wm->InjectEvent(EventMgr::CreateKeyEvent(key, true));
	wm->InjectEvent(EventMgr::CreateKeyEvent(key, false));
	Log(MESSAGE, "Prompt", "Test key '{}' pressed; the prompt window {} the keys.", name, TakesKeys() ? "has" : "doesn't have");
}

static std::vector<std::string> Words(const std::string& text)
{
	std::vector<std::string> words;
	std::istringstream in(text);
	std::string word;
	while (in >> word) {
		words.push_back(word);
	}
	return words;
}

static std::string JoinWords(const std::vector<std::string>& words, size_t from)
{
	std::string text;
	for (size_t i = from; i < words.size(); ++i) {
		text += (text.empty() ? "" : " ") + words[i];
	}
	return text;
}

// ask luck PC MAX [TIMEOUT] | ask spellburn PC [TIMEOUT] | ask yesno PC TIMEOUT TEXT |
// ask choice PC TIMEOUT A|B|C | ask vote TIMEOUT TEXT (host, single player);
// prompt key K | prompt press N | prompt answer V... | prompt raw V... | prompt rawid ID V... |
// prompt cancel | prompt dump (anyone)
bool PromptManager::RunTestOrder(const std::string& order)
{
	std::vector<std::string> words = Words(order);
	if (words.size() < 2 || (words[0] != "ask" && words[0] != "prompt")) return false;
	Game* game = core->GetGame();
	const std::string& what = words[1];
	auto number = [&words](size_t i, int fallback) { return i < words.size() ? atoi(words[i].c_str()) : fallback; };

	if (words[0] == "ask") {
		Actor* pc = game && words.size() > 2 ? game->FindPC(unsigned(number(2, 1))) : nullptr;
		PromptQuestion q;
		if (what == "luck") {
			// stands in for the DCC rules: an attack missed, its owner may burn Luck for +1 per point
			int luck = number(3, 3);
			q.kind = PromptKind::Integer;
			q.tag = "luckburn";
			q.title = "Burn Luck?";
			q.text = fmt::format("{} missed by {}. Burn Luck to turn the miss into a hit (+1 per point)?", ActorName(pc), 2);
			q.fields = { { "Luck", 0, luck, 0 } };
			q.timeout = uint32_t(number(4, 0)) * 1000;
			uint32_t pcID = pc ? pc->GetGlobalID() : 0;
			Ask(pc, std::move(q), [pcID, luck](const PromptResult& result) {
				const Game* g = core->GetGame();
				const Actor* who = g ? g->GetActorByGlobalID(pcID) : nullptr;
				int burnt = result.Value();
				Log(MESSAGE, "Prompt", "Luck burn test, C++ continuation: {} burns {} of {} Luck ({}, slot {}); the attack {}.", ActorName(who), burnt, luck, PromptOutcomeName(result.outcome), result.slot, burnt >= 2 ? "hits" : "still misses");
			});
		} else if (what == "spellburn") {
			q.kind = PromptKind::Integer;
			q.tag = "spellburn";
			q.title = "Spellburn";
			q.text = fmt::format("{} may burn ability points into this spell check.", ActorName(pc));
			q.fields = { { "Str", 0, 3, 0 }, { "Agi", 0, 3, 0 }, { "Sta", 0, 3, 0 } };
			q.timeout = uint32_t(number(3, 0)) * 1000;
			Ask(pc, std::move(q), [](const PromptResult& result) {
				Log(MESSAGE, "Prompt", "Spellburn test, C++ continuation: {} ({}).", PromptAnswerText(result.question, result.values), PromptOutcomeName(result.outcome));
			});
		} else if (what == "yesno" || what == "choice") {
			q.kind = what == "yesno" ? PromptKind::YesNo : PromptKind::Choice;
			q.tag = "test" + what;
			q.timeout = uint32_t(number(3, 0)) * 1000;
			std::string text = JoinWords(words, 4);
			if (what == "choice") {
				size_t bar = 0;
				while ((bar = text.find('|')) != std::string::npos) {
					q.choices.push_back(text.substr(0, bar));
					text.erase(0, bar + 1);
				}
				q.choices.push_back(text);
				q.title = "Choose";
				q.text = fmt::format("What should {} do?", ActorName(pc));
			} else {
				q.title = "Question";
				q.text = text;
			}
			Ask(pc, std::move(q), [](const PromptResult& result) {
				Log(MESSAGE, "Prompt", "Test prompt continuation: {} ({}, slot {}).", PromptAnswerText(result.question, result.values), PromptOutcomeName(result.outcome), result.slot);
			});
		} else if (what == "vote") {
			q.kind = PromptKind::YesNo;
			q.flags = PromptQuestion::STOP_ON_NO;
			q.tag = "testvote";
			q.title = "Vote";
			q.text = JoinWords(words, 3);
			q.defaultChoice = 1;
			q.timeout = uint32_t(number(2, 0)) * 1000;
			AskPlayers(Players(), std::move(q), [](const PromptResult& result) {
				Log(MESSAGE, "Prompt", "Test vote continuation: {}.", result.Yes() ? "yes" : "no");
			});
		} else {
			return false;
		}
		return true;
	}

	std::vector<int32_t> values;
	size_t first = what == "rawid" ? 3 : 2;
	for (size_t i = first; i < words.size(); ++i) {
		values.push_back(atoi(words[i].c_str()));
	}
	if (what == "key") {
		TestKey(words.size() > 2 ? words[2] : "");
	} else if (what == "press") {
		// what a click on the window's Nth button runs
		auto gs = core->GetGUIScriptEngine();
		if (gs) gs->ExecString(fmt::format("import Prompt\nPrompt.TestPress ({})", number(2, 0)));
	} else if (what == "answer") {
		AnswerLocal(view.id, values);
	} else if (what == "raw" || what == "rawid") {
		// straight to whoever decides, past our own checks: what a broken or cheating client could send
		uint32_t id = what == "rawid" ? uint32_t(number(2, 0)) : view.id;
		NetSession* net = core->GetNetSession();
		if (net && !net->IsHost()) {
			NetPromptAnswer answer;
			answer.id = id;
			answer.values = values;
			net->SendPromptAnswer(answer);
			Log(MESSAGE, "Prompt", "Test hook: sent the host a raw answer to prompt #{}.", id);
		} else {
			Log(MESSAGE, "Prompt", "Test hook: raw answer to prompt #{}: {}.", id, PromptQueue::StatusName(queue.Answer(id, PROMPT_LOCAL_SLOT, values, Now())));
		}
	} else if (what == "cancel") {
		if (queue.Active()) Cancel(queue.Active()->id);
	} else if (what == "dump") {
		if (!haveView) {
			Log(MESSAGE, "Prompt", "Test hook: no prompt open.");
		} else {
			Log(MESSAGE, "Prompt", "Test hook: prompt #{} '{}' '{}', {}, waiting for {}{}, {} ms left, {} queued.", view.id, view.question.tag, view.question.title, view.mine ? (view.sent ? "ours, answer sent" : "ours to answer") : "read only", view.waitingFor, view.askedName.empty() ? "" : fmt::format(" (for {})", view.askedName), Remaining(), queue.Queued());
		}
	} else {
		return false;
	}
	return true;
}

}
