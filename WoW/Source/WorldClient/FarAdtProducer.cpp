#include "FarAdtProducer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <limits>
#include <new>

namespace FarAdt {
namespace {
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
float F32(const uint8_t* p) { uint32_t n = U32(p); float f; std::memcpy(&f, &n, 4); return f; }
struct Span { size_t offset = 0, bytes = 0; bool present = false; };
bool Record(const Tile& t, size_t offset, uint32_t token, Span& s) {
    if (offset > t.encodedBytes || t.encodedBytes-offset < 8) return false;
    const auto* p = t.encoded.get()+offset;
    const size_t bytes = U32(p+4);
    if (U32(p) != token || bytes > t.encodedBytes-offset-8) return false;
    s = {offset+8, bytes, true}; return true;
}
bool AlphaValid(const uint8_t* p, size_t n, bool compressed, bool big) {
    if (!compressed) return n >= (big ? 4096u : 2048u);
    size_t out = 0, at = 0;
    while (out < 4096) {
        if (at == n) return false;
        const unsigned control = p[at++], count = control & 127;
        const size_t input = control & 128 ? 1 : count;
        if (!count || count > 4096-out || input > n-at) return false;
        at += input; out += count;
    }
    return true;
}
}
DecodeResult Decode(Tile& t) {
    if (!t.encoded || t.encodedBytes > EncodedLimit) return DecodeResult::Malformed;
    Span version, header, index, names, flags;
    if (!Record(t, 0, 0x4d564552, version) || version.bytes != 4) return DecodeResult::Malformed;
    if (U32(t.encoded.get()+version.offset) != 18) return DecodeResult::Unsupported;
    const size_t headerAt = version.offset+version.bytes;
    if (!Record(t, headerAt, 0x4d484452, header) || header.bytes != 64) return DecodeResult::Malformed;
    const auto* h = t.encoded.get()+header.offset;
    auto fromHeader = [&](size_t field, uint32_t token, Span& s, bool optional) {
        const size_t off = U32(h+field);
        if (!off) return optional;
        if (off > t.encodedBytes-header.offset) return false;
        return Record(t, header.offset+off, token, s);
    };
    if (!fromHeader(4, 0x4d43494e, index, false) || index.bytes != 256*16 ||
        !fromHeader(8, 0x4d544558, names, false) || !names.bytes ||
        !fromHeader(44, 0x4d545846, flags, true)) return DecodeResult::Malformed;
    uint32_t count = 0;
    for (size_t at = 0; at < names.bytes;) {
        const auto* start = t.encoded.get()+names.offset+at;
        const auto* end = static_cast<const uint8_t*>(std::memchr(start, 0, names.bytes-at));
        if (!end || end == start || size_t(end-start) >= 260 || ++count > 4096) return DecodeResult::Malformed;
        at += size_t(end-start)+1;
    }
    if (flags.present && flags.bytes != size_t(count)*4) return DecodeResult::Malformed;
    t.textureCount = count;
    t.textureNamesOffset = uint32_t(names.offset); t.textureNamesBytes = uint32_t(names.bytes);
    t.textureFlagsOffset = uint32_t(flags.offset); t.textureFlagsBytes = uint32_t(flags.bytes);
    std::array<bool, 256> seen{};
    for (size_t i = 0; i < 256; ++i) {
        const auto* entry = t.encoded.get()+index.offset+i*16;
        const size_t offset = U32(entry), size = U32(entry+4);
        Span chunk;
        if (!Record(t, offset, 0x4d434e4b, chunk) || chunk.bytes < 128 || size != chunk.bytes+8) return DecodeResult::Malformed;
        const auto* c = t.encoded.get()+chunk.offset;
        const uint32_t x = U32(c+4), y = U32(c+8), layers = U32(c+12);
        if (x >= 16 || y >= 16 || seen[y*16+x] || y*16+x != i || !layers || layers > 4) return DecodeResult::Malformed;
        seen[i] = true;
        auto& dst = t.chunks[i];
        dst.flags = U32(c); dst.areaId = U32(c+52); dst.layerCount = layers;
        dst.holes = uint16_t(c[60]) | uint16_t(c[61])<<8;
        for (unsigned j = 0; j < 3; ++j) {dst.position[j] = F32(c+104+j*4); if (!std::isfinite(dst.position[j])) return DecodeResult::Malformed;}
        Span height, normal, layer, alpha, vertexShading, shadow;
        // Offsets in SMChunk are relative to the MCNK record (including its 8-byte prefix).
        auto sub = [&](size_t field, uint32_t token, Span& s, bool optional) {
            const size_t off = U32(c+field);
            if (!off) return optional;
            if (off < 136 || off > chunk.bytes+8 || !Record(t, offset+off, token, s)) return false;
            return s.offset+s.bytes <= chunk.offset+chunk.bytes;
        };
        if (!sub(20, 0x4d435654, height, false) || height.bytes != 145*4 ||
            !sub(24, 0x4d434e52, normal, false) || (normal.bytes != 435 && normal.bytes != 448) ||
            !sub(28, 0x4d434c59, layer, false) || layer.bytes != layers*16 ||
            !sub(36, 0x4d43414c, alpha, true) ||
            !sub(116, 0x4d434356, vertexShading, true) ||
            (vertexShading.present && vertexShading.bytes != 145*4) ||
            // An absent Northrend MCSH can retain a nonzero ofsShadow alias
            // into MCAL. sizeShadow, not that alias, establishes presence.
            (U32(c+48) && (!sub(44, 0x4d435348, shadow, false) || shadow.bytes != 64*64/8))) return DecodeResult::Malformed;
        dst.vertexShadingOffset = uint32_t(vertexShading.offset); dst.vertexShadingBytes = uint32_t(vertexShading.bytes);
        dst.shadowOffset = uint32_t(shadow.offset); dst.shadowBytes = uint32_t(shadow.bytes);
        const size_t alphaSize=U32(c+40);
        if(alpha.present) {
            // Shipped compressed Northrend MCAL records can declare zero bytes.
            // MCNK's sizeAlpha (including the prefix) owns the actual payload.
            if(alphaSize<8) return DecodeResult::Malformed;
            const size_t payload=alphaSize-8;
            if(payload>chunk.offset+chunk.bytes-alpha.offset || (alpha.bytes && alpha.bytes!=payload))
                return DecodeResult::Malformed;
            alpha.bytes=payload;
        } else if(alphaSize) return DecodeResult::Malformed;
        dst.alphaOffset = uint32_t(alpha.offset); dst.alphaBytes = uint32_t(alpha.bytes);
        for (unsigned j = 0; j < 145; ++j) {dst.heights[j] = F32(t.encoded.get()+height.offset+j*4); if (!std::isfinite(dst.heights[j])) return DecodeResult::Malformed;}
        std::memcpy(dst.normals.data(), t.encoded.get()+normal.offset, dst.normals.size());
        for (unsigned j = 0; j < layers; ++j) {
            const auto* l = t.encoded.get()+layer.offset+j*16;
            auto& out = dst.layers[j]; out = {U32(l), U32(l+4), U32(l+8), U32(l+12)};
            if (out.textureId >= count) return DecodeResult::Malformed;
            if (out.flags & 0x100) {
                if (!j || !alpha.present || out.alphaOffset >= alpha.bytes) return DecodeResult::Malformed;
                size_t end = alpha.bytes;
                for (unsigned k = j+1; k < layers; ++k) {
                    const auto* next = t.encoded.get()+layer.offset+k*16;
                    if (U32(next+4) & 0x100) {end = U32(next+8); break;}
                }
                if (end <= out.alphaOffset || end > alpha.bytes ||
                    !AlphaValid(t.encoded.get()+alpha.offset+out.alphaOffset, end-out.alphaOffset,
                                (out.flags & 0x200) != 0, (t.mapFlags & 4) != 0)) return DecodeResult::Malformed;
            }
        }
    }
    return DecodeResult::Ready;
}
float Distance(Key key, const View& view) {
    // Same axis swap as World::PrepareAreaOfInterest: ADT x indexes world y.
    const float highX = 17066.666f-key.y*TileSize, highY = 17066.666f-key.x*TileSize;
    const float dx = std::max({highX-TileSize-view.x, view.x-highX, 0.f});
    const float dy = std::max({highY-TileSize-view.y, view.y-highY, 0.f});
    return std::hypot(dx, dy);
}
bool Requested(Key key, const View& view, float hysteresis) {
    if (key.x >= 64 || key.y >= 64 || !std::isfinite(view.x) || !std::isfinite(view.y) ||
        !std::isfinite(view.farDistance) || !std::isfinite(view.nearDistance) ||
        view.nearDistance < 0 || view.farDistance <= view.nearDistance || view.farDistance > 10000) return false;
    const float distance = Distance(key, view);
    return distance > view.nearDistance && distance <= view.farDistance+hysteresis;
}
struct Producer::State {
    Hooks hooks;
    std::shared_ptr<const void> charge;
    View view{};
    bool haveView = false;
    uint64_t generation = 1, frame = 0;
    Diagnostics census;
    struct Entry { Key key; std::shared_ptr<const Tile> tile; };
    std::array<Entry, 4> residents{};
    std::array<std::shared_ptr<Job>, 2> jobs{};
    std::array<uint64_t, 4096> retry{}, transfers{};
    uint64_t nextTicket = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
    explicit State(Hooks h, std::shared_ptr<const void> c): hooks(std::move(h)), charge(std::move(c)), worker([this] { Work(); }) {}
    void Work() {
        for (;;) {
            std::shared_ptr<Job> selected;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(5), [&] {
                    if (stopping) return true;
                    for (const auto& job: jobs) if (job && job->readDone.load() && !job->decodeDone.load()) return true;
                    return false;
                });
                if (stopping) return;
                for (const auto& job: jobs) if (job && job->readDone.load() && !job->decodeDone.load()) {selected = job; break;}
            }
            if (!selected) continue;
            if (selected->readSucceeded) {
                selected->tile->encodedBytes = selected->bytesRead;
                selected->result = Decode(*selected->tile);
            }
            // Drop worker ownership before publication: final charge releases
            // must be on the owner, never this worker.
            auto* done = &selected->decodeDone;
            selected.reset();
            done->store(true);
        }
    }
};
Producer::Producer(Hooks hooks) {
    // Persistent request/retry table and worker metadata are charged too.
    const size_t bytes = sizeof(State)+sizeof(Producer)+512;
    if (bytes > PartitionLimit || hooks.farBytes() > PartitionLimit-bytes) throw std::bad_alloc();
    auto charge = hooks.reserve(bytes);
    if (!charge) throw std::bad_alloc();
    state.reset(new State(std::move(hooks), std::move(charge)));
}
Producer::~Producer() {
    {std::lock_guard<std::mutex> lock(state->mutex); state->stopping = true;}
    state->wake.notify_one(); state->worker.join();
}
size_t Producer::ReservationBytes() { return EncodedLimit+sizeof(Tile)+sizeof(Job)+AdapterBytes; }
void Producer::Reset() {
    auto& s = *state; ++s.generation; s.haveView = false; s.retry.fill(0); s.transfers.fill(0);
    s.census.transferred = 0;
    for (auto& entry: s.residents) if (entry.tile) {entry.tile.reset(); ++s.census.evicted;}
    s.census.requested = s.census.resident = s.census.ready = 0;
    s.census.farthestReady = 0;
    s.census.pending = s.census.retired = 0;
    for (auto& job: s.jobs) if (job) { job->cancelled = true; ++s.census.pending; ++s.census.retired; }
    s.census.chargedBytes = s.hooks.farBytes();
}
void Producer::Pump(const View& view) {
    auto& s = *state; ++s.frame;
    if (s.haveView && (view.map != s.view.map || view.mapFlags != s.view.mapFlags ||
        std::hypot(view.x-s.view.x, view.y-s.view.y) > TileSize*2)) Reset();
    s.view = view; s.haveView = true;
    const bool pressure = s.hooks.nearPressure();
    for (auto& entry: s.residents) if (entry.tile && (pressure || !Requested(entry.key, view, TileSize))) {
        entry.tile.reset(); ++s.census.evicted;
    }
    for (auto& job: s.jobs) if (job && (pressure || !Requested(job->key, view, TileSize))) job->cancelled = true;
    // At most one completed payload is published/discarded on each pump.
    for (auto& slot: s.jobs) {
        if (!slot || !slot->decodeDone.load()) continue;
        auto job = slot;
        {std::lock_guard<std::mutex> lock(s.mutex); slot.reset();}
        if (!job->cancelled && job->generation == s.generation && Requested(job->key, view, TileSize)) {
            if (!job->readSucceeded) ++s.census.readFailed;
            else if (job->result == DecodeResult::Unsupported) ++s.census.unsupported;
            else if (job->result != DecodeResult::Ready) ++s.census.malformed;
            else for (auto& entry: s.residents) if (!entry.tile) {entry = {job->key, job->tile}; break;}
            s.retry[size_t(job->key.y)*64+job->key.x] = s.frame+120;
        }
        break;
    }
    s.census.requested = 0;
    bool haveCandidate = false;
    Key candidate;
    float best = 1e30f;
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        Key key{uint8_t(x), uint8_t(y)};
        const size_t index = y*64+x;
        if (s.transfers[index] && (pressure || !Requested(key, view, TileSize))) s.transfers[index] = 0;
        if (!Requested(key, view) || !s.hooks.exists(key)) continue;
        ++s.census.requested;
        bool owned = s.transfers[index] != 0;
        for (const auto& entry: s.residents) if (entry.tile && entry.key == key) owned = true;
        for (const auto& job: s.jobs) if (job && job->key == key) owned = true;
        if (owned || s.retry[y*64+x] > s.frame) continue;
        const float distance = Distance(key, view);
        if (distance < best) {best = distance; candidate = key; haveCandidate = true;}
    }
    bool emptyResident = false;
    for (const auto& entry: s.residents) if (!entry.tile) emptyResident = true;
    auto freeSlot = std::find(s.jobs.begin(), s.jobs.end(), nullptr);
    if (haveCandidate && !emptyResident) ++s.census.budgetDeferred;
    if (haveCandidate && emptyResident && freeSlot != s.jobs.end()) {
        const size_t bytes = ReservationBytes();
        auto charge = !pressure && s.hooks.farBytes() <= PartitionLimit-bytes ? s.hooks.reserve(bytes) : nullptr;
        if (!charge) ++s.census.budgetDeferred;
        else {
            try {
                auto job = std::make_shared<Job>(); job->tile = std::make_shared<Tile>();
                job->tile->charge = charge;
                job->tile->encoded.reset(new uint8_t[EncodedLimit]);
                job->tile->mapFlags = view.mapFlags; job->key = candidate; job->generation = s.generation;
                // Store before issuing; synchronous failure is represented as a
                // failed read and drained by the same bounded completion path.
                {std::lock_guard<std::mutex> lock(s.mutex); *freeSlot = job;}
                ++s.census.issued;
                try {
                    if (!s.hooks.read(job)) job->readDone.store(true);
                } catch (const std::bad_alloc&) { job->readDone.store(true); }
                s.wake.notify_one();
            } catch (const std::bad_alloc&) { ++s.census.budgetDeferred; }
        }
    }
    s.census.resident = s.census.ready = s.census.pending = s.census.retired = 0;
    s.census.farthestReady = 0;
    for (const auto& entry: s.residents) if (entry.tile) {
        ++s.census.resident; ++s.census.ready;
        s.census.farthestReady = std::max(s.census.farthestReady, Distance(entry.key, view));
    }
    for (const auto& job: s.jobs) if (job) {++s.census.pending; if (job->cancelled) ++s.census.retired;}
    s.census.transferred = uint32_t(std::count_if(s.transfers.begin(), s.transfers.end(), [](uint64_t ticket) { return ticket != 0; }));
    s.census.chargedBytes = s.hooks.farBytes();
}
std::shared_ptr<const Tile> Producer::Ready(Key key) const {
    for (const auto& entry: state->residents) if (entry.tile && entry.key == key) return entry.tile;
    return {};
}
ReadyPayload Producer::ConsumeReady(Key key) {
    auto& s = *state;
    if (key.x >= 64 || key.y >= 64 || s.nextTicket == UINT64_MAX) return {};
    for (auto& entry: s.residents) if (entry.tile && entry.key == key) {
        ReadyPayload payload{key, ++s.nextTicket, std::move(entry.tile), s.view.map, s.generation};
        s.transfers[size_t(key.y)*64+key.x] = payload.ticket;
        ++s.census.consumed; ++s.census.transferred;
        --s.census.ready; --s.census.resident;
        s.census.farthestReady = 0;
        for (const auto& remaining: s.residents) if (remaining.tile)
            s.census.farthestReady = std::max(s.census.farthestReady, Distance(remaining.key, s.view));
        return payload;
    }
    return {};
}
void Producer::Release(ReadyPayload& payload) {
    auto& s = *state;
    if (payload.ticket && payload.key.x < 64 && payload.key.y < 64) {
        auto& ticket = s.transfers[size_t(payload.key.y)*64+payload.key.x];
        if (ticket == payload.ticket) { ticket = 0; --s.census.transferred; }
    }
    payload.tile.reset(); payload.ticket = 0;
    s.census.chargedBytes = s.hooks.farBytes();
}
bool Producer::Current(const ReadyPayload& p) const {
    const auto& s=*state;
    return p.ticket && p.key.x<64 && p.key.y<64 && s.haveView && p.map==s.view.map &&
        p.generation==s.generation && s.transfers[size_t(p.key.y)*64+p.key.x]==p.ticket &&
        !s.hooks.nearPressure() && Requested(p.key,s.view,TileSize);
}
const Diagnostics& Producer::Census() const { return state->census; }
}
