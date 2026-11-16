# Core C17 Library - TuxRT Real-time Core

cmake_minimum_required(VERSION 3.20)
project(tuxrt_core LANGUAGES C)

# Version
set(TUXRT_CORE_VERSION_MAJOR 0)
set(TUXRT_CORE_VERSION_MINOR 1)
set(TUXRT_CORE_VERSION_PATCH 0)
set(TUXRT_CORE_VERSION "${TUXRT_CORE_VERSION_MAJOR}.${TUXRT_CORE_VERSION_MINOR}.${TUXRT_CORE_VERSION_PATCH}")

# Metadatos embebidos en tuxrt_sdk_version() (build_system.c).
# - commit git corto (si el árbol es un repositorio; si no, "unknown")
# - versión de Mojo declarada en pixi.toml (fallback "unknown")
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    OUTPUT_VARIABLE TUXRT_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT TUXRT_GIT_COMMIT)
    set(TUXRT_GIT_COMMIT "unknown")
endif()

set(TUXRT_MOJO_VERSION "unknown")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../pixi.toml")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../pixi.toml" _tuxrt_pixi)
    if(_tuxrt_pixi MATCHES "mojo[^\n]*([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(TUXRT_MOJO_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

# C17 standard, strict
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Include CheckCCompilerFlag for SIMD detection
include(CheckCCompilerFlag)

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(BASE_FLAGS
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
        -Werror=incompatible-pointer-types -Werror=implicit-int -Werror=return-type
        -Werror=format=2 -Wshadow -Wpointer-arith -Wcast-align=strict
        -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
        -Wredundant-decls -Wnested-externs -fno-common -fstrict-aliasing
        -fvisibility=hidden -Wno-error=unused-variable -Wno-error=unused-function)

    # SIMD ISA detection.
    # NOTE: check_c_compiler_flag() only verifies the *compiler* accepts a flag,
    # NOT that the *host CPU* can execute the emitted instructions. Enabling e.g.
    # -mavx512f on a CPU without AVX-512 makes the compiler emit AVX-512VL
    # encodings (e.g. `vpbroadcastq %rsi,%xmm0`) that raise SIGILL at runtime.
    # Detect the real host ISA from the predefined macros of the native target.
    file(WRITE "${CMAKE_BINARY_DIR}/simd_probe.c" "int main(void){return 0;}\n")

    function(tuxrt_host_has_isa _out _macro)
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -march=native -dM -E "${CMAKE_BINARY_DIR}/simd_probe.c"
            OUTPUT_VARIABLE _defs
            ERROR_QUIET
            RESULT_VARIABLE _rc)
        if(_rc EQUAL 0 AND _defs MATCHES "#define ${_macro} 1")
            set(${_out} TRUE PARENT_SCOPE)
        else()
            set(${_out} FALSE PARENT_SCOPE)
        endif()
    endfunction()

    tuxrt_host_has_isa(HOST_HAS_AVX2     __AVX2__)
    tuxrt_host_has_isa(HOST_HAS_AVX512F  __AVX512F__)
    tuxrt_host_has_isa(HOST_HAS_AVX512BW __AVX512BW__)
    tuxrt_host_has_isa(HOST_HAS_AVX512DQ __AVX512DQ__)
    tuxrt_host_has_isa(HOST_HAS_AVX512VL __AVX512VL__)
    tuxrt_host_has_isa(HOST_HAS_FMA      __FMA__)
    tuxrt_host_has_isa(HOST_HAS_SSE42    __SSE4_2__)

    set(SIMD_FLAGS "")
    if(HOST_HAS_AVX2)
        list(APPEND SIMD_FLAGS "-mavx2")
        add_definitions(-DHAVE_AVX2)
    endif()
    if(HOST_HAS_AVX512F)
        list(APPEND SIMD_FLAGS "-mavx512f")
        add_definitions(-DHAVE_AVX512F)
    endif()
    if(HOST_HAS_AVX512BW)
        list(APPEND SIMD_FLAGS "-mavx512bw")
        add_definitions(-DHAVE_AVX512BW)
    endif()
    if(HOST_HAS_AVX512DQ)
        list(APPEND SIMD_FLAGS "-mavx512dq")
        add_definitions(-DHAVE_AVX512DQ)
    endif()
    if(HOST_HAS_AVX512VL)
        list(APPEND SIMD_FLAGS "-mavx512vl")
        add_definitions(-DHAVE_AVX512VL)
    endif()
    if(HOST_HAS_FMA)
        list(APPEND SIMD_FLAGS "-mfma")
        add_definitions(-DHAVE_FMA)
    endif()
    if(HOST_HAS_SSE42)
        list(APPEND SIMD_FLAGS "-msse4.2")
        add_definitions(-DHAVE_SSE42)
    endif()
    message(STATUS "Host SIMD ISA: AVX2=${HOST_HAS_AVX2} AVX512F=${HOST_HAS_AVX512F} "
                   "AVX512VL=${HOST_HAS_AVX512VL} FMA=${HOST_HAS_FMA} SSE4.2=${HOST_HAS_SSE42}")

    set(DEBUG_FLAGS
        -O0 -g3 -fno-omit-frame-pointer)
    set(RELEASE_FLAGS
        -O3 -march=native -DNDEBUG)
    set(HARDENING_FLAGS
        -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat-security -fPIE)

    # -fPIC is required for shared library linking
    add_compile_options(${BASE_FLAGS} ${HARDENING_FLAGS} ${SIMD_FLAGS} -fPIC)
