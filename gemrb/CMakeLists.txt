# SPDX-FileCopyrightText: 2008 Contributors to the GemRB project <https://gemrb.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

SET(CMAKE_CXX_VISIBILITY_PRESET "hidden")
# FreeBSD clang still has symbol visibility issues with the GUIScript plugin
# similarly on mac, OpenBSD, probably other BSDs
IF(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	STRING(FIND "${CMAKE_SYSTEM_NAME}" "BSD" ABSD)
	IF(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR NOT ABSD EQUAL -1)
		SET(CMAKE_CXX_VISIBILITY_PRESET "default")
	ENDIF()
ENDIF()
SET(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)

INCLUDE(GenerateExportHeader)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/includes)

IF(USE_TRACY)
  ADD_SUBDIRECTORY(plugins/Tracy)
ENDIF()
ADD_SUBDIRECTORY( core )

# GUIScripts and override go directly in the bundle
SET(RESOURCE_LIST "GUIScripts" "override" "unhardcoded")
IF (NOT SKIP_DEMO_DATA)
	LIST(APPEND RESOURCE_LIST "../demo")
ENDIF()
IF (NOT OPENGL_BACKEND STREQUAL "None")
	LIST(APPEND RESOURCE_LIST "plugins/SDLVideo/Shaders")
ENDIF()
SET(RESOURCE_FILES "")

