#include "stdafx.h"

#include <GWCA/Utilities/Scanner.h>

#include <Utils/StoCHandlers.h>

StoCHandlerArray* GetStoCHandlerArray()
{
    uintptr_t address = GW::Scanner::Find("\x75\x04\x33\xC0\x5D\xC3\x8B\x41\x08\xA8\x01\x75", "xxxxxxxxxxxx", -6);
    DEBUG_ASSERT(address);
    if (!address) {
        return nullptr;
    }
    const uintptr_t StoCHandler_Addr = *(uintptr_t*)address;

    struct GameServer {
        uint8_t h0000[8];

        struct {
            uint8_t h0000[12]{};

            struct {
                uint8_t h0000[12]{};
                void* next{};
                uint8_t h0010[12]{};
                uint32_t ClientCodecArray[4]{};
                StoCHandlerArray handlers;
            }* ls_codec{};

            uint8_t h0010[12]{};
            uint32_t ClientCodecArray[4]{};
            StoCHandlerArray handlers;
        }* gs_codec;
    };

    const auto addr = (GameServer**)StoCHandler_Addr;
    if (!(addr && *addr && (*addr)->gs_codec)) {
        return nullptr;
    }
    return &(*addr)->gs_codec->handlers;
}
