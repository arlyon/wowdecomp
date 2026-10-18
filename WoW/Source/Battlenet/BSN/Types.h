#ifndef WOW_SOURCE_BATTLENET_BSN_TYPES_H
#define WOW_SOURCE_BATTLENET_BSN_TYPES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Storm/H/storm.h"

namespace BSN {
enum MetaType {
    META_ARRAY = 1,
    META_ASCII_STRING = 2,
    META_BIT_ARRAY = 3,
    META_BLOB = 4,
    META_BOOL = 5,
    META_CHOICE = 6,
    META_ENUM = 7,
    META_FOURCC = 8,
    META_INT = 9,
    META_NULL = 10,
    META_OPTIONAL = 11,
    META_REAL32 = 12,
    META_REAL64 = 13,
    META_STRUCT = 14,
    META_STRING = 15,
    META_USER = 16,
};

enum WalkResult {
    WALK_VALID = 0,
    WALK_INVALID = 1,
    WALK_UNKNOWN_TYPE = 2,
};

enum CodecResult {
    CODEC_VALID = 0,
    CODEC_INVALID = 1,
    CODEC_OVERFLOW = 2,
};

typedef uint32_t (*AdapterGetLength)(const void* data);
typedef const void* (*AdapterGetElements)(const void* data);
typedef void* (*AdapterSetLength)(void* data, uint32_t length);
typedef void (*AdapterSetCount)(void* data, uint32_t count);
typedef bool (*AdapterGetExists)(const void* data);
typedef void* (*AdapterSetExists)(void* data, bool exists);
typedef int32_t (*AdapterGetTag)(const void* data);
#if defined(_WIN32)
#line 49
typedef void* (*AdapterGetChoice)(const void* data);
typedef void (*AdapterSetTag)(void* data, int32_t tag);
#else
// Match the generated Choice::GetElement/SetTag producers on native builds.
typedef const void* (*AdapterGetChoice)(const void* data);
typedef void* (*AdapterSetTag)(void* data, int32_t tag);
#endif
#line 51
typedef uint32_t (*AdapterGetFourCC)(const void* data);
typedef void (*AdapterSetFourCC)(void* data, uint32_t value);

struct Utf8Sequence {
    /* 0000 */ uint32_t m_length;
    /* 0004 */ uint8_t m_ranges[4][2];
};

uint32_t AsciiStringCopy(char* dest, const char* source, uint32_t maxLength);
uint32_t UTF8StringCopy(char* dest, const char* source, uint32_t maxChars, uint32_t& length);

template <uint32_t N>
class AsciiString {
    public:
    /* 0000 */ uint32_t m_length;
    /* 0004 */ char m_data[N + 1];

    AsciiString();
    explicit AsciiString(const char* source);
    AsciiString<N>& operator=(const char* source);
};

template <uint32_t N>
class String {
    public:
    /* 0000 */ uint32_t m_size;
    /* 0004 */ uint32_t m_length;
    /* 0008 */ char m_data[4 * N + 1];

    String();
    explicit String(const char* source);
    String<N>& operator=(const char* source);
    String<N>& operator+=(const char* source);
};

template <class T, uint32_t N>
class Array {
    public:
    /* 0000 */ uint32_t m_count;
    /* 0004 */ T m_data[N];

    Array();
    T& operator[](uint32_t index);
    const T& operator[](uint32_t index) const;
    T* New();
    void Remove(uint32_t index);
    void SetCount(uint32_t count);
};

template <class T, uint32_t SIZE>
class FixedArray {
    public:
    /* 0000 */ T m_data[SIZE];

    const T& operator[](uint32_t index) const;
};

template <uint32_t N>
class Blob {
    public:
    /* 0000 */ uint32_t m_length;
    /* 0004 */ uint8_t m_data[N];
};

template <uint32_t N>
class FixedBlob {
    public:
    /* 0000 */ uint8_t m_data[N];
};

template <uint32_t N>
class FixedBitArray {
    public:
    /* 0000 */ uint8_t m_data[(N + 7) / 8];
};

template <class T>
class Optional {
    public:
    /* 0000 */ bool m_exists;
    /* 0004 */ T m_value;

    Optional();
    Optional(const Optional<T>& other);
    Optional<T>& operator=(const Optional<T>& other);
    const T& Get() const;
};

class Choice {
    public:
    /* 0000 */ int32_t m_tag;

    template <int32_t TAG, class T>
    T& Get();
    template <int32_t TAG, class T>
    const T& Get() const;
    template <int32_t TAG, class T>
    T* Set();
};
}

// INLINED
template <uint32_t N>
BSN::AsciiString<N>::AsciiString() {
    this->m_length = 0;
    this->m_data[0] = '\0';
}

// INLINED
template <uint32_t N>
BSN::AsciiString<N>::AsciiString(const char* source) {
    this->m_length = BSN::AsciiStringCopy(this->m_data, source, N);
}

// VERIFY: b5f69c14 auto
// OFFSET: [0x008C63C0<255>, 0x008C63F0<320>, 0x008C6480<31>]
template <uint32_t N>
BSN::AsciiString<N>& BSN::AsciiString<N>::operator=(const char* source) {
    this->m_length = BSN::AsciiStringCopy(this->m_data, source, N);
    return *this;
}

// INLINED
template <uint32_t N>
BSN::String<N>::String() {
    this->m_size = 0;
    this->m_length = 0;
    this->m_data[0] = '\0';
}

