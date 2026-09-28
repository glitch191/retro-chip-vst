# CPU-cost benchmark for each engine (512-sample blocks at 44.1 kHz and 48 kHz,
# all channels active). Results are copied into README.md by hand.

file(GLOB CHIPDSP_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(CHIPDSP_BENCH_SOURCES)
    add_executable(chipdsp_bench ${CHIPDSP_BENCH_SOURCES})
    target_link_libraries(chipdsp_bench PRIVATE chipdsp::chipdsp rcv_warnings)
endif()
