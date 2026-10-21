// Read-only corpus discovery. This is NOT the material/interface acceptance
// gate.
#include "AssetInventory.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/MPQEncrypt.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/SFile.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/SFileArchives.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
namespace fs = std::filesystem;
using namespace Blizzard::Mopaq;
using Archive = System_SFile::ARCHIVEREC;
struct CloseArchive {
    void operator()(Archive *a) const { SFileCloseArchive(a); }
};
using OwnedArchive = std::unique_ptr<Archive, CloseArchive>;
struct Mounted {
    fs::path path;
    OwnedArchive archive;
};
void Require(bool b, const std::string &message) {
    if (!b)
        throw std::runtime_error(message);
}
std::string Read(Archive *a, const std::string &name) {
    IOMpqUnit *raw = nullptr;
    Require(SFileOpenFileEx(a, name.c_str(), 0, &raw) != 0, "Cannot read archive asset: " + name);
    const std::unique_ptr<IOMpqUnit, decltype(&SFileCloseFile)> file(raw, SFileCloseFile);
    uint32_t high = 0;
    const auto size = SFileGetFileSize(file.get(), &high);
    Require(!high, "Asset exceeds runtime 32-bit file extent: " + name);
    std::string bytes(size, '\0');
    uint32_t read = 0;
    Require(SFileReadFile(file.get(), bytes.data(), size, &read) && read == size, "Incomplete archive read: " + name);
    return bytes;
}
uint64_t NameKey(const std::string &name) {
    return uint64_t(System_MPQEncrypt::Hash(name.c_str(), 1)) << 32 | System_MPQEncrypt::Hash(name.c_str(), 2);
}
bool Within(const fs::path &path, const fs::path &parent) {
    auto p = path.begin();
    for (const auto &c : parent) {
        if (p == path.end() || *p != c)
            return false;
        ++p;
    }
    return true;
}
int main(int argc, char **argv) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: RendererAssetInventory <new-report-directory> "
                         "<read-only-archive>... (lowest to highest priority)\n";
            return 2;
        }
        const auto report = fs::weakly_canonical(argv[1]);
        std::set<fs::path> paths;
        std::vector<fs::path> inputs;
        for (int i = 2; i < argc; ++i) {
            const auto path = fs::canonical(argv[i]);
            Require(fs::is_regular_file(path), "Archive must be a regular file");
            Require(path.string().size() < 260 && path.string().find_first_of("\t\r\n") == std::string::npos,
                    "Archive path exceeds runtime/report path contract");
            inputs.push_back(path);
            Require(paths.insert(path).second, "Duplicate archive input");
            Require(!Within(report, path.parent_path()),
                    "Reports must not be written inside external asset directories");
        }
        Require(!fs::exists(report) || fs::is_empty(report), "Refusing to overwrite an existing report");
        fs::create_directories(report);
        SFileSetThreadingMode(2);
        SFileEnableDirectAccess(0);
        std::vector<Mounted> mounted;
        std::set<std::string> names{"(LISTFILE)", "(ATTRIBUTES)", "(SIGNATURE)"};
        std::ofstream archiveReport(report / "archives.tsv"), unknownReport(report / "unknown-hashes.tsv"),
            assetReport(report / "assets.tsv"), shaderReport(report / "shader-records.tsv"),
            shaderErrors(report / "shader-errors.tsv");
        Require(bool(archiveReport) && bool(unknownReport) && bool(assetReport) && bool(shaderReport) &&
                    bool(shaderErrors),
                "Cannot create inventory reports");
        shaderErrors << "logical_path\tarchive_priority\terror\n";
        archiveReport << "priority\tarchive\tbytes\tactive_hash_rows\tnamed_hash_"
                         "rows\tunknown_hash_rows\n";
        unknownReport << "archive\thash_slot\thash_a\thash_b\tblock\tlocale\tplatform\n";
        assetReport << "logical_path\tstatus\tarchive_priority\tbytes\tflags\n";
        shaderReport << "logical_path\tarchive_priority\tpermutation\tbytecode_"
                        "bytes\tstage_version_token\n";
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto &path = inputs[i];
            Archive *raw = nullptr;
            // 0x400 only converts/discards the original hash table for compact
            // lookup. Keep it for denominator proof; retain the same read-only mode.
            Require(SFileOpenArchive(path.c_str(), static_cast<int32_t>(i), 0x800, &raw),
                    "Cannot open read-only archive: " + path.string());
            mounted.push_back({path, OwnedArchive(raw)});
            raw->InitializeTables();
            Require(raw->m_tablesInitialized && raw->m_hashTable.size() == raw->m_header.m_hashTableSize &&
                        !raw->m_hashTable.empty(),
                    "Unsupported archive name table; enumeration is incomplete: " + path.string());
            const auto listed = AssetInventory::ListNames(Read(raw, "(listfile)"));
            names.insert(listed.begin(), listed.end());
        }
        // Reconcile list names against actual MPQ hash rows, including names
        // supplied by other layers and special metadata. A listfile alone is not a
        // denominator.
        std::unordered_map<uint64_t, std::vector<uint32_t>> hashes;
        for (const auto &name : names)
            hashes[NameKey(name)].push_back(System_MPQEncrypt::Hash(name.c_str(), 0));
        size_t unknown = 0, active = 0;
        for (size_t priority = 0; priority < mounted.size(); ++priority) {
            auto &a = *mounted[priority].archive;
            size_t rows = 0, missing = 0;
            const auto &table = a.m_hashTable;
            const size_t mask = table.size() - 1;
            Require((table.size() & mask) == 0, "Non-power-of-two MPQ hash table");
            for (size_t slot = 0; slot < table.size(); ++slot) {
                const auto &h = table[slot];
                if (h.m_blockIndex >= 0xfffffffeu)
                    continue;
                Require(h.m_blockIndex < a.m_header.m_blockTableSize, "Invalid MPQ block reference");
                ++rows;
                bool named = false;
                const auto found = hashes.find(uint64_t(h.m_hashA) << 32 | h.m_hashB);
                if (found != hashes.end())
                    for (const auto hash : found->second) {
                        size_t probe = hash & mask;
                        for (size_t n = 0; n < table.size(); ++n) {
                            if (table[probe].m_blockIndex == 0xffffffffu)
                                break;
                            if (probe == slot) {
                                named = true;
                                break;
                            }
                            probe = (probe + 1) & mask;
                        }
                        if (named)
                            break;
                    }
                if (!named) {
                    ++missing;
                    unknownReport << priority << '\t' << slot << '\t' << h.m_hashA << '\t' << h.m_hashB << '\t'
                                  << h.m_blockIndex << '\t' << h.m_locale << '\t' << unsigned(h.m_platform) << '\n';
                }
            }
            archiveReport << priority << '\t' << mounted[priority].path.string() << '\t'
                          << fs::file_size(mounted[priority].path) << '\t' << rows << '\t' << rows - missing << '\t'
                          << missing << '\n';
            unknown += missing;
            active += rows;
        }
        size_t resident = 0, deleted = 0, stale = 0, containers = 0, records = 0, emptyRecords = 0, shaderFailures = 0;
        std::map<std::string, size_t> extensions;
        for (const auto &name : names) {
            Archive *resolved = nullptr;
            uint32_t block = 0;
            size_t priority = 0;
            bool tombstone = false;
            for (size_t i = mounted.size(); i > 0; --i) {
                auto *a = mounted[i - 1].archive.get();
                const auto index = System_SFile::SearchHashTable(a, name.c_str(), true, &tombstone);
                if (index != 0xffffffffu) {
                    resolved = a;
                    block = a->m_hashTable[index].m_blockIndex;
                    priority = i - 1;
                    break;
                }
            }
            if (!resolved) {
                assetReport << name << "\tstale-list-name\t-1\t0\t0\n";
                ++stale;
                continue;
            }
            const auto metadata = resolved->GetBlockEntry(block);
            assetReport << name << '\t' << (tombstone ? "authored-tombstone" : "resident") << '\t' << priority << '\t'
                        << metadata.m_fileSize << '\t' << metadata.m_flags << '\n';
            if (tombstone) {
                ++deleted;
                continue;
            }
            ++resident;
            const auto dot = name.find_last_of('.');
            const auto extension = dot == std::string::npos ? "" : name.substr(dot);
            ++extensions[extension];
            if (extension != ".BLS")
                continue;
            ++containers;
            std::vector<AssetInventory::ShaderRecord> programs;
            try {
                programs = AssetInventory::ShaderRecords(Read(resolved, name));
            } catch (const std::exception &e) {
                shaderErrors << name << '\t' << priority << '\t' << e.what() << '\n';
                ++shaderFailures;
                continue;
            }
            for (const auto &program : programs) {
                shaderReport << name << '\t' << priority << '\t' << program.permutation << '\t' << program.size << '\t'
                             << program.version << '\n';
                ++records;
                if (!program.size)
                    ++emptyRecords;
            }
        }
        std::ofstream summary(report / "summary.txt");
        summary << "coverage=UNVERIFIED (inventory only; no "
                   "material/maintained-interface classification)\n"
                << "enumeration=" << (unknown ? "INCOMPLETE" : "hash-row-reconciled") << "\narchives=" << mounted.size()
                << "\nactive_hash_rows=" << active << "\nunknown_hash_rows=" << unknown
                << "\nlogical_names=" << names.size() << "\nresident_names=" << resident
                << "\nauthored_tombstones=" << deleted << "\nstale_list_names=" << stale
                << "\nshader_containers=" << containers << "\nshader_records=" << records
                << "\nempty_shader_records=" << emptyRecords << "\nshader_container_errors=" << shaderFailures
                << "\nshader_enumeration=" << (shaderFailures ? "INCOMPLETE" : "record-complete") << '\n';
        for (const auto &extension : extensions)
            summary << "extension[" << extension.first << "]=" << extension.second << '\n';
        archiveReport.flush();
        unknownReport.flush();
        assetReport.flush();
        shaderReport.flush();
        shaderErrors.flush();
        summary.flush();
        Require(bool(archiveReport) && bool(unknownReport) && bool(assetReport) && bool(shaderReport) &&
                    bool(shaderErrors) && bool(summary),
                "Incomplete inventory report write");
        std::cout << "Inventory only; RT corpus coverage UNVERIFIED. Archives=" << mounted.size()
                  << " hashRows=" << active << " unknown=" << unknown << " shaderRecords=" << records << '\n';
        return unknown || shaderFailures || !containers || !records ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << "Inventory failed (coverage UNVERIFIED): " << e.what() << '\n';
        return 1;
    }
}
