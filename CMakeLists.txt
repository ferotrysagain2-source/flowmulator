cmake_minimum_required(VERSION 3.20)
project(Flowmulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

set(FLOWMULATOR_ASSETS
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/frontend_background.bmp
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/edgerunners_background.bmp
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/berserk_background.bmp
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/gba_overlay.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/nds_overlay.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/nds_top_overlay.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/jackhammer.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/jackhammer.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/big_forehead.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/skywalker.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/water_splash.rgba
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/skywalker_splash.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/skywalker_oooooh.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/.assets/flowmulator.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/.runtime/azahar.AppImage
    ${CMAKE_CURRENT_SOURCE_DIR}/.runtime/azahar_libretro.so
    ${CMAKE_CURRENT_SOURCE_DIR}/.runtime/mgba_libretro.so
    ${CMAKE_CURRENT_SOURCE_DIR}/.runtime/desmume_libretro.so
    ${CMAKE_CURRENT_SOURCE_DIR}/.runtime/MiiFix.3dsx
    ${CMAKE_CURRENT_SOURCE_DIR}/Helper.sh)

if (WIN32)
  if (NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "A MinGW/LLVM objcopy is required to embed runtime assets")
  endif()
  set(ASSET_OBJECTS)
  foreach(ASSET ${FLOWMULATOR_ASSETS})
    get_filename_component(ASSET_NAME ${ASSET} NAME)
    file(RELATIVE_PATH ASSET_REL ${CMAKE_CURRENT_SOURCE_DIR} ${ASSET})
    set(ASSET_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/${ASSET_NAME}.obj)
    add_custom_command(
        OUTPUT ${ASSET_OBJECT}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_OBJCOPY}
            --input-target binary --output-target pei-x86-64
            --binary-architecture i386:x86-64
            ${ASSET_REL} ${ASSET_OBJECT}
        DEPENDS ${ASSET}
        VERBATIM)
    list(APPEND ASSET_OBJECTS ${ASSET_OBJECT})
  endforeach()
else()
  set(ASSET_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.o)
  add_custom_command(
      OUTPUT ${ASSET_OBJECT}
      COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
          ld -r -b binary -o ${ASSET_OBJECT}
          .assets/frontend_background.bmp
          .assets/edgerunners_background.bmp
          .assets/berserk_background.bmp
          .assets/gba_overlay.rgba
          .assets/nds_overlay.rgba
          .assets/nds_top_overlay.rgba
          .assets/jackhammer.rgba
          .assets/jackhammer.wav
          .assets/big_forehead.wav
          .assets/skywalker.rgba
          .assets/water_splash.rgba
          .assets/skywalker_splash.wav
          .assets/skywalker_oooooh.wav
          .assets/flowmulator.svg
          .runtime/azahar.AppImage
          .runtime/azahar_libretro.so
          .runtime/mgba_libretro.so
          .runtime/desmume_libretro.so
          .runtime/MiiFix.3dsx
          Helper.sh
      DEPENDS ${FLOWMULATOR_ASSETS}
      VERBATIM)
  set(ASSET_OBJECTS ${ASSET_OBJECT})
endif()

add_executable(Flowmulator WIN32 main.cpp ${ASSET_OBJECTS})
target_include_directories(Flowmulator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Flowmulator PRIVATE SDL2::SDL2 OpenGL::GL)

if (WIN32)
  target_link_libraries(Flowmulator PRIVATE shell32)
  set_target_properties(Flowmulator PROPERTIES OUTPUT_NAME "Flowmulator")
endif()

add_custom_command(TARGET Flowmulator POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:Flowmulator>/ROMS/GBA
    $<TARGET_FILE_DIR:Flowmulator>/ROMS/NDS
    $<TARGET_FILE_DIR:Flowmulator>/ROMS/3DS
    $<TARGET_FILE_DIR:Flowmulator>/ROMS/WII
    $<TARGET_FILE_DIR:Flowmulator>/ROMS/GAMECUBE
    $<TARGET_FILE_DIR:Flowmulator>/SAVES
  )
