#include "WoW/Source/Battlenet/BSN/Adapter.h"
#include "WoW/Source/Battlenet/BSN/BSNEncodeInterface.h"
#include "WoW/Source/Battlenet/BattlenetProtocolTypes.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <type_traits>

static_assert(std::is_same<BSN::AdapterGetChoice, decltype(&BSN::Adapter::Choice::GetElement)>::value, "choice getter signature");
static_assert(std::is_same<BSN::AdapterSetTag, decltype(&BSN::Adapter::Choice::SetTag)>::value, "choice setter signature");

namespace {
#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

BSN::BSNPrimitiveEncoder adapter;
BSN::EncodeContext Context(uint32_t id, void* data) {
    BSN::EncodeContext result{&Protocol::s_protocol, id, 0, data};
    result.ResolveType();
    return result;
}
uint32_t NamedType(const char* name) {
    auto& p = Protocol::s_protocol;
    for (uint32_t id = 1; id < p.m_typeIdCount; ++id) {
        const char* candidate = p.DumpString(p.DumpInfo(id), 0);
        if (candidate && !strcmp(name, candidate)) return id;
    }
    CHECK(false);
    return 0;
}
void Copy(BSN::EncodeContext& source, BSN::EncodeContext& target) {
    CHECK(adapter.WalkAndGet(&adapter, &source, &target));
    CHECK(adapter.WalkAndSet(&adapter, &target, &source));
}
void ScalarsAndStrings() {
    uint32_t source = 0, target = 0;
    auto a = Context(NamedType("Battlenet::u32"), &source);
    auto b = Context(a.m_typeId, &target);
    CHECK(adapter.SetUInt(&a, 0xfedcba98));
    CHECK(!adapter.SetUInt(&a, UINT64_MAX));
    Copy(a, b);
    CHECK(target == source && adapter.GetUInt(&b) == source);
    CHECK(!adapter.SetString(&a, "wrong type"));
    CHECK(!adapter.GetOptional(&a));

    BSN::AsciiString<320> asciiSource, asciiTarget;
    a = Context(NamedType("Battlenet::Account::Mail"), &asciiSource);
    b = Context(a.m_typeId, &asciiTarget);
    CHECK(adapter.SetString(&a, "local@example.invalid"));
    Copy(a, b);
    char text[64];
    CHECK(adapter.GetString(&b, text, sizeof(text)));
    CHECK(!strcmp(text, "local@example.invalid"));
    CHECK(adapter.GetStringLength(&b) == strlen(text));
    char small[4];
    CHECK(adapter.GetString(&b, small, sizeof(small)) && !strcmp(small, "loc"));

    BSN::String<32> utfSource, utfTarget;
    a = Context(NamedType("Battlenet::Account::NamePart"), &utfSource);
    b = Context(a.m_typeId, &utfTarget);
    CHECK(adapter.SetString(&a, "A\xc3\xa9"));
    Copy(a, b);
    CHECK(adapter.GetString(&b, text, sizeof(text)) && !strcmp(text, "A\xc3\xa9"));
    CHECK(adapter.GetStringLength(&b) == 3);
    // Existing String adapter stores codepoint count in the first slot,
    // despite Types.h naming that slot m_size (and the byte slot m_length).
    CHECK(utfTarget.m_size == 2);
    CHECK(!adapter.SetString(&a, "\xff"));
}
void ArraysAndOptionals() {
    BSN::Array<BSN::FourCC, 15> source, target;
    auto a = Context(NamedType("Battlenet::Achievement::ProgramList"), &source);
    auto b = Context(a.m_typeId, &target);
    CHECK(adapter.SetArray(&a, 2));
    BSN::EncodeContext element{};
    CHECK(adapter.GetElementContext(&a, 0, &element));
    CHECK(element.m_header == BSN::META_FOURCC);
    CHECK(adapter.SetFourCC(&element, 0x576f57));
    CHECK(adapter.GetElementContext(&a, 1, &element));
    CHECK(adapter.SetFourCC(&element, 0x5332));
    CHECK(!adapter.GetElementContext(&a, 2, &element));
    BSN::Protocol::Meta::Array meta(a.m_protocol, a.m_typeId);
    CHECK(!adapter.SetArray(&a, static_cast<uint32_t>(meta.m_bounds.m_max + 1)));
    Copy(a, b);
    CHECK(adapter.GetArrayLength(&b) == 2);
    CHECK(target.m_data[0].m_value == 0x576f57 && target.m_data[1].m_value == 0x5332);

    // Obtain the optional's real type from its generated containing struct.
    auto& p = Protocol::s_protocol;
    BSN::Protocol::Meta::Struct container(&p, NamedType("Battlenet::Attribute::LocalizedString"));
    uint32_t offset, id;
    CHECK(container.GetMember(0, offset, id) && offset == 0);
    Battlenet::Attribute::LocalizedString left{}, right{};
    a = Context(id, &left.m_colorHint);
    b = Context(id, &right.m_colorHint);
    CHECK(!adapter.GetOptional(&a));
    CHECK(!adapter.GetMemberContext(&a, "", &element));
    CHECK(adapter.SetOptional(&a, true));
    CHECK(adapter.GetMemberContext(&a, "", &element));
    CHECK(adapter.SetUInt(&element, 0x123456));
    Copy(a, b);
    CHECK(right.m_colorHint.m_exists && right.m_colorHint.m_value == 0x123456);
    CHECK(adapter.SetOptional(&a, false));
    Copy(a, b);
    CHECK(!right.m_colorHint.m_exists);
}
void Choices() {
    Battlenet::Presence::FieldVal source, target;
    auto a = Context(Protocol::TypeIdOf<Battlenet::Presence::FieldVal>::s_typeId, &source);
    auto b = Context(a.m_typeId, &target);
    CHECK(adapter.SetChoice(&a, "", "s32val"));
    BSN::EncodeContext member{};
    CHECK(adapter.GetMemberContext(&a, "s32val", &member));
    CHECK(adapter.SetInt(&member, -12345));
    Copy(a, b);
    char name[64];
    CHECK(adapter.GetChoiceName(&b, name, sizeof(name)) && !strcmp(name, "s32val"));
    CHECK(target.m_int == -12345);
    CHECK(!adapter.SetChoice(&a, "", "not-a-generated-choice"));
    CHECK(!adapter.GetMemberContext(&a, "not-a-generated-choice", &member));
    CHECK(adapter.SetChoice(&a, "", "stringval"));
    CHECK(adapter.GetMemberContext(&a, "stringval", &member));
    CHECK(adapter.SetString(&member, "choice text"));
    Copy(a, b);
    CHECK(!strcmp(target.m_string.m_data, "choice text"));
}
void Codec() {
    // This is the existing generated hard codec, not a test wire schema.
    Battlenet::IP4::AddressPort source{}, target{};
    source.m_address[0] = 127;
    source.m_address[3] = 1;
    source.m_port[0] = 0x30;
    source.m_port[1] = 0x39;
    uint8_t bytes[6]{};
    BSN::Hard::Encoder encoder;
    encoder.SetData(bytes, sizeof(bytes));
    CHECK(ProtocolHard::TypeInfoOf<Battlenet::IP4::AddressPort>::Encode(source, encoder) == BSN::CODEC_VALID);
    BSN::Hard::Decoder decoder;
    decoder.SetData(bytes, sizeof(bytes));
    CHECK(ProtocolHard::TypeInfoOf<Battlenet::IP4::AddressPort>::Decode(target, decoder) == BSN::CODEC_VALID);
    CHECK(!memcmp(target.m_address, source.m_address, sizeof(source.m_address)));
    CHECK(!memcmp(target.m_port, source.m_port, sizeof(source.m_port)));
    encoder.SetData(bytes, 5);
    CHECK(ProtocolHard::TypeInfoOf<Battlenet::IP4::AddressPort>::Encode(source, encoder) == BSN::CODEC_OVERFLOW);
    decoder.SetData(bytes, 5);
    CHECK(ProtocolHard::TypeInfoOf<Battlenet::IP4::AddressPort>::Decode(target, decoder) == BSN::CODEC_OVERFLOW);
}
}
int main() {
    ScalarsAndStrings();
    ArraysAndOptionals();
    Choices();
    Codec();
    puts("BSN generated adapters, walker roundtrips and existing hard codec passed");
}
