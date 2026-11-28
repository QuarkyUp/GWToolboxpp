#pragma once

#include <GWCA/GameContainers/Array.h>

#include <cstdint>

namespace GW::Packet::StoC {
    struct PacketBase;
}

using StoCHandler_pt = bool(__fastcall*)(GW::Packet::StoC::PacketBase* pak);

struct StoCHandler {
    uint32_t* fields;
    uint32_t field_count;
    StoCHandler_pt handler_func;
};

using StoCHandlerArray = GW::Array<StoCHandler>;

StoCHandlerArray* GetStoCHandlerArray();