endif()

# Config-specific flags
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(ZLIB REQUIRED zlib)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONC REQUIRED json-c)

# Optional: liburing for io_uring transport
find_library(URING_LIB uring)
if(URING_LIB)
    set(HAVE_URING 1)
    add_definitions(-DHAVE_URING)
endif()

# Sources
set(CORE_SOURCES
    # --- core: runtime, coordinación, contrato y configuración ---
    src/core/scheduler.c
    src/core/sirc.c
    src/core/tuxrt_mutex.c
    src/core/build_system.c
    src/core/error_boundary.c
    src/core/observability.c
    src/core/config.c
    src/core/provider.c
    src/core/dependency.c
    src/core/autocfg.c
    # --- motion: cinemática, planificación y generación de pasos ---
    src/motion/motion.c
    src/motion/motion_lookahead.c
    src/motion/trapq.c
    src/motion/stepcompress.c
    src/motion/steppersync.c
    src/motion/input_shaper.c

    src/motion/kinematics.c
    # --- parser de G-code ---
    src/parser/gcode_parser.c
    # --- compresión (KSC / JLPP) ---
    src/compression/ksc.c
    src/compression/jlpp.c
    # --- integridad (CRC) ---
    src/integrity/ultracrc.c
    # --- aceleración: detección HW, runtime SIMD y HAL (CRC-32C / GF(2^8) / RS) ---
    src/accel/hwdet.c
    src/accel/accel_runtime.c
    src/accel/hwaccel.c
    # --- resiliencia: KARES, USS, WAL, Job Queue, TuxAtlas ---
    src/resilience/kares/kares.c
    src/resilience/kares/kares_recovery.c
    src/resilience/kares/kares_checkpoint.c
    src/resilience/kares/kares_ecc.c
    src/resilience/kares/kares_compress.c
    src/resilience/uss/uss.c
    src/resilience/uss/uss_ecc.c
    src/resilience/wal/wal.c
    src/resilience/job_queue/job_queue.c
    src/resilience/tuxatlas/tuxatlas.c
    # --- transporte (enlace físico) y sincronización de fase ---
    src/transport/transport.c
    src/transport/transport_usb.c
    src/transport/transport_uart.c
    src/transport/transport_can.c
    src/transport/transport_spi.c
    src/transport/transport_i2c.c
    src/transport/transport_ethertux.c
    src/transport/transport_websocket.c
    src/transport/phsync.c
    # --- protocolos de red (selección, EtherCAT, EtherTux) ---
    src/protocol/protocol.c
    src/protocol/ethercat/transport_ethercat.c
    src/protocol/ethertux/et_frame.c
    src/protocol/ethertux/et_fec.c
    src/protocol/ethertux/et_qos.c
    src/protocol/ethertux/et_sched.c
    src/protocol/ethertux/et_esm.c
    src/protocol/ethertux/et_sync.c
    src/protocol/ethertux/et_topology.c
    src/protocol/ethertux/et_mbx.c
    src/protocol/ethertux/et_flowagg.c
    src/protocol/ethertux/et_frm.c
    src/protocol/ethertux/et_aalid.c
    src/protocol/ethertux/et_socket.c
    src/protocol/ethertux/et_master.c
    src/protocol/ethertux/et_slave.c
    src/protocol/ethertux/et_slave_bridge.c
    # --- IPC: canal SPSC, shm zero-copy y framing ---
    src/ipc/channel.c
    src/ipc/zckb_shm.c
    src/ipc/msgblock.c
)

# Public headers
set(CORE_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include/tuxrt_sdk.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include/tuxrt_provider.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include/tuxrt_ethertux.h
)

