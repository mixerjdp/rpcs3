include(FetchContent)

set(RPCS3_LIBRETRO_API_DIR "" CACHE PATH
    "Directory containing the official libretro.h header. When empty, libretro-common is fetched at configure time.")
set(RPCS3_RETROARCH_ROOT "" CACHE PATH
    "Optional RetroArch directory. The core and its .info file are copied here after a successful build.")
option(RPCS3_LIBRETRO_BUILD_SMOKE_TEST
	"Add a small executable that exercises the core ABI." OFF)

if(RPCS3_LIBRETRO_API_DIR)
    set(_rpcs3_libretro_include_dir "${RPCS3_LIBRETRO_API_DIR}")
else()
    # Pin the API used by the core so an upstream header change cannot silently
    # alter the ABI of an existing RPCS3 checkout.
    FetchContent_Declare(rpcs3_libretro_api
        GIT_REPOSITORY https://github.com/libretro/libretro-common.git
        GIT_TAG 413cc8ea5d05e509970975c0d034ea19406245e6
        GIT_PROGRESS TRUE
        SOURCE_SUBDIR _header_only
    )
    FetchContent_MakeAvailable(rpcs3_libretro_api)
    set(_rpcs3_libretro_include_dir "${rpcs3_libretro_api_SOURCE_DIR}/include")
endif()

if(NOT EXISTS "${_rpcs3_libretro_include_dir}/libretro.h")
    message(FATAL_ERROR
        "RPCS3_LIBRETRO_API_DIR must point to a directory containing libretro.h "
        "(received '${_rpcs3_libretro_include_dir}').")
endif()

add_library(rpcs3_libretro SHARED
	LibretroCore.cpp
	LibretroCoreState.cpp
	LibretroCoreState.h
	LibretroEmulator.cpp
	LibretroEmulator.h
	LibretroInput.h
	LibretroPadThread.cpp
	../rpcs3_version.cpp
	../Input/product_info.cpp
	../Input/ps_move_config.cpp
	../Input/ps_move_tracker.cpp
)

target_compile_features(rpcs3_libretro PRIVATE cxx_std_23)
target_compile_definitions(rpcs3_libretro PRIVATE RPCS3_LIBRETRO=1)
target_include_directories(rpcs3_libretro SYSTEM PRIVATE "${_rpcs3_libretro_include_dir}")
target_link_libraries(rpcs3_libretro PRIVATE
	rpcs3_emu
	3rdparty::fusion
)

if(WIN32)
	target_compile_definitions(rpcs3_libretro PRIVATE UNICODE _UNICODE)
	target_link_libraries(rpcs3_libretro PRIVATE
		ws2_32 Iphlpapi Winmm Psapi gdi32 setupapi user32 shell32 ole32 opengl32)

	# The prebuilt static LLVM package used by the Windows build exposes the
	# Intel JIT event listener as a separate archive. The Qt frontend receives
	# it transitively from its larger target graph; the small libretro frontend
	# must name it explicitly.
	if(WITH_LLVM AND STATIC_LINK_LLVM)
		get_filename_component(_rpcs3_llvm_library_dir
			"${LLVM_DIR}/../.." ABSOLUTE)
		target_link_libraries(rpcs3_libretro PRIVATE
			"${_rpcs3_llvm_library_dir}/LLVMIntelJITEvents.lib")
	endif()
endif()

set_target_properties(rpcs3_libretro PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME rpcs3_libretro
)

if(MSVC)
    set_property(TARGET rpcs3_libretro PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(rpcs3_libretro PRIVATE /W4 /permissive-)
    # Keep symbols for diagnosing unload/lifecycle failures in the host process
    # even when testing the optimized Release core.
    target_link_options(rpcs3_libretro PRIVATE
        /DEBUG:FULL
        /OPT:REF
        /OPT:ICF
        "/MAP:$<TARGET_FILE_DIR:rpcs3_libretro>/rpcs3_libretro.map")
else()
    target_compile_options(rpcs3_libretro PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(RPCS3_LIBRETRO_VERSION "0.3.0-audio-input-preview")
configure_file(rpcs3_libretro.info.in rpcs3_libretro.info @ONLY)

install(TARGETS rpcs3_libretro
    RUNTIME DESTINATION cores
    LIBRARY DESTINATION cores
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rpcs3_libretro.info"
    DESTINATION info
)

if(RPCS3_RETROARCH_ROOT)
    add_custom_command(TARGET rpcs3_libretro POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RPCS3_RETROARCH_ROOT}/cores"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RPCS3_RETROARCH_ROOT}/info"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:rpcs3_libretro>"
            "${RPCS3_RETROARCH_ROOT}/cores/$<TARGET_FILE_NAME:rpcs3_libretro>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/rpcs3_libretro.info"
            "${RPCS3_RETROARCH_ROOT}/info/rpcs3_libretro.info"
		COMMENT "Deploying the RPCS3 libretro core to ${RPCS3_RETROARCH_ROOT}"
        VERBATIM
    )
endif()

if(RPCS3_LIBRETRO_BUILD_SMOKE_TEST)
    add_executable(rpcs3_libretro_smoke_test EXCLUDE_FROM_ALL
        LibretroSmokeTest.cpp
    )
    target_compile_features(rpcs3_libretro_smoke_test PRIVATE cxx_std_23)
    target_include_directories(rpcs3_libretro_smoke_test SYSTEM PRIVATE
        "${_rpcs3_libretro_include_dir}")
    target_link_libraries(rpcs3_libretro_smoke_test PRIVATE rpcs3_libretro)

    if(MSVC)
        set_property(TARGET rpcs3_libretro_smoke_test PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(rpcs3_libretro_smoke_test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(rpcs3_libretro_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_custom_target(check_rpcs3_libretro
        COMMAND "$<TARGET_FILE:rpcs3_libretro_smoke_test>"
        DEPENDS rpcs3_libretro_smoke_test
        COMMENT "Running the RPCS3 libretro ABI smoke test"
        USES_TERMINAL
    )
endif()
