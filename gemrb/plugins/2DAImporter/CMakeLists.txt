# SPDX-FileCopyrightText: 2008 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

ADD_GEMRB_PLUGIN(2DAImporter 2DAImporter.cpp 2DAImporter.h)

ADD_GEMRB_PLUGIN_TEST(2DAImporter
  2DAImporter.cpp
  ../../tests/2DAImporter/Test_2DAImporter.cpp
  ../../tests/2DAImporter/Test_DCCAbilityMod.cpp
)

IF (BUILD_TESTING)
  # the DCC table test reads the shipped unhardcoded/ethren/abilmod.2da from the source tree
  target_compile_definitions(Test_2DAImporter PRIVATE GEMRB_UNHARDCODED_DIR="${CMAKE_SOURCE_DIR}/gemrb/unhardcoded")
ENDIF()
