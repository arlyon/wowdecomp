#include "ChecksumExecutables.h"
#include <cstring>
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "WoW/Source/WowServices/HMAC.h"
#if !defined(_WIN32)
#include "Engine/Source/Storm/H/storm.h"
#endif
#line 6

#if defined(_WIN32)
#line 6
struct ChecksumFile {
    /* 0000 */ const char* m_path;
    /* 0004 */ int32_t m_type;
};

// STATIC: 0x00AD96E8
static ChecksumFile s_files[3] = { { nullptr, 2 }, { "DivxDecoder.dll", 2 }, { "unicows.dll", 1 } };
#endif
#line 15
// VERIFY: b726580e manual
// OFFSET: 0x006B9CF0
#if !defined(_WIN32)
void ChecksumExecutables(const uint8_t* challenge, uint32_t challengeLength, uint8_t* checksum) {
    // AzerothCore 12340/Win compatibility, not native executable attestation.
    // Reference: azerothcore-wotlk aae8642f, AuthSession.cpp and build_info.sql.
    static constexpr uint8_t acChallenge[16] = {
        0xBA, 0xA3, 0x1E, 0x99, 0xA0, 0x0B, 0x21, 0x57,
        0xFC, 0x37, 0x3F, 0xB3, 0x69, 0xCD, 0xD2, 0xF1
    };
    static constexpr uint8_t win12340Fingerprint[20] = {
        0xCD, 0xCB, 0xBD, 0x51, 0x88, 0x31, 0x5E, 0x6B,
        0x4D, 0x19, 0x44, 0x9D, 0x49, 0x2D, 0xBC, 0xFA,
        0xF1, 0x56, 0xA3, 0x47
    };
    if (!challenge || !checksum || challengeLength != sizeof(acChallenge) ||
        std::memcmp(challenge, acChallenge, sizeof(acChallenge))) {
        SErrDisplayAppFatalMessage("Unsupported native executable authentication challenge", __FILE__, __LINE__);
        return;
    }
    std::memcpy(checksum, win12340Fingerprint, sizeof(win12340Fingerprint));
}
#else
#line 17
void ChecksumExecutables(const uint8_t* challenge, uint32_t challengeLength, uint8_t* checksum) {
    HMAC<SHA1> hmac;
    hmac.Init(challenge, challengeLength);

    for (uint32_t i = 0; i < sizeof(s_files) / sizeof(s_files[0]); i++) {
        if (s_files[i].m_type) {
            IOsVersionHashCallback2<HMAC<SHA1>> callback(&hmac);
            IOsVersionChecksum(callback, s_files[i].m_path, s_files[i].m_type == 2);
        }
    }

    SHA1::Digest digest;
    hmac.Finish(digest);
    memcpy(checksum, &digest, sizeof(digest));
}
#endif
