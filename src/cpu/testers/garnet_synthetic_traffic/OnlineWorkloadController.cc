#include "cpu/testers/garnet_synthetic_traffic/OnlineWorkloadController.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <pybind11/pybind11.h>

#include "cpu/testers/garnet_synthetic_traffic/GarnetSyntheticTraffic.hh"
#include "mem/ruby/network/garnet/OnlinePacketRegistry.hh"
#include "sim/core.hh"
#include "sim/simulate.hh"

namespace gem5
{

OnlineWorkloadController::OnlineWorkloadController(
    const std::vector<GarnetSyntheticTraffic *> &endpoints_, bool debug_)
    : endpoints(endpoints_), debug(debug_)
{
}

namespace {

struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;
};

class JsonParser
{
  public:
    explicit JsonParser(const std::string &text) : text(text) {}
    Json parse()
    {
        Json value = parseValue();
        skip();
        if (pos != text.size())
            fail("trailing JSON data");
        return value;
    }
  private:
    const std::string &text;
    size_t pos = 0;
    [[noreturn]] void fail(const char *message) const
    { throw std::runtime_error(std::string("JSON: ") + message); }
    void skip() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; }
    void expect(char c)
    { skip(); if (pos >= text.size() || text[pos++] != c) fail("unexpected character"); }
    Json parseValue()
    {
        skip();
        if (pos >= text.size()) fail("unexpected end");
        switch (text[pos]) {
          case '{': return parseObject();
          case '[': return parseArray();
          case '"': { Json v; v.type = Json::String; v.string = parseString(); return v; }
          case 't': return parseLiteral("true", Json::Bool, true);
          case 'f': return parseLiteral("false", Json::Bool, false);
          case 'n': return parseLiteral("null", Json::Null, false);
          default: return parseNumber();
        }
    }
    Json parseLiteral(const char *literal, Json::Type type, bool value)
    {
        const size_t length = std::strlen(literal);
        if (text.compare(pos, length, literal) != 0) fail("invalid literal");
        pos += length; Json result; result.type = type; result.boolean = value; return result;
    }
    std::string parseString()
    {
        expect('"'); std::string result;
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') return result;
            if (c == '\\') {
                if (pos >= text.size()) fail("invalid escape");
                char e = text[pos++];
                const char *esc = "\"\\/\b\f\n\r\t";
                const char *at = std::strchr(esc, e);
                if (!at) fail("unsupported escape");
                static const char values[] = {'"','\\','/','\b','\f','\n','\r','\t'};
                result.push_back(values[at - esc]);
            } else result.push_back(c);
        }
        fail("unterminated string");
    }
    Json parseNumber()
    {
        skip(); const size_t start = pos;
        while (pos < text.size() && std::strchr("-+0123456789.eE", text[pos])) ++pos;
        if (start == pos) fail("invalid number");
        Json result; result.type = Json::Number;
        result.number = std::stod(text.substr(start, pos - start));
        return result;
    }
    Json parseArray()
    {
        expect('['); Json result; result.type = Json::Array; skip();
        if (pos < text.size() && text[pos] == ']') { ++pos; return result; }
        while (true) {
            result.array.push_back(parseValue()); skip();
            if (pos < text.size() && text[pos] == ']') { ++pos; return result; }
            expect(',');
        }
    }
    Json parseObject()
    {
        expect('{'); Json result; result.type = Json::Object; skip();
        if (pos < text.size() && text[pos] == '}') { ++pos; return result; }
        while (true) {
            skip(); if (pos >= text.size() || text[pos] != '"') fail("object key expected");
            std::string key = parseString(); expect(':');
            if (!result.object.emplace(key, parseValue()).second) fail("duplicate object key");
            skip(); if (pos < text.size() && text[pos] == '}') { ++pos; return result; }
            expect(',');
        }
    }
};

const Json &field(const Json &object, const char *key)
{
    if (object.type != Json::Object) throw std::runtime_error("JSON value is not an object");
    const auto it = object.object.find(key);
    if (it == object.object.end()) throw std::runtime_error(std::string("missing JSON field: ") + key);
    return it->second;
}