# Enlace universal de proveedores (TuxRT Provider ABI). El módulo es reutilizable
# por cualquier proyecto del ecosistema que quiera publicar o consumir un
# proveedor (ChronosRT exporta su descriptor; el core lo descubre con dlopen).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TuxRTProvider.cmake)
tuxrt_require_provider(ChronosRT SONAME libchronosrt.so VERSION 0.1.0)

# Shared library
add_library(tuxrt_core SHARED ${CORE_SOURCES})

target_compile_definitions(tuxrt_core PRIVATE
    TUXRT_GIT_COMMIT="${TUXRT_GIT_COMMIT}"
    TUXRT_MOJO_VERSION="${TUXRT_MOJO_VERSION}")

# NOTE: los subdirectorios por sistema se declaran ANTES que `include/` para que
# los headers canónicos (include/<sistema>/<header>.h) tengan prioridad sobre
# las copias planas heredadas de la reestructuración (include/<header>.h).
target_include_directories(tuxrt_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/motion>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/compression>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/integrity>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/accel>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ipc>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/kares>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/uss>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/wal>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/job_queue>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/tuxatlas>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/tuxrt>
)
# Headers internos de implementación que viven junto a su fuente.
target_include_directories(tuxrt_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/accel
)

target_link_libraries(tuxrt_core PUBLIC
    rt pthread m dl
    ${LIBURING_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
)
set_target_properties(tuxrt_core PROPERTIES
    VERSION ${TUXRT_CORE_VERSION}
    SOVERSION ${TUXRT_CORE_VERSION_MAJOR}
    C_VISIBILITY_PRESET default
    POSITION_INDEPENDENT_CODE ON
)

# Apply config-specific flags using add_compile_options to avoid shell interpretation issues
target_compile_options(tuxrt_core PRIVATE $<$<CONFIG:DEBUG>:${DEBUG_FLAGS}> $<$<CONFIG:RELEASE>:${RELEASE_FLAGS}>)

# Install
install(TARGETS tuxrt_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CORE_HEADERS}
    DESTINATION include/tuxrt
)

# Módulo CMake reutilizable del contrato de proveedor (usable por cualquier
# proyecto que quiera publicar/consumir un proveedor del ecosistema).
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TuxRTProvider.cmake
    DESTINATION share/tuxrt/cmake
)