FOREACH(RESOURCE_REL_PATH ${RESOURCE_LIST})
	SET(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_REL_PATH}")
	IF (APPLE)
		get_filename_component(RES_DIR "${RESOURCE_REL_PATH}" NAME)
		FILE(GLOB CHILDREN RELATIVE "${RESOURCE_DIR}" ${RESOURCE_DIR}/*)
		FOREACH(CHILD ${CHILDREN})
			IF(IS_DIRECTORY "${RESOURCE_DIR}/${CHILD}")
				# be wary of .DS_Store files. glob them out by using [A-Za-z]*.*
				FILE(GLOB RESOURCE_DIR_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${RESOURCE_DIR}/${CHILD}/[A-Za-z]*.*")
				SET_SOURCE_FILES_PROPERTIES(
					${RESOURCE_DIR_FILES}
					PROPERTIES MACOSX_PACKAGE_LOCATION Resources/${RES_DIR}/${CHILD}
				)
				SET(RESOURCE_FILES ${RESOURCE_FILES} ${RESOURCE_DIR_FILES})
			ELSEIF(NOT ${CHILD} MATCHES "\\.DS_Store")
				SET_SOURCE_FILES_PROPERTIES(
					"${RESOURCE_DIR}/${CHILD}"
					PROPERTIES MACOSX_PACKAGE_LOCATION Resources/${RES_DIR}
				)
				SET(RESOURCE_FILES ${RESOURCE_FILES} "${RESOURCE_REL_PATH}/${CHILD}")
			ENDIF()
		ENDFOREACH()
	ELSE (APPLE)
		INSTALL(DIRECTORY ${RESOURCE_DIR} DESTINATION "${DATA_DIR}")
	ENDIF (APPLE)
ENDFOREACH()

IF(APPLE) #application bundle generation
	# icon
	SET_SOURCE_FILES_PROPERTIES(
		../artwork/gemrb.icns
		PROPERTIES MACOSX_PACKAGE_LOCATION Resources
	)

	# default values
	SET_SOURCE_FILES_PROPERTIES(
		../platforms/apple/defaults.plist
		PROPERTIES MACOSX_PACKAGE_LOCATION Resources
	)

	SET(MACOSX_BUNDLE_ICON_FILE gemrb)
	SET(PRODUCT_NAME "GemRB") # for info.plist
	
	ADD_EXECUTABLE(gemrb MACOSX_BUNDLE
					../platforms/apple/CocoaWrapper/mac/CocoaWrapper.mm
					../platforms/apple/AppleLogger.mm
					../platforms/apple/EmptyStringTransformer.m
					../artwork/gemrb.icns
					../platforms/apple/defaults.plist
					${RESOURCE_FILES})

	SET(BUNDLE_FRAMEWORK_PATH "${CMAKE_BINARY_DIR}/gemrb/${PROJECT_NAME}.app/Contents/Frameworks")
	
	get_filename_component(SDL_BASENAME "${SDL2_LIBDIR}" NAME)
	
	IF(NOT SDL_BASENAME MATCHES ".framework")
		SET(SDL_BASENAME "libSDL2.dylib")
		SET(SDL_LIB "${SDL2_LIBDIR}/${SDL_BASENAME}")
		get_filename_component(SDL_FULL_PATH "${SDL_LIB}" REALPATH)
		get_filename_component(SDL_REAL_NAME "${SDL_FULL_PATH}" NAME)
		SET(SDL_BUNDLED "${BUNDLE_FRAMEWORK_PATH}/${SDL_REAL_NAME}")

		# Bundle a copy of SDL and point the copy's "install name" into the bundle. Never modify the
		# original (e.g. Homebrew's). install_name_tool invalidates the signature, which arm64 refuses
		# to load (SIGKILL, "Code Signature Invalid"), so re-sign the copy ad-hoc.
		FILE(COPY ${SDL_FULL_PATH} DESTINATION ${BUNDLE_FRAMEWORK_PATH} FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
		FILE(CREATE_LINK ${SDL_REAL_NAME} "${BUNDLE_FRAMEWORK_PATH}/${SDL_BASENAME}" SYMBOLIC)
		EXECUTE_PROCESS(COMMAND ${CMAKE_INSTALL_NAME_TOOL} -id "@loader_path/../Frameworks/${SDL_BASENAME}" "${SDL_BUNDLED}")
		EXECUTE_PROCESS(COMMAND codesign --force --sign - "${SDL_BUNDLED}")

		# link against the bundled copy, so binaries record its in-bundle install name
		IF(TARGET SDL2::SDL2)
			SET_TARGET_PROPERTIES(SDL2::SDL2 PROPERTIES IMPORTED_LOCATION "${SDL_BUNDLED}")
			GET_TARGET_PROPERTY(SDL_IMPORTED_CONFIGS SDL2::SDL2 IMPORTED_CONFIGURATIONS)
			FOREACH(SDL_CONFIG IN LISTS SDL_IMPORTED_CONFIGS)
				SET_TARGET_PROPERTIES(SDL2::SDL2 PROPERTIES IMPORTED_LOCATION_${SDL_CONFIG} "${SDL_BUNDLED}")
			ENDFOREACH()
		ELSE()
			SET(SDL_LIBRARY "${SDL_BUNDLED}")
		ENDIF()

		IF(USE_SDLMIXER)
			get_filename_component(SDL_MIXER_NAME "${SDL_MIXER_LIBRARIES}" NAME)
			get_filename_component(SDL_MIXER_FULL_PATH "${SDL_MIXER_LIBRARIES}" REALPATH)
			get_filename_component(SDL_MIXER_REAL_NAME "${SDL_MIXER_FULL_PATH}" NAME)
			SET(SDL_MIXER_BUNDLED "${BUNDLE_FRAMEWORK_PATH}/${SDL_MIXER_REAL_NAME}")
			FILE(COPY "${SDL_MIXER_FULL_PATH}" DESTINATION ${BUNDLE_FRAMEWORK_PATH} FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
			FILE(CREATE_LINK ${SDL_MIXER_REAL_NAME} "${BUNDLE_FRAMEWORK_PATH}/${SDL_MIXER_NAME}" SYMBOLIC)

			# the copy must load the bundled SDL too, not the one it was built against
			EXECUTE_PROCESS(COMMAND otool -L "${SDL_MIXER_BUNDLED}" OUTPUT_VARIABLE SDL_MIXER_DEPS)
			STRING(REGEX MATCH "[^\t\n ]*libSDL2-[^\t\n ]*\\.dylib" SDL_MIXER_SDL_DEP "${SDL_MIXER_DEPS}")
			SET(SDL_MIXER_CHANGE)
			IF(SDL_MIXER_SDL_DEP)
				SET(SDL_MIXER_CHANGE -change "${SDL_MIXER_SDL_DEP}" "@loader_path/${SDL_BASENAME}")
			ENDIF()
			EXECUTE_PROCESS(COMMAND ${CMAKE_INSTALL_NAME_TOOL} -id "@loader_path/../Frameworks/${SDL_MIXER_NAME}" ${SDL_MIXER_CHANGE} "${SDL_MIXER_BUNDLED}")
			EXECUTE_PROCESS(COMMAND codesign --force --sign - "${SDL_MIXER_BUNDLED}")
			SET(SDL_MIXER_LIBRARIES "${SDL_MIXER_BUNDLED}")
		ENDIF()
	ELSE()
		SET(SDL_LIB "${SDL2_LIBDIR}")
	ENDIF()
	
	MESSAGE(STATUS "Copying SDL: ${SDL_LIB}")
	FILE(COPY ${SDL_LIB} DESTINATION ${BUNDLE_FRAMEWORK_PATH} FILE_PERMISSIONS OWNER_READ OWNER_WRITE)

	SET(BUNDLE_LOADER "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.app/Contents/MacOS/${PROJECT_NAME}") # BUNDLE_LOADER is used by plugins needing to extend the cocoa wrapper (SDLVideo).
	STRING(APPEND CMAKE_MODULE_LINKER_FLAGS " -bundle_loader \"${BUNDLE_LOADER}\"")
ELSE(APPLE)
	ADD_SUBDIRECTORY( docs )
	ADD_SUBDIRECTORY( tests )

	SET(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../platforms")

	IF(WIN32)
		ADD_EXECUTABLE(gemrb ${PLATFORM_DIR}/windows/GemRB.cpp ${PLATFORM_DIR}/windows/window.rc)
	ELSEIF(VITA)
		ADD_EXECUTABLE(gemrb ${PLATFORM_DIR}/vita/GemRB.cpp ${PLATFORM_DIR}/vita/VitaLogger.cpp)
	ELSEIF(ANDROID)
		ADD_EXECUTABLE(gemrb ${PLATFORM_DIR}/android/GemRB.cpp ${PLATFORM_DIR}/android/AndroidLogger.cpp)
	ELSE(WIN32)
		ADD_EXECUTABLE(gemrb GemRB.cpp )
	ENDIF()

	INSTALL( FILES "${CMAKE_CURRENT_BINARY_DIR}/GemRB.cfg.sample" GemRB.cfg.noinstall.sample DESTINATION ${EXAMPLE_CONF_DIR} )
	INSTALL(FILES "${CMAKE_CURRENT_BINARY_DIR}/GemRB.cfg.sample" RENAME GemRB.cfg DESTINATION ${EXAMPLE_CONF_DIR})
ENDIF(APPLE)
target_compile_definitions(gemrb PRIVATE _USE_MATH_DEFINES)

# we need to add gemrb target before processing plugins or COCOA plugins don't work right.
ADD_SUBDIRECTORY( plugins )

# create a compilable icon from a bmp
FUNCTION(create_icon file output filename)
	FILE(READ ${file} filedata HEX)
	# convert hex data for C compatibility
	STRING(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," filedata ${filedata})
	FILE(WRITE "${PROJECT_BINARY_DIR}/${output}" "const unsigned char ${filename}[] = {${filedata}};\nconst unsigned ${filename}_size = sizeof(${filename});\n")
ENDFUNCTION()
create_icon("${CMAKE_SOURCE_DIR}/artwork/gemrb.bmp" "gemrb-icon.h" "gemrb_icon")

SET_TARGET_PROPERTIES(gemrb PROPERTIES INSTALL_RPATH ${LIB_DIR})
INCLUDE_DIRECTORIES( ${SDL_INCLUDE_DIR} )
IF(APPLE)
  # custom info.plist
  SET(PRODUCT_BUNDLE_IDENTIFIER "org.gemrb.gemrb")
  SET(EXECUTABLE_NAME "${PROJECT_NAME}")
  SET_TARGET_PROPERTIES(gemrb PROPERTIES BUNDLE True MACOSX_BUNDLE_INFO_PLIST ../platforms/apple/osx/Info.plist OUTPUT_NAME ${PROJECT_NAME})

  # compile nib
  ADD_CUSTOM_COMMAND (TARGET gemrb POST_BUILD
                      COMMAND ${IBTOOL} --errors --warnings --notices --output-format human-readable-text 
                              --compile ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.app/Contents/Resources/GemRB.nib 
                            ${CMAKE_CURRENT_SOURCE_DIR}/../platforms/apple/CocoaWrapper/mac/nibs/GemRB.xib
                      COMMENT "Compiling GemRB.xib")

  if (STATIC_LINK)
    TARGET_LINK_LIBRARIES(gemrb
        ${COCOA_LIBRARY_PATH} ${CMAKE_DL_LIBS} Threads::Threads
       gemrb_core ${plugins}  -Wl,-all_load)
  else (STATIC_LINK)
    TARGET_LINK_LIBRARIES(gemrb gemrb_core
        ${COCOA_LIBRARY_PATH} ${CMAKE_DL_LIBS} Threads::Threads)
  endif (STATIC_LINK)
ELSE(APPLE)
  if (STATIC_LINK)
    TARGET_LINK_LIBRARIES(gemrb ${CMAKE_DL_LIBS} Threads::Threads
      -Wl,--whole-archive gemrb_core ${plugins} -Wl,--no-whole-archive)
  else (STATIC_LINK)
    IF (MSVC)
      TARGET_LINK_LIBRARIES(gemrb gemrb_core)
    ELSE (MSVC)
      TARGET_LINK_LIBRARIES(gemrb gemrb_core ${CMAKE_DL_LIBS} Threads::Threads)
    ENDIF (MSVC)
  endif (STATIC_LINK)
ENDIF(APPLE)

# preconfigure the sample config with the selected paths
CONFIGURE_FILE(
  "${CMAKE_CURRENT_SOURCE_DIR}/GemRB.cfg.sample.in"
  "${CMAKE_CURRENT_BINARY_DIR}/GemRB.cfg.sample"
  @ONLY
)

# copy config for complex tests to build dir
CONFIGURE_FILE(../demo/tester.cfg "${CMAKE_BINARY_DIR}")

INSTALL( TARGETS gemrb DESTINATION ${BIN_DIR} )

# optional script to help deploy dependencies when building with windows.
IF(VCPKG_AUTO_DEPLOY)
	INCLUDE(${CMAKE_SOURCE_DIR}/platforms/windows/vcpkg_deps.cmake)
ENDIF()

IF(VITA)
	INCLUDE(${CMAKE_SOURCE_DIR}/platforms/vita/vita_build_vpk.cmake)
ENDIF(VITA)

# Tests
IF (BUILD_TESTING)
  ADD_EXECUTABLE(Test_gemrb_core
    tests/core/DCC/Test_AbilityMod.cpp
    tests/core/DCC/Test_Augur.cpp
    tests/core/DCC/Test_Body.cpp
    tests/core/DCC/Test_Character.cpp
    tests/core/DCC/Test_Classes.cpp
    tests/core/DCC/Test_Combat.cpp
    tests/core/DCC/Test_CombatRules.cpp
    tests/core/DCC/Test_CombatTables.cpp
    tests/core/DCC/Test_Deeds.cpp
    tests/core/DCC/Test_DiceChain.cpp
    tests/core/DCC/Test_Extension.cpp
    tests/core/DCC/Test_Luck.cpp
    tests/core/DCC/Test_Magic.cpp
    tests/core/DCC/Test_Learning.cpp
    tests/core/DCC/Test_Looks.cpp
    tests/core/Test_BucketPriorityQueue.cpp
    tests/core/Test_Map.cpp
    tests/core/Test_Movement.cpp
    tests/core/Test_MurmurHash.cpp
    tests/core/Test_Orient.cpp
    tests/core/Test_Palette.cpp
    tests/core/Test_PathFinder.cpp
    tests/core/Net/Test_Prompt.cpp
    tests/core/Net/Test_Protocol.cpp
    tests/core/Net/Test_Replication.cpp
    tests/core/Net/Test_Resync.cpp
    tests/core/Test_SaveGame.cpp
    tests/core/Test_Spell.cpp
    tests/core/Streams/Test_DataStream.cpp
    tests/core/Strings/Test_CString.cpp
    tests/core/Strings/Test_String.cpp
    tests/core/Strings/Test_StringView.cpp
    tests/core/Strings/Test_UTF8Comparison.cpp
    tests/core/System/Test_VFS.cpp
  )

  target_compile_definitions(Test_gemrb_core PRIVATE _USE_MATH_DEFINES)
  TARGET_LINK_LIBRARIES(Test_gemrb_core GTest::gtest GTest::gtest_main gemrb_core ${Iconv_LIBRARY})
  IF (WIN32)
    TARGET_LINK_LIBRARIES(Test_gemrb_core shlwapi)
  ENDIF()

  IF (MSVC)
    ADD_TEST(NAME Test_gemrb_core
      COMMAND ${CMAKE_COMMAND} -E env "PATH=${CMAKE_BINARY_DIR}/gemrb;$ENV{PATH}" Test_gemrb_core
      WORKING_DIRECTORY $<TARGET_FILE_DIR:gemrb_core>
    )
  ELSE()
    ADD_TEST(NAME Test_gemrb_core COMMAND Test_gemrb_core
      WORKING_DIRECTORY $<TARGET_FILE_DIR:gemrb_core>
    )
  ENDIF()

  # ethren (M3.1): the funnel's rolls (GUIScripts/ethren/FunnelRoll.py) need no engine, only a Python
  FIND_PROGRAM(GEMRB_TEST_PYTHON NAMES python3 python)
  IF (GEMRB_TEST_PYTHON)
    ADD_TEST(NAME Test_FunnelRoll COMMAND ${GEMRB_TEST_PYTHON} ${CMAKE_SOURCE_DIR}/gemrb/tests/GUIScripts/test_funnelroll.py)
  ENDIF()

  ADD_CUSTOM_COMMAND(TARGET Test_gemrb_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:gemrb_core>/tests/resources
      && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/gemrb/tests/resources $<TARGET_FILE_DIR:gemrb_core>/tests/resources
  )
ENDIF()
