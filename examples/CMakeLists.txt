# Builds the official LV2 example plugins (ISC licensed) into
# <build>/bin/lv2/<bundle>.lv2 so the host finds them next to the executable.
include(FetchContent)
FetchContent_Declare(lv2_examples
    GIT_REPOSITORY https://github.com/lv2/lv2.git
    GIT_TAG v1.18.10
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(lv2_examples)
set(LV2_SRC ${lv2_examples_SOURCE_DIR})
set(LV2_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lv2)

function(lovehost_example_plugin bundle libname)
    set(src_dir ${LV2_SRC}/plugins/${bundle}.lv2)
    set(out_dir ${LV2_OUT}/${bundle}.lv2)
    add_library(${libname} MODULE ${src_dir}/${libname}.c)
    target_include_directories(${libname} PRIVATE ${LV2_SRC}/include ${LV2_INCLUDE_DIR} ${src_dir})
    target_compile_definitions(${libname} PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${libname} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${libname}
        LIBRARY_OUTPUT_DIRECTORY ${out_dir}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${out_dir}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${out_dir}
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${out_dir}
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${out_dir}
        FOLDER examples)
    # Turtle files: *.ttl.in has a @LIB_EXT@ placeholder, others are copied as-is.
    set(LIB_EXT ".dll")
    file(GLOB ttl_in ${src_dir}/*.ttl.in)
    foreach(f ${ttl_in})
        get_filename_component(name ${f} NAME_WLE)
        configure_file(${f} ${out_dir}/${name} @ONLY)
    endforeach()
    file(GLOB ttl ${src_dir}/*.ttl)
    foreach(f ${ttl})
        get_filename_component(name ${f} NAME)
        configure_file(${f} ${out_dir}/${name} COPYONLY)
    endforeach()
endfunction()

lovehost_example_plugin(eg-amp amp)          # stereo-less gain effect (1 in / 1 out)
lovehost_example_plugin(eg-midigate midigate) # MIDI-gated audio effect
lovehost_example_plugin(eg-fifths fifths)     # MIDI -> MIDI (adds a fifth), atom out
lovehost_example_plugin(eg-params params)     # control + patch parameter demo

# lovehost's own Win32 test UI for eg-amp, placed inside the eg-amp bundle.
add_library(amp_ui MODULE ${CMAKE_CURRENT_SOURCE_DIR}/amp-ui/amp_ui.c)
target_include_directories(amp_ui PRIVATE ${LV2_INCLUDE_DIR})
target_compile_definitions(amp_ui PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(amp_ui PRIVATE user32 gdi32 comctl32)
set_target_properties(amp_ui PROPERTIES PREFIX "" OUTPUT_NAME amp_ui FOLDER examples)
foreach(cfg "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(amp_ui PROPERTIES LIBRARY_OUTPUT_DIRECTORY${cfg} ${LV2_OUT}/eg-amp.lv2)
endforeach()
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/amp-ui/amp_ui.ttl AMP_UI_TTL)
file(APPEND ${LV2_OUT}/eg-amp.lv2/manifest.ttl "${AMP_UI_TTL}")
