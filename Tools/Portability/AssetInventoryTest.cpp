#include "Tools/Vulkan/AssetInventory.h"
#include <iostream>
#include <stdexcept>
using namespace AssetInventory;
void Check(bool b) {
    if (!b)
        throw std::runtime_error("inventory assertion failed");
}
template <class F> void Reject(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    Check(rejected);
}
void Append(std::string &s, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i)
        s.push_back(char(word >> (8 * i)));
}
int main() {
    try {
        const auto names = ListNames("Shaders/Pixel/A.bls\r\n\nshaders\\pixel\\a.bls\nWorld/Maps/Last.adt");
        Check(names.size() == 2 && names.count("WORLD\\MAPS\\LAST.ADT"));
        Check(ListNames(std::string(259, 'a')).size() == 1);
        for (const auto &bad :
             {std::string(260, 'a'), std::string("../escape"), std::string("a/../b"), std::string("a//b"),
              std::string("/root"), std::string("c:/file"), std::string("a\0b", 3), std::string("a\tb")})
            Reject([&] { ListNames(bad); });
        std::string bls;
        Append(bls, 0x47585348);
        Append(bls, 0x10003);
        Append(bls, 3);
        // Identical programs still occupy two independent permutation identities.
        for (unsigned i = 0; i < 3; ++i) {
            Append(bls, 0);
            Append(bls, 0);
            Append(bls, 0);
            Append(bls, i == 2 ? 0 : 8);
            if (i != 2) {
                Append(bls, 0xffff0300);
                Append(bls, 0xffff);
            }
        }
        const auto records = ShaderRecords(bls);
        Check(records.size() == 3 && records[0].permutation == 0 && records[1].permutation == 1 &&
              records[2].size == 0);
        // Every truncated prefix must reject rather than silently report a subset.
        for (size_t size = 0; size < bls.size(); ++size)
            Reject([&] { ShaderRecords(std::string_view(bls).substr(0, size)); });
        Reject([&] { ShaderRecords(bls + "extra"); });
        auto corrupt = bls;
        corrupt[24] = char(0xfc);
        Reject([&] { ShaderRecords(corrupt); });
        std::cout << "PASS complete list lines, strict paths, distinct "
                     "duplicate-bytecode identities, explicit empty records and "
                     "all truncated prefixes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