// INLINED
template <uint32_t N>
BSN::String<N>::String(const char* source) {
    this->m_size = BSN::UTF8StringCopy(this->m_data, source, N, this->m_length);
}

// VERIFY: 497fac72 auto
// OFFSET: 0x00530FB0<127>
template <uint32_t N>
BSN::String<N>& BSN::String<N>::operator=(const char* source) {
    this->m_size = BSN::UTF8StringCopy(this->m_data, source, N, this->m_length);
    return *this;
}

// INLINED
template <uint32_t N>
BSN::String<N>& BSN::String<N>::operator+=(const char* source) {
    uint32_t length;
    this->m_size += BSN::UTF8StringCopy(&this->m_data[this->m_size], source, N - this->m_length, length);
    this->m_length += length;
    return *this;
}

// VERIFY: c98431e8 manual
// OFFSET: [0x008C6B90<Battlenet::Authentication::ModuleParam, 4>, 0x00537B30<BattlenetUIFriend, 100>]
template <class T, uint32_t N>
BSN::Array<T, N>::Array() {
    this->m_count = 0;
}

// VERIFY: 902008f6 auto
// OFFSET: [0x008C6390<Battlenet::IP4::Address, 127>, 0x00531000<Battlenet::Friends::FriendInvite, 100>, 0x005310C0<BattlenetUIFriend, 100>]
template <class T, uint32_t N>
T& BSN::Array<T, N>::operator[](uint32_t index) {
    if (index >= this->m_count) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return this->m_data[index];
}

// VERIFY: ae8dfdec auto
// OFFSET: [0x008C6420<Battlenet::Authentication::ModuleParam, 4>, 0x008C6450<Battlenet::Realm::CharacterCount, 64>]
template <class T, uint32_t N>
const T& BSN::Array<T, N>::operator[](uint32_t index) const {
    if (index >= this->m_count) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return this->m_data[index];
}

// VERIFY: 566cee18 auto
// OFFSET: 0x00531030<Battlenet::Friends::FriendInvite, 100>
template <class T, uint32_t N>
T* BSN::Array<T, N>::New() {
    if (this->m_count >= N) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return &this->m_data[this->m_count++];
}

// INLINED
template <class T, uint32_t SIZE>
const T& BSN::FixedArray<T, SIZE>::operator[](uint32_t index) const {
    if (index >= SIZE) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return this->m_data[index];
}

// INLINED
template <class T>
BSN::Optional<T>::Optional() : m_exists(false), m_value() {
}

// INLINED
template <class T>
BSN::Optional<T>::Optional(const Optional<T>& other) : m_exists(other.m_exists), m_value() {
    if (this->m_exists) {
        this->m_value = other.m_value;
    }
}

// INLINED
template <class T>
BSN::Optional<T>& BSN::Optional<T>::operator=(const Optional<T>& other) {
    this->m_exists = other.m_exists;

    if (this->m_exists) {
        this->m_value = other.m_value;
    }

    return *this;
}

// VERIFY: 49436f17 auto
// OFFSET: 0x00530FE0<BSN::String<127>>
template <class T>
const T& BSN::Optional<T>::Get() const {
    if (!this->m_exists) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return this->m_value;
}

// INLINED
template <int32_t TAG, class T>
T& BSN::Choice::Get() {
    if (this->m_tag != TAG) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return *reinterpret_cast<T*>(this + 1);
}

// VERIFY: dec6a3bb auto
// OFFSET: [0x00531060<Battlenet::Friends::FriendInvite, 100>, 0x005310F0<BattlenetUIFriend, 100>]
template <class T, uint32_t N>
void BSN::Array<T, N>::Remove(uint32_t index) {
    if (index >= this->m_count) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    if (this->m_count - index > 1) {
        memmove(&this->m_data[index], &this->m_data[index + 1], (this->m_count - index - 1) * sizeof(T));
    }

    this->m_count--;
}

// INLINED
template <class T, uint32_t N>
void BSN::Array<T, N>::SetCount(uint32_t count) {
    if (count > N) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    for (uint32_t i = this->m_count; i < count; i++) {
        this->m_data[i] = T();
    }

    this->m_count = count;
}

// VERIFY: df702c9e auto
// OFFSET: [0x008C66E0<2, Battlenet::Realm::JoinResponse>, 0x008C6700<3, uint32_t>, 0x008C6720<0, Battlenet::Authentication::ModuleRequest>, 0x008C6740<1, uint32_t>, 0x008C6760<9, Battlenet::Realm::Response>, 0x00531180<6, int32_t>, 0x005311A0<17, Battlenet::Toon::FullName>, 0x005311C0<18, Battlenet::Presence::AccountName>]
template <int32_t TAG, class T>
const T& BSN::Choice::Get() const {
    if (this->m_tag != TAG) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    return *reinterpret_cast<const T*>(this + 1);
}

// VERIFY: 31bcc217 manual
// OFFSET: [0x008C7000<0, Battlenet::Authentication::Request>, 0x008C6FD0<2, Battlenet::Authentication::ModuleMessage>, 0x008C6C90<1, BSN::AsciiString<320>>, 0x008C6CB0<0, BSN::AsciiString<31>>, 0x008C6CD0<12, Battlenet::Event::Creep::Portal>]
template <int32_t TAG, class T>
T* BSN::Choice::Set() {
    this->m_tag = TAG;
    return new (this + 1) T();
}

#endif
