set(XMLS ${CMAKE_SOURCE_DIR}/tests/resources/xmls)

# Loopback tests: a daqiri: context against iiod-emu on 127.0.0.1:30431.
if (WITH_DAQIRI_BACKEND AND WITH_IIOD_EMU)
    # libiio looks modules up in IIO_MODULES_DIR, which is baked in at configure
    # time; without the override these tests would load whatever is installed.
    if (NOT IIO_MODULES_DIR_OVERRIDE)
        message(
            WARNING
                "daqiri loopback tests need -DIIO_MODULES_DIR_OVERRIDE=${CMAKE_BINARY_DIR}/ "
                "to load the modules from the build tree")
    endif ()

    add_executable(test_daqiri_context test_daqiri_context.c)
    target_link_libraries(test_daqiri_context PRIVATE iio)
    target_include_directories(test_daqiri_context
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    # The test only links libiio; make sure the module and the emulator it
    # exercises are rebuilt before it runs.
    add_dependencies(test_daqiri_context iio-daqiri iiod-emu)

    add_test(NAME daqiri_context
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/with_emu.sh
                     ${XMLS}/fmcomms2.xml $<TARGET_FILE:test_daqiri_context>)
    set_tests_properties(
        daqiri_context
        PROPERTIES LABELS daqiri-loopback ENVIRONMENT
                   "IIOD_EMU=$<TARGET_FILE:iiod-emu>" RESOURCE_LOCK
                   iiod-emu-30431)
endif ()

# VITA 49.2 prologue parser unit test: header-only, needs neither libiio's
# daqiri module nor the DAQIRI SDK, so it is built whenever tests are.
if (WITH_TESTS AND NOT WIN32)
    add_executable(test_vrt_prologue test_vrt_prologue.c)
    target_include_directories(test_vrt_prologue
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    target_compile_definitions(
        test_vrt_prologue
        PRIVATE TESTS_VRT_DIR="${CMAKE_SOURCE_DIR}/tests/resources/vrt")
    set_target_properties(
        test_vrt_prologue
        PROPERTIES C_STANDARD 99
                   C_STANDARD_REQUIRED ON
                   C_EXTENSIONS OFF)
    add_test(NAME test_vrt_prologue COMMAND test_vrt_prologue)
    set_tests_properties(test_vrt_prologue PROPERTIES LABELS daqiri)

    # Sequence tracker and block assembler unit tests, built against the
    # DAQIRI-free framing objects (iio_daqiri_framing, top-level CMakeLists).
    add_executable(test_vrt_gap test_vrt_gap.c)
    target_link_libraries(test_vrt_gap PRIVATE iio_daqiri_framing)
    target_include_directories(test_vrt_gap
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    set_target_properties(
        test_vrt_gap
        PROPERTIES C_STANDARD 99
                   C_STANDARD_REQUIRED ON
                   C_EXTENSIONS OFF)
    add_test(NAME test_vrt_gap COMMAND test_vrt_gap)
    set_tests_properties(test_vrt_gap PROPERTIES LABELS daqiri)

    add_executable(test_vrt_assembler test_vrt_assembler.c)
    target_link_libraries(test_vrt_assembler PRIVATE iio_daqiri_framing)
    target_include_directories(test_vrt_assembler
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    set_target_properties(
        test_vrt_assembler
        PROPERTIES C_STANDARD 99
                   C_STANDARD_REQUIRED ON
                   C_EXTENSIONS OFF)
    add_test(NAME test_vrt_assembler COMMAND test_vrt_assembler)
    set_tests_properties(test_vrt_assembler PROPERTIES LABELS daqiri)
endif ()
