#ifndef WOW_WORLDCLIENT_FARADTPRODUCER_H
#define WOW_WORLDCLIENT_FARADTPRODUCER_H
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace FarAdt {
constexpr size_t EncodedLimit = 4 * 1024 * 1024;
constexpr size_t PartitionLimit = 16 * 1024 * 1024;
constexpr size_t NearHeadroom = 13 * 1024 * 1024;
constexpr size_t AdapterBytes = 768; // async object/read/lease + bounded control-block metadata
constexpr float TileSize = 1600.f / 3.f;
struct Layer { uint32_t textureId, flags, alphaOffset, effectId; };
struct Chunk {
    uint32_t flags = 0, areaId = 0, layerCount = 0;
    uint16_t holes = 0;
    std::array<float, 3> position{};
    std::array<float, 145> heights{};
    // Authored MCNR order/handedness, not converted to renderer coordinates.
    std::array<int8_t, 435> normals{};
    std::array<Layer, 4> layers{};
    uint32_t alphaOffset = 0, alphaBytes = 0;
    // Source SMChunk::m_ofsMCCV/m_ofsShadow; spans in Tile::encoded.
    // Absent MCCV uses stock neutral vertex colour, not a white substitute.
    uint32_t vertexShadingOffset = 0, vertexShadingBytes = 0;
    uint32_t shadowOffset = 0, shadowBytes = 0;
};
struct Tile {
    // Keep exact MCLY flags (UV animation/orientation/specular), MCAL encoding,
    // MTEX names and MTXF flags. Consumers must honor WDT/chunk blend flags.
    std::shared_ptr<const void> charge;
    std::unique_ptr<uint8_t[]> encoded;
    size_t encodedBytes = 0;
    uint32_t mapFlags = 0, textureNamesOffset = 0, textureNamesBytes = 0;
    uint32_t textureFlagsOffset = 0, textureFlagsBytes = 0, textureCount = 0;
    std::array<Chunk, 256> chunks{};
};
enum class DecodeResult { Ready, Malformed, Unsupported };
// No allocation, I/O, game objects or texture calls. Source layouts: SMChunk,
// SMChunkInfo, SMAreaHeader, SMLayer. Tile owns the original bytes beforehand.
DecodeResult Decode(Tile& tile);
struct Key { uint8_t x = 0, y = 0; bool operator==(Key b) const { return x == b.x && y == b.y; } };
struct View { float x = 0, y = 0, nearDistance = 0, farDistance = 0; uint64_t map = 0; uint32_t mapFlags = 0; };
float Distance(Key key, const View& view);
bool Requested(Key key, const View& view, float hysteresis = 0);
struct Job {
    std::shared_ptr<Tile> tile;
    Key key;
    uint64_t generation = 0;
    bool cancelled = false;
    // Read adapter writes bytes/success before release-store readDone; decoder
    // publishes result before release-store decodeDone. Owner never waits.
    size_t bytesRead = 0;
    bool readSucceeded = false;
    std::atomic<bool> readDone{false}, decodeDone{false};
    DecodeResult result = DecodeResult::Malformed;
};
struct ReadyPayload {
    Key key;
    uint64_t ticket = 0;
    std::shared_ptr<const Tile> tile;
    uint64_t map = 0, generation = 0;
};
struct Diagnostics {
    uint32_t requested = 0, resident = 0, ready = 0, pending = 0, retired = 0, transferred = 0;
    uint64_t consumed = 0;
    uint64_t issued = 0, evicted = 0, budgetDeferred = 0, malformed = 0, unsupported = 0, readFailed = 0;
    size_t chargedBytes = 0;
    float farthestReady = 0;
};
struct Hooks {
    // All hooks and lease releases run on the owner thread. Reserve must not
    // evict near resources. bytes includes current, retained, retired/inflight.
    std::function<std::shared_ptr<const void>(size_t)> reserve;
    std::function<size_t()> farBytes;
    std::function<bool()> nearPressure;
    std::function<bool(Key)> exists;
    std::function<bool(const std::shared_ptr<Job>&)> read;
};
class Producer {
public:
    explicit Producer(Hooks hooks);
    ~Producer(); // teardown only; joins pure CPU worker, never waits for I/O
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    void Pump(const View& view); // <=1 issue and <=1 completion; no waits
    void Reset(); // invalidate map/teleport/Classic; retain inflight charges
    std::shared_ptr<const Tile> Ready(Key key) const; // owner-thread lease
    // Owner-thread transfer: suppress rereads while the consumer owns this key.
    // All copies of tile remain charged, even after Release/reset/eviction.
    // Transfer is not evidence of upload, material readiness or draw coverage.
    ReadyPayload ConsumeReady(Key key);
    void Release(ReadyPayload& payload); // stale tickets cannot release a newer transfer
    bool Current(const ReadyPayload&) const;
    const Diagnostics& Census() const;
    static size_t ReservationBytes();
private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
