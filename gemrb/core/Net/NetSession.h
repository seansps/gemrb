// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file NetSession.h
 * Host-authoritative co-op session.
 *
 * The host runs the normal simulation; clients forward player orders to it
 * and mirror the actor state and world events it broadcasts instead of
 * running scripts and AI themselves. Clients join (and resync) by loading
 * a save the host writes and streams to them. A session only exists when
 * NetMode is set in the config, so single player never touches any of this.
 */

#ifndef NET_SESSION_H
#define NET_SESSION_H

#include "exports.h"
#include "ie_types.h"

#include "Net/Protocol.h"
#include "Net/Replication.h"
#include "Strings/String.h"
#include "Strings/StringView.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GemRB {

class Action;
class Actor;
class Container;
class InfoPoint;
class Map;
class Projectile;
class Scriptable;
class VEFObject;
struct AudioPlaybackConfig;
struct Color;
struct CoreSettings;

class GEM_EXPORT NetSession {
public:
	enum class Mode : uint8_t {
		Host,
		Client
	};

	/** returns nullptr when networking is off (NetMode unset) or unavailable (built without ENet);
	 * a host started from the GUI (lobby) waits in its lobby, one from the config starts at once */
	static std::unique_ptr<NetSession> Create(const CoreSettings& config, bool lobby = false);
	/** whether this build can network at all */
	static bool Available();

	NetSession(const NetSession&) = delete;
	NetSession& operator=(const NetSession&) = delete;
	~NetSession();

	Mode GetMode() const { return mode; }
	bool IsHost() const { return mode == Mode::Host; }
	/** clients mirror the host: they never run local scripts/AI and forward orders instead */
	bool IsReplica() const;
	/** client: true once the host's save is loaded and its actors are mapped */
	bool IsInSync() const;

	/** once per game loop tick, before the simulation: poll and handle incoming messages */
	void PreTick();
	/** once per game loop tick, after the simulation: broadcast state and flush */
	void PostTick();
	/** once per drawn frame: clients smooth actor movement between snapshots */
	void Frame();

	/** what the player typed: said to everyone, or a command (/w NAME text whispers, /r replies) */
	void SendChat(const std::string& text);
	/** the message window was made anew: shows the recent chat (lobby included) in it again */
	size_t ReplayChat();
	/** client: send a player order (NetCommand::Flags) for a local actor to the host; false if it wasn't forwarded */
	bool ForwardAction(const Actor& actor, const Action& action, uint8_t flags);
	/** client: the player sent the actor through a travel region */
	bool ForwardUseExit(const Actor& actor, const Scriptable& exit);
	/** client: ask the host for a fresh save to reload; false if not connected */
	bool RequestResync();

	// host: things the simulation shows or plays, so clients see and hear them too;
	// ignored on clients and outside the simulation (eg. GUI clicks)
	void OnFeedback(const String& markup);
	void OnOverheadText(const Scriptable& owner, const String& text, const Color& color);
	void OnSound(StringView resource, const AudioPlaybackConfig& config, bool speech);
	void OnProjectile(const Map& map, const Projectile& pro, uint32_t targetID, bool fake);
	void OnAreaVfx(const Map& map, VEFObject& vvc);
	/** both sides: the game was paused or unpaused here; quiet pauses come from GUI windows */
	void OnPause(bool paused, bool quiet = false);

	// dialog: the host runs DialogHandler and shows it to everyone; the owner answers
	/** host: a line of the conversation was said (party: a party member's reply) */
	void OnDialogLine(const Scriptable* speaker, const String& text, bool party);
	/** host: the conversation reached a node with these replies (DialogHandler's options) */
	void OnDialogState(const std::vector<std::pair<int, String>>& options);
	void OnDialogEnd();
	/** both sides, the GUI picked a reply: true if it was handled here, ie. refused (not
	 * the owner) or forwarded to the host (clients); false lets DialogHandler go ahead */
	bool DialogChoiceHandled(int choice);

	/** host: StartStore ran; true if it opens on a remote player's screen instead of ours */
	bool OnStartStore(const ResRef& store, const Scriptable& merchant);
	/** host: a remote player's party member reached and opened a container */
	void OnContainerUsed(const Actor& actor, Container& container);
	/** client: the player clicked a container or an info point */
	bool ForwardUseContainer(const Actor& actor, const Container& container);
	bool ForwardRegionClick(const Actor& actor, const InfoPoint& region);
	/** a state changing GemRB.* GUI call is about to run: false if it is refused here (not our
	 * party member, no permission); clients forward it, and false for the ones only the host runs */
	bool GuiCall(NetGuiOp op, std::vector<int32_t> args, const std::string& text = std::string());
	/** both sides: whether our player may order this party member; tells the player if not */
	bool MayOrder(const Actor& actor);

	// the lobby (M1.6): players, who controls which party slot, permissions, readiness, chat
	NetPhase GetPhase() const;
	/** players, party slots and permissions as the host has them; changes bump the revision */
	const NetLobbyState& GetLobby() const;
	uint32_t GetLobbyRevision() const;
	uint8_t GetSlot() const;
	/** for the lobby window, eg. "Waiting for the host to start the game" */
	const std::string& GetStatus() const;
	const std::vector<std::string>& GetChatLog() const;
	/** whether we may do this, as the host decided (the host may do everything) */
	bool Allowed(NetPermission perm) const;
	void SetReady(bool ready);
	// host only
	bool Assign(uint8_t partySlot, uint8_t playerSlot);
	bool SetPermission(uint8_t slot, NetPermission perm, bool allowed);
	/** everyone is ready: from now on joins get the game, which the GUI starts next */
	bool StartGame();
	bool CanStart() const;

private:
	struct Impl;

	NetSession(Mode mode, std::unique_ptr<Impl> impl);

	Mode mode;
	std::unique_ptr<Impl> impl;
};

}

#endif