int intField(const Json &object, const char *key)
{ const Json &value = field(object, key); if (value.type != Json::Number) throw std::runtime_error("JSON integer expected"); return static_cast<int>(value.number); }
uint64_t uintField(const Json &object, const char *key)
{ const Json &value = field(object, key); if (value.type != Json::Number || value.number < 0) throw std::runtime_error("JSON unsigned integer expected"); return static_cast<uint64_t>(value.number); }
std::string stringField(const Json &object, const char *key)
{ const Json &value = field(object, key); if (value.type != Json::String) throw std::runtime_error("JSON string expected"); return value.string; }
std::vector<int> intArray(const Json &object, const char *key)
{ const Json &value = field(object, key); if (value.type != Json::Array) throw std::runtime_error("JSON array expected"); std::vector<int> result; for (const Json &item : value.array) { if (item.type != Json::Number) throw std::runtime_error("JSON integer array expected"); result.push_back(static_cast<int>(item.number)); } return result; }

std::string parentDir(const std::string &path)
{
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string joinPath(const std::string &base, const std::string &child)
{
    if (child.empty() || child.size() >= 2 && child[1] == ':' ||
        !child.empty() && (child[0] == '/' || child[0] == '\\'))
        return child.empty() ? base : child;
    if (base.empty())
        return child;
    return base + (base.back() == '/' || base.back() == '\\' ? "" : "/") + child;
}

std::string replaceExt(const std::string &path, const std::string &ext)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < slash)
        return path + ext;
    return path.substr(0, dot) + ext;
}

struct TraceReader {
    // runOnlineWorkload is a Python-bound method retaining the GIL. Reuse
    // its interpreter's buffered bz2 reader instead of spawning per group.
    pybind11::object stream = pybind11::none();
    std::string path;
    size_t read(void *buffer, size_t requested)
    {
        std::string data;
        try {
            data = stream.attr("read")(requested).cast<std::string>();
        } catch (const pybind11::error_already_set &error) {
            throw std::runtime_error(path + ": BZ2 read failed: " + error.what());
        }
        std::memcpy(buffer, data.data(), data.size());
        return data.size();
    }
    void skipBytes(uint64_t bytes, const std::string &path, const char *what)
    {
        char buffer[4096];
        while (bytes != 0) {
            const size_t requested = static_cast<size_t>(std::min<uint64_t>(bytes, sizeof(buffer)));
            if (read(buffer, requested) != requested)
                fail(path, what);
            bytes -= requested;
        }
    }
    explicit TraceReader(const std::string &path) : path(path)
    {
        stream = pybind11::module_::import("bz2").attr("open")(path, "rb");
        struct Header {
            uint32_t magic;
            float version;
            char benchmark[30];
            uint16_t nodes;
            uint8_t pad;
            uint64_t cycles;
            uint64_t packets;
            uint32_t notesLength;
            uint32_t regions;
            char padding[8];
        } __attribute__((packed));
        Header header{};
        if (read(&header, sizeof(header)) != sizeof(header))
            fail(path, "short netrace header");
        skipBytes(header.notesLength, path, "short netrace notes");
        skipBytes(header.regions * 24ULL, path, "short netrace regions");
    }
    [[noreturn]] void fail(const std::string &path, const char *why)
    {
        close();
        throw std::runtime_error(path + ": " + why);
    }
    bool next(OnlineWorkloadController::PacketTemplate *packet)
    {
        struct Raw {
            uint64_t cycle;
            uint32_t id;
            uint32_t addr;
            uint8_t type;
            uint8_t pad;
            uint16_t src;
            uint16_t dst;
            uint8_t nodeTypes;
            uint8_t deps;
            uint32_t bytes;
        } __attribute__((packed));
        Raw raw{};
        const size_t count = read(&raw, sizeof(raw));
        if (count == 0) {
            return false;
        }
        if (count != sizeof(raw))
            fail(path, "short netrace packet");
        if (raw.deps != 0)
            fail("netrace", "online templates cannot contain dependencies");
        packet->id = raw.id;
        packet->src = raw.src;
        packet->dst = raw.dst;
        packet->bytes = raw.bytes;
        return true;
    }
    void close()
    {
        if (stream.is_none())
            return;
        stream.attr("close")();
        stream = pybind11::none();
    }
    ~TraceReader() { close(); }
};

} // anonymous namespace

