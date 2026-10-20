#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/MPQEncrypt.h"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace fs = std::filesystem;
void Check(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
std::string Text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    Check(bool(f), "missing fixture report");
    return {(std::istreambuf_iterator<char>(f)), {}};
}
void Word(std::string &s, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        s.push_back(char(v >> (i * 8)));
}
void Archive(const fs::path &path, bool omitName, bool unsafeName, bool truncateShader) {
    const std::vector<std::string> names = {"(listfile)", "Shaders\\Pixel\\ps_3_0\\Fixture.bls", "World\\Named.bin"};
    std::string shader;
    Word(shader, 0x47585348);
    Word(shader, 0x10003);
    Word(shader, 2);
    for (unsigned i = 0; i < 2; ++i) {
        Word(shader, 0);
        Word(shader, 0);
        Word(shader, 0);
        Word(shader, 8);
        Word(shader, 0xffff0300);
        Word(shader, 0xffff);
    }
    if (truncateShader)
        shader.pop_back();
    const std::string list = names[1] + (omitName ? "" : "\r\n" + names[2]) + (unsafeName ? "\n../escape" : "");
    const std::vector<std::string> data = {list, shader, "owned fixture payload"};
    std::vector<uint32_t> hashes(8 * 4, UINT32_MAX), blocks;
    std::string image(44, '\0');
    for (size_t i = 0; i < names.size(); ++i) {
        uint32_t slot = System_MPQEncrypt::Hash(names[i].c_str(), 0) & 7;
        while (hashes[slot * 4 + 3] != UINT32_MAX)
            slot = (slot + 1) & 7;
        hashes[slot * 4] = System_MPQEncrypt::Hash(names[i].c_str(), 1);
        hashes[slot * 4 + 1] = System_MPQEncrypt::Hash(names[i].c_str(), 2);
        hashes[slot * 4 + 2] = 0;
        hashes[slot * 4 + 3] = i;
        blocks.insert(blocks.end(),
                      {uint32_t(image.size()), uint32_t(data[i].size()), uint32_t(data[i].size()), 0x80000000});
        image += data[i];
    }
    const auto hashOffset = image.size();
    System_MPQEncrypt::Encrypt(hashes.data(), hashes.size() * 4, System_MPQEncrypt::Hash("(hash table)", 3));
    image.append(reinterpret_cast<const char *>(hashes.data()), hashes.size() * 4);
    const auto blockOffset = image.size();
    System_MPQEncrypt::Encrypt(blocks.data(), blocks.size() * 4, System_MPQEncrypt::Hash("(block table)", 3));
    image.append(reinterpret_cast<const char *>(blocks.data()), blocks.size() * 4);
    const uint32_t header[] = {0x1a51504d,
                               44,
                               uint32_t(image.size()),
                               1 | (3u << 16),
                               uint32_t(hashOffset),
                               uint32_t(blockOffset),
                               8,
                               uint32_t(names.size())};
    for (size_t i = 0; i < 8; ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            image[i * 4 + byte] = char(header[i] >> (byte * 8));
    std::ofstream f(path, std::ios::binary);
    f.write(image.data(), image.size());
    Check(bool(f), "fixture archive write failed");
}
int Run(const fs::path &tool, const fs::path &archive, const fs::path &report, const fs::path &log) {
    const auto child = fork();
    Check(child >= 0, "fork failed");
    if (!child) {
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            _exit(125);
        if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(126);
        close(fd);
        execl(tool.c_str(), tool.c_str(), report.c_str(), archive.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child, "waitpid failed");
    Check(WIFEXITED(status), "inventory process crashed");
    return WEXITSTATUS(status);
}
int main(int argc, char **argv) {
    fs::path root;
    try {
        Check(argc == 2, "inventory executable argument required");
        const auto tool = fs::canonical(argv[1]);
        root = fs::current_path() / ("asset-inventory-fixture-" + std::to_string(getpid()));
        Check(fs::create_directory(root), "refusing to reuse fixture directory");
        fs::create_directory(root / "archives");
        fs::create_directory(root / "reports");
        for (const auto &mode : {"positive", "unknown-name", "unsafe-name", "truncated-shader"}) {
            const std::string name = mode;
            const auto archive = root / "archives" / (name + ".mpq"), report = root / "reports" / name;
            Archive(archive, name == "unknown-name", name == "unsafe-name", name == "truncated-shader");
            const auto before = Text(archive);
            const int result = Run(tool, archive, report, root / (name + ".log"));
            Check(Text(archive) == before, "read-only inventory changed archive bytes");
            if (name == "positive") {
                Check(result == 0, "complete synthetic inventory rejected");
                const auto summary = Text(report / "summary.txt");
                Check(summary.find("coverage=UNVERIFIED") != std::string::npos &&
                          summary.find("shader_records=2") != std::string::npos &&
                          summary.find("unknown_hash_rows=0") != std::string::npos,
                      "inventory claimed coverage or collapsed duplicate identities");
                const auto original = summary;
                Check(Run(tool, archive, report, root / "overwrite.log") == 1, "existing report overwrite accepted");
                Check(Text(report / "summary.txt") == original, "existing report changed");
                Check(Run(tool, archive, root / "archives" / "forbidden", root / "external-write.log") == 1,
                      "report inside asset directory accepted");
                Check(!fs::exists(root / "archives" / "forbidden"), "external asset directory was mutated");
            } else {
                Check(result == 1, "incomplete/malformed inventory accepted");
                if (name == "unknown-name")
                    Check(Text(report / "summary.txt").find("unknown_hash_rows=1") != std::string::npos,
                          "missing archive-name denominator was concealed");
                if (name == "truncated-shader")
                    Check(Text(report / "summary.txt").find("shader_container_errors=1") != std::string::npos,
                          "truncated permutation census concealed");
            }
        }
        fs::remove_all(root);
        std::cout << "PASS read-only MPQ census, unknown hash rejection, final "
                     "list line, duplicate permutations, malformed/truncated "
                     "inputs and report confinement\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "; retained fixture " << root << '\n';
        return 1;
    }
}
