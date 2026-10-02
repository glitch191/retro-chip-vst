# Post-build step of the VST3 target (RCV_COPY_PLUGIN_AFTER_BUILD, plugin/CMakeLists.txt):
# copies the built .vst3 bundle over the installed one in DEST_DIR. A failed copy (no write
# access to the folder, or a host that has the plugin loaded) is a warning, not a build
# error: the build output stays usable and build.ps1 -Install can copy it with elevation.
#
#   cmake -DSRC=<built bundle> -DDEST_DIR=<VST3 folder> -P CopyVst3Bundle.cmake

get_filename_component(name "${SRC}" NAME)
set(dest "${DEST_DIR}/${name}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SRC}" "${dest}"
                RESULT_VARIABLE result
                ERROR_VARIABLE error)
if(result EQUAL 0)
    message(STATUS "Installed ${dest}")
else()
    message(WARNING "Could not copy the plugin to ${dest}: ${error}"
                    "Close the host if it has the plugin loaded, give your account write "
                    "access to that folder, or run build.ps1 -Install (elevated copy).")
endif()