std::string
OnlineWorkloadController::run(const std::string &manifest,
                               const std::string &position,
                               const std::string &catalog,
                               const std::string &nonuniform_grid,
                               const std::string &endpoint_mode,
                               int k_node,
                               int k_chip,
                               double frequency_ghz,
                               const std::string &result_path)
{
    if (endpoints.empty())
        throw std::runtime_error("Omelet online controller has no endpoints");
    if (!(frequency_ghz > 0.0) || !std::isfinite(frequency_ghz))
        throw std::runtime_error("invalid Omelet frequency");
    if (endpoint_mode != "physical" && endpoint_mode != "cnode")
        throw std::runtime_error("unsupported Omelet endpoint mode: " + endpoint_mode);

    std::ifstream manifestStream(manifest);
    if (!manifestStream)
        throw std::runtime_error("cannot open Omelet manifest: " + manifest);
    std::stringstream manifestBuffer;
    manifestBuffer << manifestStream.rdbuf();
    const Json root = JsonParser(manifestBuffer.str()).parse();
    const std::string base = parentDir(manifest);
    const int numInputs = intField(root, "num_inputs");
    const int numResources = intField(root, "num_resources");
    if (numInputs <= 0 || numResources <= 0)
        throw std::runtime_error("manifest has invalid input/resource count");

    std::unordered_map<int, Group> groups;
    for (const Json &item : field(root, "template_groups").array) {
        Group group;
        group.id = intField(item, "group_id");
        group.path = joinPath(base, stringField(item, "bz2_path"));
        group.sharedPolicy = item.object.count("shared_prefix_policy") ? intField(item, "shared_prefix_policy") : 0;
        if (group.sharedPolicy != 0)
            throw std::runtime_error("Omelet C++ controller does not support shared-prefix groups yet");
        if (!groups.emplace(group.id, group).second)
            throw std::runtime_error("duplicate template group");
    }

    std::vector<Phase> phases;
    std::unordered_map<int, int> phaseIndex;
    for (const Json &item : field(root, "phases").array) {
        Phase phase;
        phase.id = intField(item, "phase_id");
        phase.input = intField(item, "input_id");
        phase.layer = intField(item, "layer_id");
        phase.computeCycles = uintField(item, "compute_latency_cycles");
        phase.deps = intArray(item, "dep_phase_ids");
        phase.resources = intArray(item, "resource_ids");
        phase.group = item.object.count("group_id") ? intField(item, "group_id") : -1;
        phase.remainingDeps = static_cast<int>(phase.deps.size());
        if (phase.input < 0 || phase.input >= numInputs || phase.computeCycles == 0)
            throw std::runtime_error("invalid phase input or compute latency");
        if (phase.group >= 0 && !groups.count(phase.group))
            throw std::runtime_error("phase references missing group");
        for (const int resource : phase.resources)
            if (resource < 0 || resource >= numResources)
                throw std::runtime_error("phase resource out of range");
        if (!phaseIndex.emplace(phase.id, static_cast<int>(phases.size())).second)
            throw std::runtime_error("duplicate phase id");
        phases.push_back(phase);
    }
    std::vector<std::vector<int>> dependents(phases.size());
    for (size_t i = 0; i < phases.size(); ++i) {
        for (const int dep : phases[i].deps) {
            auto it = phaseIndex.find(dep);
            if (it == phaseIndex.end())
                throw std::runtime_error("phase dependency not found");
            dependents[it->second].push_back(static_cast<int>(i));
        }
    }

    std::unordered_map<int, int> byPhysical;
    std::unordered_map<int, int> byCnode;
    std::unordered_map<int, int> virtualPhysical;
    std::unordered_map<int, CatalogLayer> catalogLayers;
    if (!catalog.empty()) {
        std::ifstream catalogStream(catalog);
        if (!catalogStream)
            throw std::runtime_error("cannot open Omelet endpoint catalog: " + catalog);
        std::stringstream catalogBuffer;
        catalogBuffer << catalogStream.rdbuf();
        const Json catalogRoot = JsonParser(catalogBuffer.str()).parse();
        for (const Json &tile : field(catalogRoot, "virtual_tiles").array)
            virtualPhysical.emplace(intField(tile, "virtual_id"), intField(tile, "physical_id"));
        for (const Json &item : field(catalogRoot, "layers").array) {
            CatalogLayer layer;
            layer.block = intField(item, "block_id");
            layer.virtualIds = intArray(item, "virtual_ids");
            layer.canonical = intArray(item, "canonical");
            const Json &candidateList = field(item, "candidates");
            if (candidateList.type != Json::Array)
                throw std::runtime_error("catalog candidates must be an array");
            for (const Json &candidate : candidateList.array) {
                if (candidate.type != Json::Array)
                    throw std::runtime_error("catalog candidate must be an array");
                std::vector<int> values;
                for (const Json &value : candidate.array) {
                    if (value.type != Json::Number)
                        throw std::runtime_error("catalog candidate endpoint must be an integer");
                    values.push_back(static_cast<int>(value.number));
                }
                layer.candidates.push_back(std::move(values));
            }
            if (layer.virtualIds.size() != layer.canonical.size())
                throw std::runtime_error("catalog layer virtual/canonical size mismatch");
            catalogLayers.emplace(intField(item, "layer_id"), std::move(layer));
        }
    }
    Json nonuniformRoot;
    if (!nonuniform_grid.empty()) {
        std::ifstream gridStream(nonuniform_grid);
        if (!gridStream)
            throw std::runtime_error("cannot open nonuniform grid: " + nonuniform_grid);
        std::stringstream gridBuffer;
        gridBuffer << gridStream.rdbuf();
        nonuniformRoot = JsonParser(gridBuffer.str()).parse();
    }
    {
        std::ifstream input(position);
        if (!input)
            throw std::runtime_error("cannot open Omelet position file: " + position);
        int node, x, y, pyChip, nocChip, cnode;
        std::string kind;
        std::string line;
        while (std::getline(input, line)) {
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#')
                continue;
            std::istringstream row(line.substr(first));
            if (!(row >> node >> x >> y >> pyChip >> nocChip >> kind >> cnode))
                throw std::runtime_error("malformed Omelet position row: " + line);
            if (node < 0 || cnode < 0)
                throw std::runtime_error("negative node in position file");
            if (nocChip < 0)
                continue;
            int router = node;
            if (nonuniform_grid.empty()) {
                if (k_node <= 0 || k_chip <= 0)
                    throw std::runtime_error("invalid uniform Omelet grid dimensions");
                const int chipX = x / k_node;
                const int chipY = y / k_node;
                router = (chipY * k_chip + chipX) * k_node * k_node +
                         (y % k_node) * k_node + (x % k_node);
            } else {
                const Json &chips = field(nonuniformRoot, "chiplets");
                const auto chipIt = chips.object.find(std::to_string(nocChip));
                if (chipIt == chips.object.end())
                    throw std::runtime_error("nonuniform chiplet is missing from grid");
                const Json &chip = chipIt->second;
                const auto origin = intArray(chip, "origin");
                const auto shape = intArray(chip, "grid");
                const int baseId = intField(chip, "node_id_base");
                if (origin.size() != 2 || shape.size() != 2 || shape[0] <= 0 || shape[1] <= 0)
                    throw std::runtime_error("invalid nonuniform grid entry");
                const int localX = x - origin[0];
                const int localY = y - origin[1];
                if (localX < 0 || localY < 0 || localX >= shape[0] || localY >= shape[1])
                    throw std::runtime_error("position is outside nonuniform chiplet grid");
                router = baseId + localY * shape[0] + localX;
            }
            byPhysical.emplace(node, router);
            byCnode.emplace(cnode, router);
        }
    }
    if (byPhysical.empty())
        throw std::runtime_error("Omelet position file has no nodes");

    std::unordered_map<uint64_t, int> packetRun;
    std::unordered_map<int, int> phaseCandidate;
    std::vector<int> resourceOwner(numResources, -1);
    std::vector<uint64_t> resourceRelease(numResources, 0);
    std::unordered_map<int, int> runRemaining;
    std::unordered_map<int, int> runPhase;
    std::unordered_map<int, size_t> runIndex;
    std::vector<Run> runs;
    std::vector<int> inputRemaining(numInputs, 0);
    std::vector<uint64_t> inputFirst(numInputs, 0);
    std::vector<uint64_t> inputDone(numInputs, 0);
    uint64_t tick = 0;
    uint64_t nextPacketId = 1;
    int nextRunId = 0;
    uint64_t arrived = 0;
    double latencySum = 0.0;
    std::unordered_map<uint64_t, uint64_t> injectedAt;

    auto routerFor = [&](int input, int phaseIndex, int endpoint) -> int {
        (void)input;
        const int layer = phases[phaseIndex].layer;
        auto layerIt = catalogLayers.find(layer);
        if (layerIt != catalogLayers.end()) {
            const CatalogLayer &entry = layerIt->second;
            auto virtualIt = std::find(entry.virtualIds.begin(), entry.virtualIds.end(), endpoint);
            if (virtualIt != entry.virtualIds.end()) {
                const size_t index = static_cast<size_t>(virtualIt - entry.virtualIds.begin());
                const int candidate = phaseCandidate.count(phaseIndex) ? phaseCandidate.at(phaseIndex) : -1;
                if (candidate >= 0 && static_cast<size_t>(candidate) < entry.candidates.size() &&
                    index < entry.candidates[candidate].size())
                    endpoint = entry.candidates[candidate][index];
                else
                    endpoint = entry.canonical[index];
            } else if (virtualPhysical.count(endpoint)) {
                endpoint = virtualPhysical.at(endpoint);
            }
        } else if (virtualPhysical.count(endpoint)) {
            endpoint = virtualPhysical.at(endpoint);
        }
        auto it = endpoint_mode == "cnode" ? byCnode.find(endpoint) : byPhysical.find(endpoint);
        if (it == (endpoint_mode == "cnode" ? byCnode.end() : byPhysical.end()))
            throw std::runtime_error("endpoint missing from position map");
        if (it->second < 0 || static_cast<size_t>(it->second) >= endpoints.size())
            throw std::runtime_error("endpoint router exceeds gem5 endpoint count");
        return it->second;
    };

    for (const Phase &phase : phases)
        ++inputRemaining[phase.input];

    auto markDone = [&](int index, uint64_t doneTick) {
        Phase &phase = phases[index];
        if (phase.status == 3)
            return;
        phase.status = 3;
        phase.commEnd = std::max(doneTick, phase.computeEnd);
        for (const int resource : phase.resources) {
            if (resourceOwner[resource] == phase.id)
                resourceOwner[resource] = -1;
            resourceRelease[resource] = std::max(resourceRelease[resource], phase.commEnd);
        }
        for (const int dependent : dependents[index])
            --phases[dependent].remainingDeps;
        if (--inputRemaining[phase.input] == 0 && inputDone[phase.input] == 0)
            inputDone[phase.input] = phase.commEnd;
    };

    while (true) {
        for (const auto &arrival : OnlinePacketRegistry::drainArrivalRecords()) {
            auto runIt = packetRun.find(arrival.id);
            if (runIt == packetRun.end())
                continue;
            const int run = runIt->second;
            const auto injectedIt = injectedAt.find(arrival.id);
            if (injectedIt == injectedAt.end())
                throw std::runtime_error("arrival has no injection timestamp");
            const uint64_t injectedTick = injectedIt->second;
            injectedAt.erase(injectedIt);
            packetRun.erase(runIt);
            --runRemaining[run];
            ++arrived;
            latencySum += static_cast<double>(arrival.tick - injectedTick) * frequency_ghz / 1000.0;
            if (runRemaining[run] == 0) {
                const int phase = runPhase.at(run);
                phases[phase].commEnd = arrival.tick;
                runs.at(runIndex.at(run)).finishTick = arrival.tick;
                markDone(phase, arrival.tick);
            }
        }

        for (size_t i = 0; i < phases.size(); ++i) {
            Phase &phase = phases[i];
            if (phase.status != 1 || phase.computeEnd > tick)
                continue;
            for (const int resource : phase.resources) {
                resourceRelease[resource] = std::max(resourceRelease[resource], phase.computeEnd);
            }
            phase.commStart = tick;
            if (phase.group < 0) {
                markDone(static_cast<int>(i), tick);
                continue;
            }
            phase.status = 2;
            const Group &group = groups.at(phase.group);
            TraceReader reader(group.path);
            PacketTemplate packet;
            const int run = nextRunId++;
            runPhase[run] = static_cast<int>(i);
            runRemaining[run] = 0;
            runIndex[run] = runs.size();
            runs.push_back(Run{run, static_cast<int>(i), phase.group, phase.input, tick, 0, 0, 0});
            if (inputFirst[phase.input] == 0)
                inputFirst[phase.input] = tick;
            while (reader.next(&packet)) {
                const int source = routerFor(phase.input, static_cast<int>(i), packet.src);
                const int destination = routerFor(phase.input, static_cast<int>(i), packet.dst);
                if (source == destination)
                    continue;
                const uint64_t id = nextPacketId++;
                endpoints[source]->sendOnlinePacket(destination, packet.bytes, id);
                packetRun[id] = run;
                injectedAt[id] = tick;
                ++runRemaining[run];
                ++runs.back().packets;
                ++runs.back().physicalPackets;
            }
            if (runRemaining[run] == 0) {
                runs.back().finishTick = tick;
                markDone(static_cast<int>(i), tick);
            }
        }

        uint64_t nextEvent = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < phases.size(); ++i) {
            Phase &phase = phases[i];
            if (phase.status == 1) {
                nextEvent = std::min(nextEvent, phase.computeEnd);
            } else if (phase.status == 0 && phase.remainingDeps == 0) {
                bool available = true;
                const auto catalogIt = catalogLayers.find(phase.layer);
                if (catalogIt != catalogLayers.end() && catalogIt->second.block >= 0) {
                    int selected = -1;
                    for (size_t candidate = 0; candidate < catalogIt->second.candidates.size(); ++candidate) {
                        bool candidateAvailable = true;
                        for (const int resource : catalogIt->second.candidates[candidate]) {
                            if (resource < 0 || resource >= numResources ||
                                resourceOwner[resource] != -1 || resourceRelease[resource] > tick)
                                candidateAvailable = false;
                        }
                        if (candidateAvailable) {
                            selected = static_cast<int>(candidate);
                            break;
                        }
                    }
                    if (selected < 0) {
                        available = false;
                    } else {
                        phaseCandidate[static_cast<int>(i)] = selected;
                        phase.resources = catalogIt->second.candidates[static_cast<size_t>(selected)];
                    }
                }
                for (const int resource : phase.resources)
                    available &= resourceOwner[resource] == -1 && resourceRelease[resource] <= tick;
                if (available) {
                    phase.status = 1;
                    phase.computeStart = tick;
                    phase.computeEnd = tick + static_cast<uint64_t>(std::ceil(
                        phase.computeCycles * 1000.0 / frequency_ghz));
                    for (const int resource : phase.resources)
                        resourceOwner[resource] = phase.id;
                    nextEvent = std::min(nextEvent, phase.computeEnd);
                }
            }
        }

        bool allDone = true;
        for (const Phase &phase : phases)
            allDone &= phase.status == 3;
        if (allDone && packetRun.empty())
            break;
        if (nextEvent == std::numeric_limits<uint64_t>::max()) {
            if (packetRun.empty())
                throw std::runtime_error("Omelet workload stalled: dependencies or resources");
            nextEvent = tick + 1000000000ULL;
        }
        if (nextEvent <= tick)
            throw std::runtime_error("Omelet scheduler selected a non-future compute event");
        simulate(nextEvent - tick);
        tick = curTick();
    }

    auto writeIntList = [](std::ostream &out, const std::vector<int> &values) {
        out << '[';
        for (size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << values[i]; }
        out << ']';
    };
    std::ofstream output(result_path, std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot create Omelet result: " + result_path);
    uint64_t injected = 0;
    for (const Run &record : runs)
        injected += record.packets;
    output << "{\"mode\":\"online\",\"backend\":\"omelet\"," 
           << "\"final_tick\":" << tick << ','
           << "\"final_cycle\":" << static_cast<uint64_t>(std::ceil(tick * frequency_ghz / 1000.0)) << ','
           << "\"cycle_frequency_ghz\":" << frequency_ghz << ','
           << "\"time_base\":\"gem5_tick_1ps\","
           << "\"message_injected\":" << injected << ','
           << "\"message_arrived\":" << arrived << ','
           << "\"average_latency\":" << (arrived == 0 ? 0.0 : latencySum / arrived) << ',';
    output << "\"inputs\":[";
    for (int input = 0; input < numInputs; ++input) {
        if (input) output << ',';
        output << "{\"input_id\":" << input
               << ",\"first_injection_cycle\":" << static_cast<uint64_t>(std::ceil(inputFirst[input] * frequency_ghz / 1000.0))
               << ",\"all_comm_done_cycle\":" << static_cast<uint64_t>(std::ceil(inputDone[input] * frequency_ghz / 1000.0))
               << ",\"input_done_cycle\":" << static_cast<uint64_t>(std::ceil(inputDone[input] * frequency_ghz / 1000.0)) << "}";
    }
    output << "],\"groups\":[";
    for (size_t index = 0; index < runs.size(); ++index) {
        const Run &record = runs[index];
        if (index) output << ',';
        output << "{\"run_id\":" << record.id
               << ",\"group_id\":" << record.group
               << ",\"phase_id\":" << phases[record.phase].id
               << ",\"input_id\":" << record.input
               << ",\"inject_cycle\":" << static_cast<uint64_t>(std::ceil(record.injectTick * frequency_ghz / 1000.0))
               << ",\"finish_cycle\":" << static_cast<uint64_t>(std::ceil(record.finishTick * frequency_ghz / 1000.0))
               << ",\"packet_count\":" << record.packets
               << ",\"physical_packet_count\":" << record.physicalPackets
               << ",\"shared_follower_count\":0,\"boundary_fanout_packet_count\":0}";
    }
    output << "],\"phases\":[";
    for (size_t index = 0; index < phases.size(); ++index) {
        const Phase &phase = phases[index];
        if (index) output << ',';
        output << "{\"phase_id\":" << phase.id
               << ",\"input_id\":" << phase.input
               << ",\"layer_id\":" << phase.layer
               << ",\"dep_phase_ids\":";
        writeIntList(output, phase.deps);
        output << ",\"resource_ids\":";
        writeIntList(output, phase.resources);
        output << ",\"compute_latency_cycles\":" << phase.computeCycles
               << ",\"compute_start_cycle\":" << static_cast<uint64_t>(std::ceil(phase.computeStart * frequency_ghz / 1000.0))
               << ",\"compute_end_cycle\":" << static_cast<uint64_t>(std::ceil(phase.computeEnd * frequency_ghz / 1000.0))
               << ",\"comm_start_cycle\":" << static_cast<uint64_t>(std::ceil(phase.commStart * frequency_ghz / 1000.0))
               << ",\"comm_end_cycle\":" << static_cast<uint64_t>(std::ceil(phase.commEnd * frequency_ghz / 1000.0))
               << ",\"state_final\":\"done\",\"has_group\":" << (phase.group >= 0 ? "true" : "false") << '}';
    }
    output << "]}\n";
    output.close();
    return result_path;
}

} // namespace gem5