# Unit tests
enable_testing()
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    # --- Standalone tests (no external framework required) ---
    # These use a self-contained ASSERT harness and link only against tuxrt_core.
    # Scope: the modules declared in libtuxrt_core/CMakeLists.txt (F0-03).
    set(TUXRT_STANDALONE_TESTS
        scheduler
        motion
        wal
        transport
        phsync
        aalid
    )

    foreach(_t ${TUXRT_STANDALONE_TESTS})
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_${_t}.c)
            add_executable(test_${_t} test/test_${_t}.c)
            target_link_libraries(test_${_t} PRIVATE tuxrt_core)
            add_test(NAME ${_t} COMMAND test_${_t})
        endif()
    endforeach()

    # --- ultracrc: requires the internal (hidden) protocol CRC symbols, so the
    #     source is compiled directly into the test executable (F1a-01). hwdet.c
    #     is pulled in because ultracrc's dispatch depends on it (both are hidden
    #     in the shared library). ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ultracrc.c)
        add_executable(test_ultracrc test/test_ultracrc.c src/integrity/ultracrc.c src/accel/hwdet.c)
        target_link_libraries(test_ultracrc PRIVATE tuxrt_core)
        add_test(NAME ultracrc COMMAND test_ultracrc)
    endif()

    # --- gcode_parser: parser SIMD + hooks de accel_runtime (F1a-02). El
    #     runtime acelerado y la detección de HW se compilan directamente para
    #     ejercitar los símbolos ocultos en el .so. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_gcode_parser.c)
        add_executable(test_gcode_parser
            test/test_gcode_parser.c src/parser/gcode_parser.c src/accel/hwdet.c src/accel/accel_runtime.c)
        target_include_directories(test_gcode_parser PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/accel)
        target_link_libraries(test_gcode_parser PRIVATE tuxrt_core m)
        add_test(NAME gcode_parser COMMAND test_gcode_parser)
    endif()

    # --- channel: canal SPSC wait-free (F1b-01). El canal usa símbolos
    #     internos (ocultos en el .so), por lo que se compila directamente. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_channel.c)
        add_executable(test_channel test/test_channel.c src/ipc/channel.c)
        target_link_libraries(test_channel PRIVATE tuxrt_core pthread)
        add_test(NAME channel COMMAND test_channel)
    endif()

    # --- zckb_shm: memoria compartida zero-copy (F1b-02). Compila el canal y
    #     la shm directamente para ejercitar los símbolos internos. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_zckb_shm.c)
        add_executable(test_zckb_shm
            test/test_zckb_shm.c src/ipc/zckb_shm.c src/ipc/channel.c)
        target_link_libraries(test_zckb_shm PRIVATE tuxrt_core rt pthread)
        add_test(NAME zckb_shm COMMAND test_zckb_shm)
    endif()

    # --- msgblock: framing V2 + fragmentación (F1b-03). El CRC usa los
    #     símbolos internos de ultracrc, que a su vez depende de hwdet. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_msgblock.c)
        add_executable(test_msgblock
            test/test_msgblock.c src/ipc/msgblock.c src/integrity/ultracrc.c src/accel/hwdet.c)
        target_link_libraries(test_msgblock PRIVATE tuxrt_core pthread)
        add_test(NAME msgblock COMMAND test_msgblock)
    endif()

    # --- trapq: cola de trapezoides + junction deviation (F1c-02). Los símbolos
    #     de trapq están ocultos en el .so, por lo que se compila directamente. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_trapq.c)
        add_executable(test_trapq test/test_trapq.c src/motion/trapq.c)
        target_link_libraries(test_trapq PRIVATE tuxrt_core m)
        add_test(NAME trapq COMMAND test_trapq)
    endif()

    # --- stepcompress: DDA + min-heap (F1c-03). Depende de trapq para el perfil. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_stepcompress.c)
        add_executable(test_stepcompress
            test/test_stepcompress.c src/motion/stepcompress.c src/motion/trapq.c)
        target_link_libraries(test_stepcompress PRIVATE tuxrt_core m)
        add_test(NAME stepcompress COMMAND test_stepcompress)
    endif()

    # --- steppersync: k-way merge temporal multi-eje (F1c-04). ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_steppersync.c)
        add_executable(test_steppersync test/test_steppersync.c src/motion/steppersync.c)
        target_link_libraries(test_steppersync PRIVATE tuxrt_core m)
        add_test(NAME steppersync COMMAND test_steppersync)
    endif()

    # --- input_shaper: ZV/EI/ZVD/MZV (F1c-05). ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_input_shaper.c)
        add_executable(test_input_shaper test/test_input_shaper.c src/motion/input_shaper.c)
        target_link_libraries(test_input_shaper PRIVATE tuxrt_core m)
        add_test(NAME input_shaper COMMAND test_input_shaper)
    endif()

    # --- ksc: compresor adaptativo KSC (F1d-01). Símbolos ocultos, compila directo. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ksc.c)
        add_executable(test_ksc test/test_ksc.c src/compression/ksc.c)
        target_link_libraries(test_ksc PRIVATE tuxrt_core)
        add_test(NAME ksc COMMAND test_ksc)
    endif()

    # --- jlpp: compresor telemetría JLPP DPCM+Golomb-Rice (F1d-02). Símbolos ocultos, compila directo. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_jlpp.c)
        add_executable(test_jlpp test/test_jlpp.c src/compression/jlpp.c)
        target_link_libraries(test_jlpp PRIVATE tuxrt_core)
        add_test(NAME jlpp COMMAND test_jlpp)
    endif()

    # --- kares: KARES 7 fases + checkpoint + ECC BCH + compresión adaptativa (F1c-01) ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_kares.c)
        add_executable(test_kares
            test/test_kares.c
            src/resilience/kares/kares_recovery.c
            src/resilience/kares/kares_checkpoint.c
            src/resilience/kares/kares_ecc.c
            src/resilience/kares/kares_compress.c
            src/resilience/uss/uss.c
            src/resilience/uss/uss_ecc.c
            src/accel/hwdet.c)
        target_link_libraries(test_kares PRIVATE tuxrt_core m)
        add_test(NAME kares COMMAND test_kares)
    endif()

    # --- kares_perf: tests de rendimiento de compresión y recuperación (opcional) ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_kares_perf.c)
        add_executable(test_kares_perf
            test/test_kares_perf.c
            src/resilience/kares/kares_recovery.c
            src/resilience/kares/kares_checkpoint.c
            src/resilience/kares/kares_ecc.c
            src/resilience/kares/kares_compress.c
            src/resilience/uss/uss.c
            src/resilience/uss/uss_ecc.c
            src/accel/hwdet.c)
        target_link_libraries(test_kares_perf PRIVATE tuxrt_core m)
        add_test(NAME kares_perf COMMAND test_kares_perf)
    endif()

    # --- EtherTux: protocolo determinista sobre Ethernet comercial (0x88B5).
    #     Usa únicamente la API pública, por lo que enlaza contra el .so. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ethertux.c)
        add_executable(test_ethertux test/test_ethertux.c)
        target_link_libraries(test_ethertux PRIVATE tuxrt_core)
        add_test(NAME ethertux COMMAND test_ethertux)
    endif()

    # --- EtherTux v2.0: ET-ESM, ET-Sync, topología/ET-WKC, ET-MBX, Flow
    #     Aggregator y FRM. Rutas puras de CPU, sin hardware de red. ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_ethertux_v2.c)
        add_executable(test_ethertux_v2 test/test_ethertux_v2.c)
        target_link_libraries(test_ethertux_v2 PRIVATE tuxrt_core)
        add_test(NAME ethertux_v2 COMMAND test_ethertux_v2)
    endif()

    # --- HAL de aceleración: CRC-32C, GF(2^8) y Reed-Solomon (FEC). ---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_hwaccel.c)
        add_executable(test_hwaccel test/test_hwaccel.c)
        target_link_libraries(test_hwaccel PRIVATE tuxrt_core)
        add_test(NAME hwaccel COMMAND test_hwaccel)
    endif()

    # --- Criterion-based tests (optional; require the `criterion` package) ---
    find_package(Criterion QUIET)
    if(Criterion_FOUND)
        set(TUXRT_CRITERION_TESTS sirc)
        foreach(_t ${TUXRT_CRITERION_TESTS})
            if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_${_t}.c)
                add_executable(test_${_t} test/test_${_t}.c)
                target_link_libraries(test_${_t} PRIVATE tuxrt_core criterion)
                add_test(NAME ${_t} COMMAND test_${_t})
            endif()
        endforeach()
    else()
        message(STATUS "Criterion not found — skipping criterion-based tests. "
                       "Install 'criterion' (e.g. sudo pacman -S criterion) to enable them.")
    endif()
