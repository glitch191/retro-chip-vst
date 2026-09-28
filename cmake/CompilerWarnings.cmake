# Interface target carrying the project's warning configuration.
# Link it PRIVATE from every first-party target.

add_library(rcv_warnings INTERFACE)

if(MSVC)
    target_compile_options(rcv_warnings INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /wd4324   # structure was padded due to alignment specifier
    )
    if(RCV_WARNINGS_AS_ERRORS)
        target_compile_options(rcv_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(rcv_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    )
    if(RCV_WARNINGS_AS_ERRORS)
        target_compile_options(rcv_warnings INTERFACE -Werror)
    endif()
endif()

# Fast, deterministic floating point: no fast-math (bit-exact tables matter).
if(MSVC)
    target_compile_options(rcv_warnings INTERFACE /fp:precise)
endif()
