// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file NetSession.h
 * Host-authoritative co-op session (spike).
 *
 * The host runs the normal simulation; clients forward player orders to it
 * and mirror the actor state it broadcasts instead of running scripts and
 * AI themselves. A session only exists when NetMode is set in the config,
 * so single player never touches any of this.
 */

#ifndef NET_SESSION_H
#define NET_SESSION_H

#include "exports.h"

#include <memory>
#include <string>

namespace GemRB {

class Actor;
struct CoreSettings;

class GEM_EXPORT NetSession {
public:
	enum class Mode : uint8_t {
		Host,
		Client
	};

	/** returns nullptr when networking is off (NetMode unset) or unavailable (built without ENet) */
	static std::unique_ptr<NetSession> Create(const CoreSettings& config);

	NetSession(const NetSession&) = delete;
	NetSession& operator=(const NetSession&) = delete;
	~NetSession();

	Mode GetMode() const { return mode; }
	bool IsHost() const { return mode == Mode::Host; }
	/** a client that mirrors the host: don't run local scripts/AI, forward orders instead */
	bool IsReplica() const;

	/** once per game loop tick, before the simulation: poll and handle incoming messages */
	void PreTick();
	/** once per game loop tick, after the simulation: broadcast state and flush */
	void PostTick();

	void SendChat(const std::string& text);
	/** client: send an order for a local actor to the host; false if it wasn't forwarded */
	bool ForwardCommand(const Actor& actor, const std::string& action, bool append);

private:
	struct Impl;

	NetSession(Mode mode, std::unique_ptr<Impl> impl);

	Mode mode;
	std::unique_ptr<Impl> impl;
};

}

#endif
