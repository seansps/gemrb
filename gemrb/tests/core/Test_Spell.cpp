// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Spell casting and spell trigger checks against the demo's data.

// FIXME: remove once fixed, this is excluding non-linux build bots
#if defined(USE_OPENGL_BACKEND) || (!defined(__APPLE__) && !defined(WIN32))

	#include "LiveGameFixture.h"

	#include "../../core/EffectQueue.h"
	#include "../../core/GameScript/GameScript.h"
	#include "../../core/Scriptable/Actor.h"
	#include "../../core/Spell.h"
	#include "../../includes/ie_stats.h"

	#include <gtest/gtest.h>

namespace GemRB {

class SpellTest : public LiveGameTest {
};

// the demo's effects.ids only maps Damage and FindTraps, so there is no casting glow opcode
TEST_F(SpellTest, CastingGlowWithoutOpcode)
{
	EffectRef glowRef = { "CastingGlow", -1 };
	ASSERT_LT(EffectQueue::ResolveEffect(glowRef), 0);

	std::unique_ptr<Actor> caster(gamedata->GetCreature(ResRef("protagon")));
	ASSERT_NE(caster, nullptr);

	Spell spl;
	spl.CastingSound = -1; // no sound, there is no area to play it in
	EffectQueue fxqueue;
	fxqueue.SetOwner(caster.get());
	spl.AddCastingGlow(&fxqueue, 30, SEX_MALE);
	EXPECT_EQ(fxqueue.GetEffectsCount(), 0u);

	// and again, now with the failed lookup cached
	spl.AddCastingGlow(&fxqueue, 30, SEX_MALE);
	EXPECT_EQ(fxqueue.GetEffectsCount(), 0u);
}

TEST_F(SpellTest, HaveSpellRES)
{
	std::unique_ptr<Actor> actor(gamedata->GetCreature(ResRef("protagon")));
	ASSERT_NE(actor, nullptr);
	ASSERT_EQ(actor->objects.LastMarkedSpell, 0);

	Trigger trigger;
	trigger.resref0Parameter = ResRef("SPWI112");
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &trigger), 0);

	CRESpellMemorization* sm = actor->spellbook.GetSpellMemorization(IE_SPELL_TYPE_WIZARD, 0);
	ASSERT_NE(sm, nullptr);
	auto memorized = MakeHolder<CREMemorizedSpell>();
	memorized->SpellResRef = ResRef("SPWI112");
	memorized->Flags = 1;
	sm->memorizedSpells.push_back(memorized);
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &trigger), 1);

	// a cast (depleted) spell doesn't count
	memorized->Flags = 0;
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &trigger), 0);
	memorized->Flags = 1;

	// HaveSpell(): by number, and the iwd2 fallback to the marked spell for 0
	Trigger byNumber;
	byNumber.int0Parameter = 2112;
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &byNumber), 1);
	byNumber.int0Parameter = 0;
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &byNumber), 0);
	actor->objects.LastMarkedSpell = 2112;
	EXPECT_EQ(GameScript::HaveSpell(actor.get(), &byNumber), 1);
}

}

#endif
