project(OFS-SE)

set(OPEN_FUNSCRIPTER_SOURCES
  "main.cpp"
  "OpenFunscripter.cpp"
  "OFS_ScriptingMode.cpp"
  "OFS_Project.cpp"
  
  "OFS_UndoSystem.cpp"
  "OFS_UiDriver.cpp"

  "UI/OFS_Preferences.cpp"
  "UI/OFS_ScriptSimulator.cpp"
  "UI/OFS_SpecialFunctions.cpp"
  "UI/OFS_ScriptPositionsOverlays.cpp"
  "UI/OFS_DownloadFfmpeg.cpp"
  "UI/OFS_FunscriptMetadataEditor.cpp"
  "UI/OFS_ChapterManager.cpp"
  "UI/OFS_ScriptCheck.cpp"

  "api/OFS_DeviceLink.cpp"
  "api/OFS_TCode.cpp"
  "api/OFS_WebsocketApi.cpp"
  "api/OFS_WebsocketApiClient.cpp"
  "api/OFS_WebsocketApiEvents.cpp"
  "api/OFS_WebsocketApiCommands.cpp"

  "gl/OFS_GPU.cpp"

  "state/OpenFunscripterState.cpp"

  "lua/OFS_LuaExtensions.cpp"
  "lua/OFS_LuaExtension.cpp"
  "lua/OFS_LuaCoreExtension.cpp"
  "lua/OFS_LuaExtensionAPI.cpp"
  "lua/api/OFS_LuaPlayerAPI.cpp"
  "lua/api/OFS_LuaImGuiAPI.cpp"
  "lua/api/OFS_LuaScriptAPI.cpp"
  "lua/api/OFS_LuaProcessAPI.cpp"
)

if(WIN32)
	set(OPEN_FUNSCRIPTER_SOURCES ${OPEN_FUNSCRIPTER_SOURCES} "../icon.rc")
endif()

if(APPLE)
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${OPEN_FUNSCRIPTER_SOURCES})
elseif(WIN32)
	add_executable(${PROJECT_NAME} WIN32 ${OPEN_FUNSCRIPTER_SOURCES})
else()
	add_executable(${PROJECT_NAME} ${OPEN_FUNSCRIPTER_SOURCES})
endif()

target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR})

# copy data directory
if(APPLE)
	add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
						COMMAND ${CMAKE_COMMAND} -E copy_directory
						"${CMAKE_SOURCE_DIR}/data/" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Resources/data")
else()
	# copy_if_different so an already running OFS-SE does not fail the build by
	# holding a lock on files that did not actually change.
	add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
						COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
						"${CMAKE_SOURCE_DIR}/data/" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/data")
endif()


target_link_libraries(${PROJECT_NAME} PUBLIC
  lua
  sol2
  OFS_lib
  civetweb
)

if(WIN32)
  # Downloads url to dest, trying again when the transfer fails or, given a
  # SHA256, when the file is not the one expected. A cut off download would
  # otherwise only show up at build time, as an archive 7zr cannot open. A
  # file already there with the right hash is kept.
  function(ofs_download url dest)
    cmake_parse_arguments(ARG "" "SHA256" "" ${ARGN})
    if(ARG_SHA256 AND EXISTS "${dest}")
      file(SHA256 "${dest}" have)
      if(have STREQUAL ARG_SHA256)
        return()
      endif()
    endif()
    foreach(attempt RANGE 1 5)
      file(DOWNLOAD "${url}" "${dest}" SHOW_PROGRESS STATUS status)
      list(GET status 0 code)
      list(GET status 1 reason)
      if(code EQUAL 0)
        file(SIZE "${dest}" size)
        if(ARG_SHA256)
          file(SHA256 "${dest}" have)
          if(have STREQUAL ARG_SHA256)
            return()
          endif()
          set(reason "got ${size} bytes that do not match the expected SHA256")
        elseif(size GREATER 0)
          return()
        else()
          set(reason "got an empty file")
        endif()
      endif()
      message(WARNING "Download of ${url} failed (attempt ${attempt} of 5): ${reason}")
    endforeach()
    file(REMOVE "${dest}")
    message(FATAL_ERROR "Could not download ${url}. Check the connection and configure again.")
  endfunction()

  # download mpv binaries
  if (NOT EXISTS "${CMAKE_BINARY_DIR}/mpv-2.dll")
    ofs_download("https://www.7-zip.org/a/7zr.exe" "${CMAKE_BINARY_DIR}/7zr.exe")
    ofs_download("https://sourceforge.net/projects/mpv-player-windows/files/libmpv/mpv-dev-x86_64-v3-20220925-git-56e24d5.7z/download" "${CMAKE_BINARY_DIR}/mpv-dev.7z"
      SHA256 b7ec2fca1f70cb4c7f5e9e4ff03127691e65158b088b3e74c948629b62c6036b)
    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND "${CMAKE_BINARY_DIR}/7zr.exe" "x" "-y" "mpv-dev.7z" "mpv-2.dll" 
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/mpv-2.dll"
        DEPENDS "${CMAKE_BINARY_DIR}/mpv-dev.7z")    
  endif()

	# copy mpv-2.dll to bin dir
	add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                   "${CMAKE_BINARY_DIR}/mpv-2.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/mpv-2.dll"
				   DEPENDS "${CMAKE_BINARY_DIR}/mpv-2.dll")
endif()

target_compile_definitions(${PROJECT_NAME} PUBLIC
	"_CRT_SECURE_NO_WARNINGS"
)

target_include_directories(${PROJECT_NAME} PRIVATE 
	"${PROJECT_SOURCE_DIR}/"
	"${PROJECT_SOURCE_DIR}/UI/"
	"${PROJECT_SOURCE_DIR}/Funscript/"
	"${PROJECT_SOURCE_DIR}/gl/"
	"${PROJECT_SOURCE_DIR}/event/"
	"${PROJECT_SOURCE_DIR}/lua/"
	"${PROJECT_SOURCE_DIR}/api/"
)

# c++17
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

if(WIN32)
	if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo") 
		target_compile_options(${PROJECT_NAME} PUBLIC /GL)
		target_link_options(${PROJECT_NAME} PUBLIC /LTCG)
		message("== ${PROJECT_NAME} - Whole program optimization enabled.")
	endif()
elseif(UNIX AND NOT APPLE) # clang/gcc
    target_compile_options(${PROJECT_NAME} PUBLIC -fpermissive)
	install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "bin/")
elseif(APPLE)
	target_compile_options(${PROJECT_NAME} PUBLIC -fpermissive)
	# Note Mac specific extension .app
	set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app")
endif()


if(OFS_SNAP_IMAGE)
# this is awful
target_compile_definitions(${PROJECT_NAME} PRIVATE
	"OFS_SNAP_IMAGE")
install(DIRECTORY "../data" DESTINATION "bin/")
endif()