endif()

# Test runner
add_custom_target(test_all COMMAND ctest --output-on-failure)

# Sanitizer builds
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # Subdirectorios por sistema (kares/uss/wal/job_queue/tuxatlas)
    set(TUXRT_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/core
        ${CMAKE_CURRENT_SOURCE_DIR}/include/motion
        ${CMAKE_CURRENT_SOURCE_DIR}/include/compression
        ${CMAKE_CURRENT_SOURCE_DIR}/include/integrity
        ${CMAKE_CURRENT_SOURCE_DIR}/include/accel
        ${CMAKE_CURRENT_SOURCE_DIR}/include/ipc
        ${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/kares
        ${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/uss
        ${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/wal
        ${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/job_queue
        ${CMAKE_CURRENT_SOURCE_DIR}/include/resilience/tuxatlas
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/accel)

    # ASan + UBSan
    add_library(tuxrt_core_asan SHARED ${CORE_SOURCES})
    target_include_directories(tuxrt_core_asan PRIVATE ${TUXRT_INCLUDE_DIRS})
    target_compile_options(tuxrt_core_asan PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    target_link_libraries(tuxrt_core_asan PUBLIC rt pthread m dl ${LIBURING_LIBRARIES} ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES} ${LIBCURL_LIBRARIES} ${JSONC_LIBRARIES})
    set_target_properties(tuxrt_core_asan PROPERTIES OUTPUT_NAME tuxrt_core_asan)

    # TSAN
    add_library(tuxrt_core_tsan SHARED ${CORE_SOURCES})
    target_include_directories(tuxrt_core_tsan PRIVATE ${TUXRT_INCLUDE_DIRS})
    target_compile_options(tuxrt_core_tsan PRIVATE -fsanitize=thread -O1 -g)
    target_link_libraries(tuxrt_core_tsan PUBLIC rt pthread m dl ${LIBURING_LIBRARIES} ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES} ${LIBCURL_LIBRARIES} ${JSONC_LIBRARIES})
    set_target_properties(tuxrt_core_tsan PROPERTIES OUTPUT_NAME tuxrt_core_tsan)
endif()

# Format target
add_custom_target(format_core
    COMMAND clang-format -i ${CORE_SOURCES} ${CORE_HEADERS}
    COMMENT "Formatting core sources"
)

# Static analysis
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    add_custom_target(tidy_core
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${CORE_SOURCES} -- -I${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
        COMMENT "Running clang-tidy on core"
    )
endif()