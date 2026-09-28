#pragma once

#include "chipdsp/ChipTypes.h"
#include "chipdsp/IChipEngine.h"

#include <memory>

namespace chipdsp
{

// Creates the engine for a chip. The plugin layer only ever sees IChipEngine.
// Message thread only (allocates).
std::unique_ptr<IChipEngine> createEngine(ChipId chip);

// Display names used by the plugin and the preset tools.
const char* chipName(ChipId chip) noexcept;      // "NES", "SNES", "Genesis"
const char* chipKey(ChipId chip) noexcept;       // "nes", "snes", "genesis"
int chipChannelCount(ChipId chip) noexcept;

} // namespace chipdsp
