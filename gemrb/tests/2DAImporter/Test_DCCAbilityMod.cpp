// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks the shipped unhardcoded/ethren/abilmod.2da against DCC Table 1-1
// (docs/rules/abilities.md), read through the real 2DA importer and loader.

#include "../../core/DCC/AbilityMod.h"
#include "../../core/Streams/FileStream.h"
#include "../../plugins/2DAImporter/2DAImporter.h"

#include <gtest/gtest.h>

namespace GemRB {

static const path_t ABILMOD_FILE = PathJoin(GEMRB_UNHARDCODED_DIR, "ethren", "abilmod.2da");

TEST(DCC_AbilModTable, MatchesTable11)
{
	auto stream = std::make_unique<FileStream>();
	ASSERT_TRUE(stream->Open(ABILMOD_FILE)) << ABILMOD_FILE;
	p2DAImporter importer;
	ASSERT_TRUE(importer.Open(std::unique_ptr<DataStream> { std::move(stream) }));

	DCC::AbilityModTable table;
	ASSERT_TRUE(table.Load(importer));
	EXPECT_EQ(table.MinScore(), 3);
	EXPECT_EQ(table.MaxScore(), 18);

	// score: modifier, wizard spells known adjustment, max spell level (0: no spellcasting)
	const int expected[16][3] = {
		{ -3, 0, 0 }, // 3
		{ -2, -2, 1 }, // 4
		{ -2, -2, 1 }, // 5
		{ -1, -1, 1 }, // 6
		{ -1, -1, 1 }, // 7
		{ -1, 0, 2 }, // 8
		{ 0, 0, 2 }, // 9
		{ 0, 0, 3 }, // 10
		{ 0, 0, 3 }, // 11
		{ 0, 0, 4 }, // 12
		{ 1, 0, 4 }, // 13
		{ 1, 1, 4 }, // 14
		{ 1, 1, 5 }, // 15
		{ 2, 1, 5 }, // 16
		{ 2, 2, 5 }, // 17
		{ 3, 2, 5 } // 18
	};
	for (int score = 3; score <= 18; ++score) {
		const int* row = expected[score - 3];
		EXPECT_EQ(table.Modifier(score), row[0]) << "score " << score;
		EXPECT_EQ(table.WizardSpells(score), row[1]) << "score " << score;
		EXPECT_EQ(table.MaxSpellLevel(score), row[2]) << "score " << score;
	}
}

}
