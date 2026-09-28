# chiptool: command-line utility for the preset generator (tools/gen_presets.py).
#   chiptool dump-params <nes|snes|genesis>          parameter table as JSON
#   chiptool render <preset.json> <out.wav> [...]    render one preset to a WAV file
#   chiptool features <bank.json> <features.json>    log-mel / envelope features per preset
# Links the whole chipdsp library; no JUCE.

find_package(Threads REQUIRED)

add_executable(chiptool chiptool.cpp MiniJson.h)
target_link_libraries(chiptool PRIVATE chipdsp::chipdsp Threads::Threads rcv_warnings)
