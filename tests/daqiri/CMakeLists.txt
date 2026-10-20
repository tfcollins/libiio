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

    # With libiio-emu.so as a module, iiod-emu crashes on attribute writes
    # (the module calls the non-exported iio_strndup).
    if (WITH_EMU_BACKEND_DYNAMIC)
        message(
            WARNING
                "daqiri loopback tests need -DWITH_EMU_BACKEND_DYNAMIC=OFF: "
                "iiod-emu crashes on attribute writes when emu is a module")
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

    # iiod-emu fills a block from its .bin with at most one wrap, so a .bin
    # shorter than a block leaves the end of the block unwritten. Serve a copy
    # of fmcomms2.xml next to the stock cf-ad9361-lpc_buf0.bin (2019 bytes)
    # three times over, which fills the 4096-byte blocks of the test.
    set(EQ_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/equivalence)
    add_custom_command(
        OUTPUT ${EQ_FIXTURE}/fmcomms2.xml ${EQ_FIXTURE}/cf-ad9361-lpc_buf0.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${EQ_FIXTURE}
        COMMAND ${CMAKE_COMMAND} -E copy ${XMLS}/fmcomms2.xml
                ${EQ_FIXTURE}/fmcomms2.xml
        COMMAND
            sh -c "cat \"$0\" \"$0\" \"$0\" > \"$1\""
            ${XMLS}/cf-ad9361-lpc_buf0.bin ${EQ_FIXTURE}/cf-ad9361-lpc_buf0.bin
        DEPENDS ${XMLS}/fmcomms2.xml ${XMLS}/cf-ad9361-lpc_buf0.bin
        VERBATIM)
    add_custom_target(
        daqiri_equivalence_fixture DEPENDS ${EQ_FIXTURE}/fmcomms2.xml
                                           ${EQ_FIXTURE}/cf-ad9361-lpc_buf0.bin)

    find_package(Threads REQUIRED)
    add_executable(test_block_equivalence test_block_equivalence.c)
    target_link_libraries(test_block_equivalence PRIVATE iio Threads::Threads)
    target_include_directories(test_block_equivalence
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    target_compile_definitions(
        test_block_equivalence
        PRIVATE TESTS_DEBUG TESTS_BUF0_BIN="${XMLS}/cf-ad9361-lpc_buf0.bin")
    add_dependencies(test_block_equivalence iio-daqiri iiod-emu
                     daqiri_equivalence_fixture)

    add_test(
        NAME daqiri_block_equivalence
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/with_emu.sh ${EQ_FIXTURE}/fmcomms2.xml
            $<TARGET_FILE:test_block_equivalence>)
    # IIO_DAQIRI_YAML is cleared: its framing=vrt case expects no YAML.
    set_tests_properties(
        daqiri_block_equivalence
        PROPERTIES LABELS daqiri-loopback ENVIRONMENT
                   "IIOD_EMU=$<TARGET_FILE:iiod-emu>;IIO_DAQIRI_YAML="
                   RESOURCE_LOCK iiod-emu-30431)

    # Opens a buffer through TESTS_API_URI; run_loopback.sh runs it per XML so
    # that the api label cannot pass on buffers that never opened.
    add_executable(test_daqiri_open test_daqiri_open.c)
    target_link_libraries(test_daqiri_open PRIVATE iio)
    target_include_directories(test_daqiri_open
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    target_compile_definitions(test_daqiri_open PRIVATE TESTS_DEBUG)
    add_dependencies(test_daqiri_open iio-daqiri iiod-emu)

    # framing=vrt buffer lifecycle over the DAQIRI socket engine. The engine
    # binds UDP 127.0.0.1:25101 (see the YAML), hence the second lock.
    add_executable(test_daqiri_lifecycle test_daqiri_lifecycle.c)
    target_link_libraries(test_daqiri_lifecycle PRIVATE iio)
    target_include_directories(test_daqiri_lifecycle
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    target_compile_definitions(test_daqiri_lifecycle PRIVATE TESTS_DEBUG)
    add_dependencies(test_daqiri_lifecycle iio-daqiri iiod-emu)

    add_test(NAME daqiri_lifecycle
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/with_emu.sh
                     ${XMLS}/fmcomms2.xml $<TARGET_FILE:test_daqiri_lifecycle>)
    set_tests_properties(
        daqiri_lifecycle
        PROPERTIES
            LABELS
            daqiri-loopback
            ENVIRONMENT
            "IIOD_EMU=$<TARGET_FILE:iiod-emu>;DAQIRI_YAML=${CMAKE_CURRENT_SOURCE_DIR}/resources/socket-udp-loopback.yaml"
            RESOURCE_LOCK
            "iiod-emu-30431;daqiri-udp-25101")

    # The channel-length rule needs a device with 16- and 32-bit channels
    add_test(
        NAME daqiri_lifecycle_mixed
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/with_emu.sh
            ${XMLS}/float_scan_elements.xml
            $<TARGET_FILE:test_daqiri_lifecycle> --mixed)
    set_tests_properties(
        daqiri_lifecycle_mixed
        PROPERTIES
            LABELS
            daqiri-loopback
            ENVIRONMENT
            "IIOD_EMU=$<TARGET_FILE:iiod-emu>;DAQIRI_YAML=${CMAKE_CURRENT_SOURCE_DIR}/resources/socket-udp-loopback.yaml"
            RESOURCE_LOCK
            "iiod-emu-30431;daqiri-udp-25101")

    # Standalone VITA 49.2 software framer (libc only; sendmmsg and
    # clock_nanosleep make it Linux-only, hence here and not with the unit tests)
    add_executable(vrt_sender vrt_sender.c)
    set_target_properties(
        vrt_sender
        PROPERTIES C_STANDARD 99
                   C_STANDARD_REQUIRED ON
                   C_EXTENSIONS OFF)

    # framing=vrt end to end: vrt_sender streams to the engine from the YAML's
    # remote_addr while the test checks blocks and counters against its oracle.
    add_executable(test_vrt_loopback test_vrt_loopback.c)
    target_link_libraries(test_vrt_loopback PRIVATE iio)
    target_include_directories(test_vrt_loopback
                               PRIVATE ${CMAKE_SOURCE_DIR}/tests/api)
    target_compile_definitions(test_vrt_loopback PRIVATE TESTS_DEBUG)
    add_dependencies(test_vrt_loopback iio-daqiri iiod-emu vrt_sender)

    add_test(NAME daqiri_vrt_loopback
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/with_emu.sh
                     ${XMLS}/fmcomms2.xml $<TARGET_FILE:test_vrt_loopback>)
    set_tests_properties(
        daqiri_vrt_loopback
        PROPERTIES
            LABELS
            daqiri-loopback
            ENVIRONMENT
            "IIOD_EMU=$<TARGET_FILE:iiod-emu>;DAQIRI_YAML=${CMAKE_CURRENT_SOURCE_DIR}/resources/socket-udp-loopback.yaml;VRT_SENDER=$<TARGET_FILE:vrt_sender>"
            RESOURCE_LOCK
            "iiod-emu-30431;daqiri-udp-25101"
            TIMEOUT
            60)

    # The api label, once per XML, with TESTS_API_URI pointing at daqiri:.
    add_test(
        NAME daqiri_loopback_api
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_loopback.sh ${CMAKE_BINARY_DIR}
                $<TARGET_FILE:test_daqiri_open> ${XMLS})
    set_tests_properties(
        daqiri_loopback_api
        PROPERTIES LABELS
                   daqiri-loopback
                   ENVIRONMENT
                   "IIOD_EMU=$<TARGET_FILE:iiod-emu>"
                   RESOURCE_LOCK
                   iiod-emu-30431
                   RUN_SERIAL
                   TRUE
                   TIMEOUT
                   300)
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
