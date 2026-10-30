// SPDX-FileCopyrightText: 2003 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SAVEGAMEITERATOR_H
#define SAVEGAMEITERATOR_H

#include "exports.h"

#include "SaveGame.h"

#include <vector>

namespace GemRB {

#define SAVEGAME_DIRECTORY_MATCHER "%d - %[^\n\\/]"

class GEM_EXPORT SaveGameIterator {
private:
	using charlist = std::vector<Holder<SaveGame>>;
	charlist save_slots;

public:
	SaveGameIterator() noexcept = default;
	~SaveGameIterator() noexcept = default;
	const charlist& GetSaveGames();
	void DeleteSaveGame(const Holder<SaveGame>&) const;
	int CreateSaveGame(Holder<SaveGame> save, const String& slotname, Holder<Sprite2D> preview, bool force = false) const;
	int CreateSaveGame(Holder<SaveGame>, StringView slotname, Holder<Sprite2D> preview, bool force = false) const;
	int CreateSaveGame(int index, bool mqs, Holder<Sprite2D> preview) const;
	Holder<SaveGame> GetSaveGame(const String& slotname);
	/** writes the game files of a save (GAM, WMP, SAV) into an existing dir, without the checks, portraits or preview */
	static bool WriteGameFiles(const path_t& path, bool overrideRunning = false);

private:
	bool RescanSaveGames();
	static Holder<SaveGame> BuildSaveGame(std::string slotname);
	void PruneQuickSave(StringView folder) const;
};

}

#endif
