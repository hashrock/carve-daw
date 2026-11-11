#pragma once

#include <cstddef>
#include <cstring>

#include <CarveInstrumentData.h>

namespace carve::plugins
{

// The analysed instruments' data, compiled into the binary (see
// assets/instruments and juce_add_binary_data in CMakeLists.txt) rather than
// shipped as content: an instrument that cannot find its sound is no
// instrument, and the renderer, the tests and the app all have to find it.
struct InstrumentData
{
    const void* data = nullptr;
    std::size_t size = 0;
};

// By its file name under assets/instruments. Empty if it is not there.
inline InstrumentData findInstrumentData (const char* fileName)
{
    for (int i = 0; i < CarveInstrumentData::namedResourceListSize; ++i)
        if (std::strcmp (CarveInstrumentData::originalFilenames[i], fileName) == 0)
        {
            int size = 0;
            const auto* bytes = CarveInstrumentData::getNamedResource (CarveInstrumentData::namedResourceList[i], size);
            return { bytes, (std::size_t) size };
        }

    return {};
}

} // namespace carve::plugins
