#pragma once

#include <GWCA/GameContainers/Array.h>

#include <cstdint>

namespace GW::Packet::StoC {
    struct PacketBase;
}

// A field descriptor's low four bits: 8, 9 and 10 are coded as 4, 5 and 6, and the client keeps them apart.
enum class StoCFieldType : uint32_t {
    Id = 0,
    Float = 1,
    Vect2 = 2,
    Vect3 = 3,
    Int = 4,
    Blob = 5,
    ArrayData = 6,
    String16 = 7,
    IntB = 8,
    BlobB = 9,
    ArrayDataB = 10,
    ArrayCount = 11,
    NestedStruct = 12,
};

using StoCHandler_pt = bool(__fastcall*)(GW::Packet::StoC::PacketBase* pak);

struct StoCHandler {
    uint32_t* fields;
    uint32_t field_count;
    StoCHandler_pt handler_func;
};

using StoCHandlerArray = GW::Array<StoCHandler>;

StoCHandlerArray* GetStoCHandlerArray();
