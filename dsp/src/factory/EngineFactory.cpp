#include "chipdsp/EngineFactory.h"

#include "chipdsp/factory/StubEngine.h"

// RCV_HAVE_<CHIP>_ENGINE is defined by dsp/CMakeLists.txt once that engine's sources exist.
#if defined(RCV_HAVE_NES_ENGINE)
    #include "chipdsp/nes/Nes2A03Engine.h"
#endif
#if defined(RCV_HAVE_SNES_ENGINE)
    #include "chipdsp/snes/SnesDspEngine.h"
#endif
#if defined(RCV_HAVE_GENESIS_ENGINE)
    #include "chipdsp/genesis/GenesisEngine.h"
#endif

namespace chipdsp
{

std::unique_ptr<IChipEngine> createEngine(ChipId chip)
{
    switch (chip)
    {
        case ChipId::Nes:
#if defined(RCV_HAVE_NES_ENGINE)
            return std::make_unique<Nes2A03Engine>();
#else
            return std::make_unique<StubEngine>(chip);
#endif
        case ChipId::Snes:
#if defined(RCV_HAVE_SNES_ENGINE)
            return std::make_unique<SnesDspEngine>();
#else
            return std::make_unique<StubEngine>(chip);
#endif
        case ChipId::Genesis:
#if defined(RCV_HAVE_GENESIS_ENGINE)
            return std::make_unique<GenesisEngine>();
#else
            return std::make_unique<StubEngine>(chip);
#endif
    }
    return std::make_unique<StubEngine>(chip);
}

const char* chipName(ChipId chip) noexcept
{
    switch (chip)
    {
        case ChipId::Nes:     return "NES";
        case ChipId::Snes:    return "SNES";
        case ChipId::Genesis: return "Genesis";
    }
    return "Unknown";
}

const char* chipKey(ChipId chip) noexcept
{
    switch (chip)
    {
        case ChipId::Nes:     return "nes";
        case ChipId::Snes:    return "snes";
        case ChipId::Genesis: return "genesis";
    }
    return "unknown";
}

int chipChannelCount(ChipId chip) noexcept
{
    switch (chip)
    {
        case ChipId::Nes:     return kNesChannels;
        case ChipId::Snes:    return kSnesChannels;
        case ChipId::Genesis: return kGenesisChannels;
    }
    return 0;
}

} // namespace chipdsp
