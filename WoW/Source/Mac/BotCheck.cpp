#include "WoW/Source/Client.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/AccountData.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void UnsupportedBotCheck(const char* operation) {
    // The Windows scanner/module implementation is incomplete too. Never
    // translate its placeholders into synthetic clean/detected native results.
    std::fprintf(stderr, "Wow: unsupported native security operation: %s\n", operation);
    std::abort();
}
}

void AdditionalCheck1() {
    if (!s_botCheckEnabled || s_botCheckDelay <= 0 || s_botCheckDelay > 420) {
        return;
    }
    long expected = 1;
    if (!__atomic_compare_exchange_n(&s_additionalCheckArmed, &expected, 0, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }
    UnsupportedBotCheck("AdditionalCheck1");
}

void InitTokenSeed(const uint8_t* key) {
    UnsupportedBotCheck("InitTokenSeed");
}

int32_t Packet_SMSG_CHECK_FOR_BOTS(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    UnsupportedBotCheck("Packet_SMSG_CHECK_FOR_BOTS");
}

int32_t GatherAdditionalChecks(uint8_t* check1, uint8_t* check2, uint8_t* check3) {
    UnsupportedBotCheck("GatherAdditionalChecks");
}

uint8_t AdditionalCheck1Scan(const AdditionalCheckInterface* iface, int32_t a2) {
    UnsupportedBotCheck("AdditionalCheck1Scan");
}

void LoadEncryptedModule(const uint8_t* key, uint32_t length, void (*callback)()) {
    UnsupportedBotCheck("LoadEncryptedModule");
}
