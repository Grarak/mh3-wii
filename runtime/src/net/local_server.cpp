// SPDX-License-Identifier: AGPL-3.0-or-later
// The embedded multiplayer server (local_server.h). A port of MH3SP's handlers
// (mh/pat.py, fmp_server.py), its layers (mh/database.py, mh/session.py) and
// its binaries (mh/constants.py), Copyright (C) 2021-2025 MH3SP Server Project,
// and of what the account server answers.
//
// PAT packets: u16 size, u16 sequence, u32 id, then the data; big endian. A
// client logs in on OPN (terms, then LMP's address), LMP (ticket, Capcom ID,
// the server list), then moves through FMP: a server holds gates, a gate
// cities, a city up to four hunters and their quest circles. Hunters reconnect
// at each step and find their session by ticket. Everything runs on the
// server's own thread.
#include "local_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace LocalServer {
namespace {
#include "local_server_ids.inc"

using Clock = std::chrono::steady_clock;
using Bytes = std::string;

constexpr uint8_t kItemByte = 1, kItemWord = 2, kItemLong = 3, kItemLongLong = 4, kItemString = 5, kItemBinary = 6;
constexpr uint32_t kGates = 2, kCities = 8, kCityPlayers = 4, kCirclePlayers = 4;
constexpr uint32_t kTicksPerCycle = 23040;
constexpr size_t kHunterBytes = 0x100;

// Layer states.
constexpr uint8_t kEmpty = 1, kFull = 2, kJoinable = 3;

// What a login's NtcLogin says.
constexpr uint8_t kLoginReady = 5, kLoginFmp = 3, kLoginLmp = 1, kLoginLmpTicket = 2;

void U16(Bytes& out, uint32_t value) {
    out += char(value >> 8);
    out += char(value);
}

void U32(Bytes& out, uint32_t value) {
    U16(out, value >> 16);
    U16(out, value & 0xFFFF);
}

void Lp2(Bytes& out, const Bytes& text) {
    U16(out, uint32_t(text.size()));
    out += text;
}

// Items of a PAT structure: a count, then field id and typed value each.
class Fields {
  public:
    void Byte(uint8_t id, uint8_t value) {
        Add(id, kItemByte);
        body += char(value);
    }

    void Word(uint8_t id, uint16_t value) {
        Add(id, kItemWord);
        U16(body, value);
    }

    void Long(uint8_t id, uint32_t value) {
        Add(id, kItemLong);
        U32(body, value);
    }

    void LongLong(uint8_t id, uint32_t value) {
        Add(id, kItemLongLong);
        U32(body, 0);
        U32(body, value);
    }

    void String(uint8_t id, const Bytes& value) {
        Add(id, kItemString);
        Lp2(body, value);
    }

    void Binary(uint8_t id, const Bytes& value) {
        Add(id, kItemBinary);
        Lp2(body, value);
    }

    Bytes Pack() const { return Bytes(1, char(count)) + body; }

  private:
    void Add(uint8_t id, uint8_t type) {
        ++count;
        body += char(id);
        body += char(type);
    }

    uint8_t count = 0;
    Bytes body;
};

// Reads big-endian values; past the end they read 0 and `failed` is set.
class Reader {
  public:
    explicit Reader(const Bytes& bytes, size_t start = 0) : data(bytes), at(start) {}

    uint32_t U8() { return Take(1) ? uint8_t(data[at - 1]) : 0; }

    uint32_t U16() { return Take(2) ? uint32_t(uint8_t(data[at - 2])) << 8 | uint8_t(data[at - 1]) : 0; }

    uint32_t U32() {
        const uint32_t high = U16();
        return high << 16 | U16();
    }

    Bytes Text(size_t size) { return Take(size) ? data.substr(at - size, size) : Bytes(); }

    Bytes Lp2() { return Text(U16()); }

    Bytes Rest() { return at < data.size() ? data.substr(at) : Bytes(); }

    size_t Offset() const { return at; }

    bool failed = false;

  private:
    bool Take(size_t size) {
        if (at + size > data.size()) {
            failed = true;
            return false;
        }

        at += size;
        return true;
    }

    const Bytes& data;
    size_t at;
};

// A PAT structure as received.
struct Item {
    uint8_t type = 0;
    uint32_t number = 0;
    Bytes text;
};

using Items = std::map<uint8_t, Item>;

Items ReadItems(Reader& in) {
    Items items;
    const uint32_t count = in.U8();

    for (uint32_t i = 0; i < count && !in.failed; ++i) {
        const uint8_t id = uint8_t(in.U8());
        Item item;
        item.type = uint8_t(in.U8());

        switch (item.type) {
        case kItemByte:
            item.number = in.U8();
            break;
        case kItemWord:
            item.number = in.U16();
            break;
        case kItemLong:
            item.number = in.U32();
            break;
        case kItemLongLong:
            in.U32();
            item.number = in.U32();
            break;
        case kItemString:
        case kItemBinary:
            item.text = in.Lp2();
            break;
        default:
            in.failed = true;
        }

        items[id] = std::move(item);
    }

    return items;
}

// A list of optional fields: id, whether it has a value, the value.
using Optional = std::vector<std::pair<uint8_t, std::optional<uint32_t>>>;

Optional ReadOptional(Reader& in) {
    Optional fields;
    const uint32_t count = in.U8();

    for (uint32_t i = 0; i < count && !in.failed; ++i) {
        const uint8_t id = uint8_t(in.U8());
        std::optional<uint32_t> value;

        if (in.U8())
            value = in.U32();

        fields.push_back({id, value});
    }

    return fields;
}

Bytes PackOptional(const Optional& fields) {
    Bytes out(1, char(fields.size()));

    for (const auto& [id, value] : fields) {
        out += char(id);
        out += char(value ? 1 : 0);

        if (value)
            U32(out, *value);
    }

    return out;
}

Bytes Padded(const Bytes& text, size_t size) {
    Bytes out = text.substr(0, size);
    out.resize(size, '\0');
    return out;
}

Bytes RandomText(size_t size) {
    static std::mt19937 random{std::random_device{}()};
    static constexpr char kCharset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    Bytes text;

    for (size_t i = 0; i < size; ++i)
        text += kCharset[random() % (sizeof(kCharset) - 1)];

    return text;
}

uint32_t Now() {
    return uint32_t(std::time(nullptr));
}

struct Client;

struct Session {
    Client* client = nullptr;
    Bytes ticket, supportCode, capcomId, hunterName;
    std::array<uint8_t, kHunterBytes> hunter{};
    bool keep = false; // survives its connection: the hunter is on the way to the next server
    uint32_t layer = 0, serverId = 0, gateId = 0, cityId = 0;
    int circleId = -1;
    bool standby = false, inQuest = false;
};

// Players in slots, as many as the capacity.
class Players {
  public:
    explicit Players(size_t capacity = 0) : slots(capacity, nullptr) {}

    size_t Count() const { return size_t(std::count_if(slots.begin(), slots.end(), [](Session* s) { return s != nullptr; })); }

    size_t Capacity() const { return slots.size(); }

    int Index(const Session* session) const {
        const auto found = std::find(slots.begin(), slots.end(), session);
        return found == slots.end() ? -1 : int(found - slots.begin());
    }

    int Add(Session* session) {
        if (const int index = Index(session); index >= 0)
            return index;

        const auto free = std::find(slots.begin(), slots.end(), nullptr);

        if (free == slots.end())
            return -1;

        *free = session;
        return int(free - slots.begin());
    }

    void Remove(const Session* session) {
        if (const int index = Index(session); index >= 0)
            slots[size_t(index)] = nullptr;
    }

    Session* Find(const Bytes& capcomId) const {
        for (Session* s : slots)
            if (s && s->capcomId == capcomId)
                return s;

        return nullptr;
    }

    std::vector<Session*> slots;
};

struct Circle {
    Session* leader = nullptr;
    Players players{kCirclePlayers};
    bool departed = false;
    uint32_t questId = 0, unknown0e = 0;
    std::optional<Bytes> password, remarks;

    bool Empty() const { return !leader; }
};

struct City {
    Bytes name;
    Players players{kCityPlayers};
    Optional optional;
    Session* leader = nullptr;
    bool reserved = false;
    std::array<Circle, kCityPlayers> circles{};

    uint8_t State() const {
        if (reserved)
            return kFull;

        const size_t size = players.Count();
        return size == 0 ? kEmpty : size < players.Capacity() ? kJoinable : kFull;
    }

    uint32_t InQuest() const {
        return uint32_t(std::count_if(players.slots.begin(), players.slots.end(), [](Session* s) { return s && s->inQuest; }));
    }
};

struct Gate {
    Bytes name;
    Players players{100};
    std::array<City, kCities> cities{};

    uint32_t Population() const {
        uint32_t count = uint32_t(players.Count());

        for (const auto& city : cities)
            count += uint32_t(city.players.Count());

        return count;
    }

    uint8_t State() const {
        const uint32_t size = Population();
        return size == 0 ? kEmpty : size < players.Capacity() ? kJoinable : kFull;
    }
};

struct World {
    Bytes name = "Ad hoc";
    Players players{100};
    std::array<Gate, kGates> gates{};

    uint32_t Population() const {
        uint32_t count = uint32_t(players.Count());

        for (const auto& gate : gates)
            count += gate.Population();

        return count;
    }
};

struct State {
    Bytes address;
    World world;
    std::list<Session> sessions;
    std::map<Bytes, std::array<Bytes, 6>> consoles; // support code -> Capcom IDs
    std::map<Bytes, Bytes> names;                   // Capcom ID -> hunter
    std::list<Client> clients;
};

constexpr const char* kBlankCapcomId = "******";

void Send(Client& client, uint32_t id, const Bytes& data, uint32_t sequence);

// One connection.
struct Client {
    std::unique_ptr<Connection> connection;
    uint16_t port = 0;
    Session* session = nullptr;
    Bytes input, output;
    Clock::time_point opened = Clock::now(), pinged = Clock::now();
    bool requested = false, closing = false, ended = false;
};

State* state = nullptr;

const bool trace = std::getenv("MH3_LOCAL_SERVER_TRACE") != nullptr;

void Dump(const char* direction, uint32_t id, uint32_t port, const Bytes& data) {
    if (!trace)
        return;

    std::fprintf(stderr, "PAT %u %s %08X", port, direction, id);

    for (size_t i = 0; i < data.size() && i < 48; ++i)
        std::fprintf(stderr, " %02x", uint8_t(data[i]));

    std::fprintf(stderr, "\n");
}

void Send(Client& client, uint32_t id, const Bytes& data, uint32_t sequence) {
    Dump(">", id, client.port, data);
    U16(client.output, uint32_t(data.size()));
    U16(client.output, sequence);
    U32(client.output, id);
    client.output += data;
}

void Broadcast(const Players& players, uint32_t id, const Bytes& data, uint32_t sequence, const Session* except) {
    for (const Session* s : players.slots)
        if (s && s != except && s->client)
            Send(*s->client, id, data, sequence);
}

City& CityOf(const Session& s) {
    return state->world.gates[s.gateId - 1].cities[s.cityId - 1];
}

Gate& GateOf(const Session& s) {
    return state->world.gates[s.gateId - 1];
}

const Players& LayerPlayers(const Session& s) {
    return s.layer == 2 ? CityOf(s).players : s.layer == 1 ? GateOf(s).players : state->world.players;
}

Bytes LayerPath(uint32_t gate, uint32_t city) {
    Bytes out;
    U32(out, city ? 3 : gate ? 2 : 1);
    U32(out, 1); // the server
    U16(out, 1);
    U16(out, gate);
    U16(out, city);
    return out;
}

// LayerData.create_from: The fields of a layer in the order of their ids.
Bytes PackLayer(uint32_t index, const Bytes& name, uint32_t size, uint32_t capacity, uint8_t layerState, uint8_t depth, const Bytes* path, const uint32_t* childPopulation, const Bytes* pathname) {
    Fields fields;
    fields.Long(0x01, index);

    if (path)
        fields.Binary(0x02, *path);

    fields.String(0x03, name);
    fields.Word(0x05, uint16_t(index));
    fields.Long(0x06, size);
    fields.Long(0x07, size);
    fields.Long(0x09, capacity);

    if (childPopulation) {
        fields.Long(0x0B, *childPopulation);
        fields.Long(0x0C, 0xC);
    }

    fields.Byte(0x10, layerState);
    fields.Long(0x11, 500);
    fields.Byte(0x12, 1);
    fields.Byte(0x15, depth);

    if (pathname)
        fields.String(0x16, *pathname);

    return fields.Pack();
}

Bytes GatePack(uint32_t index, const Gate& gate) {
    return PackLayer(index, gate.name, gate.Population(), uint32_t(gate.players.Capacity()), gate.State(), 2, nullptr, nullptr, nullptr);
}

Bytes CityPack(uint32_t gateIndex, uint32_t index, const City& city, bool withPath) {
    const uint32_t inQuest = city.InQuest();
    const Bytes path = LayerPath(gateIndex, index);
    const Bytes pathname = state->world.name + "\t" + state->world.gates[gateIndex - 1].name + "\t" + city.name;
    return PackLayer(index, city.name, uint32_t(city.players.Count()), uint32_t(city.players.Capacity()), city.State(), 3, withPath ? &path : nullptr, &inQuest, &pathname);
}

Bytes DummyLayer() {
    Fields fields;
    fields.Word(0x05, 1);
    fields.String(0x03, "LayerStart");
    fields.Long(0x06, 0);
    fields.Long(0x09, 4);
    fields.Byte(0x10, 1);
    fields.Long(0x11, 500);
    fields.Byte(0x12, 1);
    return fields.Pack();
}

Bytes LayerUser(const Session& s) {
    Fields user;
    user.String(0x01, s.capcomId);
    user.String(0x02, s.hunterName);
    user.Binary(0x07, Bytes(reinterpret_cast<const char*>(s.hunter.data()), kHunterBytes));
    return user.Pack();
}

Bytes BinaryInfo(const Session& s, bool time) {
    Fields info;

    if (time)
        info.Long(0x01, 0x12345678);

    info.String(0x02, s.capcomId);
    info.String(0x03, s.hunterName);
    return info.Pack();
}

// CircleInfo.pack_from, with its optional fields.
Bytes CirclePack(const Circle& circle, uint32_t index) {
    Fields info;
    info.Long(0x01, index);

    if (!circle.Empty()) {
        if (circle.password) {
            info.Byte(0x03, 1);
            info.String(0x04, *circle.password);
        }

        Bytes members(0x88, '\0');

        for (size_t i = 0; i < circle.players.slots.size() && i < 4; ++i)
            if (const Session* player = circle.players.slots[i]) {
                std::copy_n(Padded(player->capcomId, 0x10).begin(), 0x10, members.begin() + long(i * 0x10));
                std::copy_n(Padded(player->hunterName, 0x10).begin(), 0x10, members.begin() + long(0x40 + i * 0x10));
                members[0x80 + i] = char(player->hunter[5]); // the weapon's type
                members[0x84 + i] = char(0xFF);
            }

        info.Binary(0x05, members);

        if (circle.remarks)
            info.String(0x06, *circle.remarks);

        info.Long(0x07, uint32_t(circle.players.Count()));
        info.Long(0x09, uint32_t(circle.players.Capacity()));
        info.Long(0x0C, index);
        info.String(0x0D, circle.leader->capcomId);
        info.Byte(0x0E, uint8_t(circle.unknown0e));
        info.Byte(0x0F, circle.departed || circle.players.Count() == circle.players.Capacity());
    }

    return info.Pack() + PackOptional({{1, uint32_t(circle.players.Capacity())}, {2, circle.questId}});
}

Bytes ServerTypes() {
    struct Type {
        const char* name;
        const char* text;
        uint16_t lowest, highest;
    };

    static constexpr Type kTypes[]{
        {"Open", "Hunters of all Ranks\ncan gather here.", 0, 999},
        {"Rookie", "Only hunters HR 30\nor lower may enter.", 0, 30},
        {"Expert", "Only hunters HR 31\nor higher may enter.", 31, 999},
        {"Recruiting", "Hunters in search of\nhunting companions\ncan gather here.", 0, 999},
    };

    Bytes data;

    for (const auto& type : kTypes) {
        data += Padded(type.name, 24);
        data += Padded(type.text, 168);
    }

    U32(data, 0);
    U32(data, Now());
    U32(data, kTicksPerCycle);

    static constexpr const char* kSeekings[]{"Everyone welcome!", "Casual play", "Huntin' and chattin'", "Beginners welcome!", "Skilled players only", "Let's Quest together", "Event Quest ho!", "Arena battles!",
                                             "Earning money", "HR grinding", "Gathering materials", "Rare material search", "Just a Quest or two", "In for the long haul", "Playing with friends", "Solo play"};

    for (uint32_t i = 0; i < 32; ++i) {
        data += Padded(i < std::size(kSeekings) ? kSeekings[i] : "", 48);
        data += Bytes("\0\1\xFF\0", 4);
    }

    struct Limit {
        const char* name;
        uint16_t lowest, highest;
    };

    static constexpr Limit kLimits[]{{"None", 0, 999}, {"Low", 0, 30}, {"High", 31, 999}, {"", 0, 999}, {"", 0, 999}, {"", 0, 999}, {"", 0, 999}, {"", 0, 999}};

    for (const auto& limit : kLimits) {
        data += Padded(limit.name, 30);
        data += Bytes("\1\0", 2);
        U16(data, limit.lowest);
        U16(data, limit.highest);
    }

    static constexpr const char* kGoals[]{"None", "HR1~", "HR9~", "HR18~", "HR31~", "HR40~", "HR51~", "1* Urgent Quest", "2* Urgent Quest", "3* Urgent Quest", "4* Urgent Quest", "5* Urgent Quest",
                                          "Great Jaggi", "Qurupeco", "Royal Ludroth", "Barroth", "Gobul", "Rathian", "Great Baggi", "Gigginox", "Lagiacrus", "Barioth", "Rathalos", "Diablos",
                                          "Uragaan", "Agnaktor", "Jhen Mohran", "Deviljho", "Alatreon", "Mining Ore", "Catching Bugs", "Fishing", "Small Monsters", "Gathering", "Transporting", "The Arena"};

    for (uint32_t i = 0; i < 64; ++i) {
        data += Padded(i < std::size(kGoals) ? kGoals[i] : "", 30);
        data += Bytes("\1\0", 2);
        U16(data, 0);
        U16(data, 0);
    }

    for (const auto& type : kTypes) {
        U16(data, type.lowest);
        U16(data, type.highest);
    }

    for (uint32_t i = 0; i < 8; ++i)
        U16(data, 30 * 30);

    return data;
}

// The City's greeters: shops, the Trading Post, the quests and the Guildmaster.
Bytes Greeters() {
    constexpr size_t kSize = 0x180;
    Bytes data;

    for (const char* text : {"Prices are normal.", "Prices are normal.", "The Trading Post is open!", "", "", "Good hunting!"})
        data += Padded(text, kSize);

    return data;
}

// Binaries come in one set per language, 15 each; of a set's first five the
// server type list and the events matter, and no event quests are on offer.
Bytes BinaryContent(uint32_t type) {
    const uint32_t base = (type - 1) % 15 + 1;

    switch (base) {
    case 1:
        return ServerTypes();
    case 2: {
        // Fog, sandstorm and its end: none soon.
        Bytes data;
        const uint32_t far = Now() + 3650u * 86400u;
        U32(data, far);
        U32(data, far);
        U32(data, far);
        return data;
    }
    case 3:
        return Greeters();
    case 5:
        return Bytes(0x10, '5');
    default:
        return {};
    }
}

void LeaveCircle(Client& client, uint32_t sequence);

void CircleListChange(const Session& s, uint32_t index, uint32_t sequence) {
    Bytes data;
    U32(data, index);
    data += CirclePack(CityOf(s).circles[index - 1], index);
    Broadcast(LayerPlayers(s), kNtcCircleListLayerChange, data, sequence, nullptr);
}

void NotifyCityInfo(const Session& s, uint32_t cityId) {
    const City& city = GateOf(s).cities[cityId - 1];
    Bytes data;
    Lp2(data, LayerPath(s.gateId, cityId));
    data += CityPack(s.gateId, cityId, city, true);
    data += PackOptional(city.optional);
    Broadcast(GateOf(s).players, kNtcLayerInfoSet, data, 0, &s);
}

void NotifyCityNumber(const Session& s, uint32_t cityId) {
    const City& city = GateOf(s).cities[cityId - 1];
    Fields number;
    number.Binary(0x01, LayerPath(s.gateId, cityId));
    number.Long(0x02, uint32_t(city.players.Count()));
    number.Long(0x03, cityId);
    number.Long(0x04, 0xAF00FA00);
    number.Long(0x05, uint32_t(city.players.Capacity()));
    number.Long(0x06, 0xBF00FB00);
    number.Long(0x07, city.InQuest());
    Bytes data(1, char(4));
    data += number.Pack();
    Broadcast(GateOf(s).players, kNtcLayerUserNum, data, 0, &s);
}

void LeaveCity(Session& s) {
    City& city = CityOf(s);
    GateOf(s).players.Add(&s);
    city.players.Remove(&s);

    if (!city.players.Count()) {
        const Bytes name = city.name;
        city = City{};
        city.name = name;
    }

    s.cityId = 0;
}

void LeaveGate(Session& s) {
    state->world.players.Add(&s);
    GateOf(s).players.Remove(&s);
    s.gateId = 0;
}

// notify_layer_departure, then layer_end or layer_up.
void LeaveLayer(Client& client, bool end) {
    Session& s = *client.session;
    const uint32_t cityId = s.cityId;

    if (s.layer == 2) {
        City& city = CityOf(s);

        if (city.leader == &s)
            for (Session* player : city.players.slots)
                if (player && player != &s) {
                    city.leader = player;
                    Bytes data;
                    U16(data, 0x08);
                    U32(data, 0);
                    U32(data, 0);
                    Lp2(data, player->capcomId);
                    Lp2(data, player->hunterName);
                    Broadcast(city.players, kNtcLayerHost, data, 0, &s);
                    break;
                }
    }

    if (s.layer > 0) {
        Bytes data;
        Lp2(data, s.capcomId);
        Broadcast(LayerPlayers(s), kNtcLayerOut, data, 0, &s);
    }

    if (s.circleId >= 0)
        LeaveCircle(client, 0);

    const uint32_t gateId = s.gateId;

    if (end) {
        if (s.layer > 1)
            LeaveCity(s);

        if (s.layer > 0)
            LeaveGate(s);

        s.layer = 0;
    } else {
        if (s.layer == 1)
            LeaveGate(s);
        else if (s.layer == 2)
            LeaveCity(s);

        if (s.layer)
            --s.layer;
    }

    // The gate hears the city's new count.
    if (cityId && s.gateId == gateId) {
        NotifyCityNumber(s, cityId);

        if (!GateOf(s).cities[cityId - 1].leader)
            NotifyCityInfo(s, cityId);
    }
}

void LeaveCircle(Client& client, uint32_t sequence) {
    Session& s = *client.session;
    City& city = CityOf(s);
    const uint32_t index = uint32_t(s.circleId + 1);
    Circle& circle = city.circles[size_t(s.circleId)];
    Bytes data;
    U32(data, index);
    Lp2(data, s.capcomId);
    data += char(circle.players.Index(&s) + 1);
    data += char(0);
    Broadcast(circle.players, kNtcCircleLeave, data, sequence, &s);

    if (circle.leader == &s) {
        if (circle.departed) {
            // The next hunter leads what is left of the quest.
            for (size_t i = 0; i < circle.players.slots.size(); ++i)
                if (Session* player = circle.players.slots[i]; player && player != &s) {
                    circle.leader = player;
                    Bytes host;
                    U32(host, index);
                    host += char(i + 1);
                    Lp2(host, player->capcomId);
                    Lp2(host, player->hunterName);
                    Broadcast(circle.players, kNtcCircleHost, host, sequence, &s);
                    break;
                }
        } else {
            Bytes unknown;
            U32(unknown, 0);
            Broadcast(circle.players, kNtcCircleBreak, unknown, sequence, &s);
        }
    }

    s.circleId = -1;
    s.inQuest = false;
    s.standby = false;

    if (circle.leader == &s)
        circle = Circle{};
    else
        circle.players.Remove(&s);

    CircleListChange(s, index, sequence);
}

Session& NewSession() {
    state->sessions.emplace_back();
    return state->sessions.back();
}

void Handle(Client& client, uint32_t id, const Bytes& data, uint32_t sequence) {
    Reader in(data);
    Session* session = client.session;

    switch (id) {
    case kAnsConnection: {
        const Items settings = ReadItems(in);
        Session* found = nullptr;
        bool identified = false;

        if (const auto ticket = settings.find(0x02); ticket != settings.end()) {
            identified = true;

            for (auto& s : state->sessions)
                if (s.ticket == ticket->second.text)
                    found = &s;
        }

        if (!found)
            found = &NewSession();

        if (const auto code = settings.find(0x01); code != settings.end()) {
            identified = true;
            found->supportCode = code->second.text;
        }

        found->client = &client;
        found->keep = !identified;
        client.session = found;
        const uint8_t login = client.port == kFmpPort ? kLoginFmp : client.port == kLmpPort ? (settings.count(0x02) ? kLoginLmpTicket : kLoginLmp) : kLoginReady;
        Send(client, kNtcLogin, Bytes(1, char(login)), sequence);
        break;
    }
    case kAnsLineCheck:
    case kAnsMemoryCheck:
    case kNtcCheatCheck:
    case kNtcCollectionLog:
        break;
    case kReqAuthenticationToken:
        Send(client, kAnsAuthenticationToken, {}, sequence);
        break;
    case kReqTermsVersion: {
        Bytes out;
        U32(out, 1);
        U32(out, 16);
        Send(client, kAnsTermsVersion, out, sequence);
        break;
    }
    case kReqTerms: {
        in.U32();
        const uint32_t offset = in.U32(), size = in.U32();
        static const Bytes kTerms = "Ad hoc play.    ";
        Bytes out;
        U32(out, offset);
        U32(out, size);
        Lp2(out, kTerms.substr(std::min<size_t>(offset, kTerms.size()), size));
        Send(client, kAnsTerms, out, sequence);
        break;
    }
    case kReqSubTermsInfo: {
        Bytes out;
        U32(out, 1);
        out += char(in.U8());
        U32(out, 8);
        Send(client, kAnsSubTermsInfo, out, sequence);
        break;
    }
    case kReqSubTerms: {
        in.U32();
        const uint32_t unknown = in.U8(), offset = in.U32(), size = in.U32();
        static const Bytes kTerms = "Ad hoc. ";
        Bytes out(1, char(unknown));
        U32(out, offset);
        U32(out, size);
        Lp2(out, kTerms.substr(std::min<size_t>(offset, kTerms.size()), size));
        Send(client, kAnsSubTerms, out, sequence);
        break;
    }
    case kReqAnnounce: {
        Bytes out;
        Lp2(out, "<BR><CENTER><BODY><SIZE=6>Ad hoc play<BR><BODY><CENTER><SIZE=4>Hunters on this console's network.<END>");
        Send(client, kAnsAnnounce, out, sequence);
        break;
    }
    case kReqNoCharge: {
        Bytes out;
        Lp2(out, "<BODY><CENTER>No charge.<END>");
        Send(client, kAnsNoCharge, out, sequence);
        break;
    }
    case kReqVulgarityInfoLow: {
        Bytes out;
        U32(out, 1);
        U32(out, in.U32());
        U32(out, 0);
        Send(client, kAnsVulgarityInfoLow, out, sequence);
        break;
    }
    case kReqCommonKey:
        // As MH3SP: the common key would start encryption; this answer skips it.
        Send(client, kAnsAuthenticationToken, {}, sequence);
        break;
    case kReqLmpConnect: {
        Bytes out;
        Lp2(out, state->address);
        U16(out, kLmpPort);
        Send(client, kAnsLmpConnect, out, sequence);
        break;
    }
    case kReqShut: {
        const uint32_t type = in.U8();
        Send(client, kAnsShut, Bytes(1, char(type)), sequence);

        if (type == 1 && session)
            session->keep = false;

        client.closing = true;
        break;
    }
    case kReqLoginInfo: {
        if (session->supportCode.empty())
            session->supportCode = RandomText(11);

        state->consoles.try_emplace(session->supportCode, std::array<Bytes, 6>{kBlankCapcomId, kBlankCapcomId, kBlankCapcomId, kBlankCapcomId, kBlankCapcomId, kBlankCapcomId});
        Bytes out(1, char(1));
        Lp2(out, "dummy_data");
        Fields info;
        info.String(0x07, session->supportCode);
        out += info.Pack();
        Send(client, kAnsLoginInfo, out, sequence);
        break;
    }
    case kReqTicket: {
        session->ticket = RandomText(11);
        Bytes out;
        Lp2(out, session->ticket);
        Send(client, kAnsTicket, out, sequence);
        break;
    }
    case kReqUserListHead: {
        Bytes out;
        U32(out, in.U32());
        U32(out, in.U32());
        Send(client, kAnsUserListHead, out, sequence);
        break;
    }
    case kReqUserListData: {
        const uint32_t first = in.U32(), count = in.U32();
        Bytes out;
        U32(out, first);
        U32(out, count);
        const auto& users = state->consoles[session->supportCode];

        for (uint32_t i = 0; i < count; ++i) {
            const Bytes capcomId = i < users.size() ? users[i] : kBlankCapcomId;
            Fields user;
            user.Long(0x01, first + i);
            user.String(0x02, capcomId);

            if (const auto name = state->names.find(capcomId); name != state->names.end())
                user.String(0x03, name->second);

            out += user.Pack();
        }

        Send(client, kAnsUserListData, out, sequence);
        break;
    }
    case kReqUserListFoot:
        Send(client, kAnsUserListFoot, {}, sequence);
        break;
    case kReqServerTime: {
        Bytes out;
        U32(out, Now());
        U32(out, Now());
        Send(client, kAnsServerTime, out, sequence);
        break;
    }
    case kReqUserObject: {
        const uint32_t empty = in.U8(), slot = in.U32();
        (void)empty;
        Items user = ReadItems(in);
        Bytes name;

        if (const auto found = user.find(0x03); found != user.end())
            name = found->second.text;

        auto& users = state->consoles[session->supportCode];
        Bytes& capcomId = users[std::clamp<uint32_t>(slot, 1, 6) - 1];

        if (capcomId == kBlankCapcomId)
            capcomId = RandomText(6);

        if (!name.empty())
            state->names[capcomId] = name;

        session->capcomId = capcomId;
        session->hunterName = state->names[capcomId];
        Bytes out(1, char(1));
        Lp2(out, "Unk_UserObj_str");
        U32(out, 0);
        Fields object;

        for (const auto& [field, item] : user)
            if (field == 0x01)
                object.Long(field, item.number);

        object.String(0x02, capcomId);

        if (!name.empty())
            object.String(0x03, name);

        out += object.Pack();
        Send(client, kAnsUserObject, out, sequence);
        break;
    }
    case kReqFmpListVersion:
    case kReqFmpListVersion2: {
        Bytes out;
        U32(out, 1);
        Send(client, id + 0x100, out, sequence); // the answer's id
        break;
    }
    case kReqFmpListHead:
    case kReqFmpListHead2: {
        Bytes out;
        U32(out, 0);
        U32(out, 1);
        Send(client, id + 0x100, out, sequence); // the answer's id
        break;
    }
    case kReqFmpListData:
    case kReqFmpListData2: {
        const uint32_t first = in.U32(), count = in.U32();
        Bytes out;
        U32(out, 0);
        U32(out, count);

        if (first == 1) {
            Fields server;
            server.Long(0x01, 1);
            server.String(0x02, state->address);
            server.Word(0x03, kFmpPort);
            server.LongLong(0x07, 1);
            server.Long(0x08, state->world.Population());
            server.Long(0x09, uint32_t(state->world.players.Capacity()));
            server.String(0x0A, state->world.name);
            server.String(0x0B, "X");
            server.Long(0x0C, 0x12345678);
            out += server.Pack();
        }

        Send(client, id + 0x100, out, sequence); // the answer's id
        break;
    }
    case kReqFmpListFoot:
    case kReqFmpListFoot2:
        Send(client, id + 0x100, {}, sequence);
        break;
    case kReqFmpInfo:
    case kReqFmpInfo2: {
        const uint32_t index = in.U32();
        const uint32_t fields = in.U8();
        const Bytes wanted = in.Text(fields);
        (void)index;
        session->serverId = 1;
        state->world.players.Add(session);
        Fields server;

        if (wanted.find('\x02') != Bytes::npos)
            server.String(0x02, state->address);

        if (wanted.find('\x03') != Bytes::npos)
            server.Word(0x03, kFmpPort);

        Send(client, id + 0x100, server.Pack(), sequence);
        session->keep = true;
        break;
    }
    case kReqBinaryVersion: {
        Bytes out(1, char(0));
        U32(out, 1);
        Send(client, kAnsBinaryVersion, out, sequence);
        break;
    }
    case kReqBinaryHead: {
        Bytes out;
        U32(out, 1);
        U32(out, uint32_t(BinaryContent(in.U8()).size()));
        Send(client, kAnsBinaryHead, out, sequence);
        break;
    }
    case kReqBinaryData: {
        const uint32_t type = in.U8(), version = in.U32(), offset = in.U32(), size = in.U32();
        const Bytes content = BinaryContent(type);
        Bytes out;
        U32(out, version);
        U32(out, offset);
        U32(out, size);
        Lp2(out, offset < content.size() ? content.substr(offset, size) : Bytes());
        Send(client, kAnsBinaryData, out, sequence);
        break;
    }
    case kReqBinaryFoot:
        Send(client, kAnsBinaryFoot, {}, sequence);
        break;
    case kReqUserSearchInfoMine: {
        // Identify a hunter in NtcLayerBinary.
        uint32_t hash = 2166136261u;

        for (const char c : session->capcomId)
            hash = (hash ^ uint8_t(c)) * 16777619u;

        Fields info;
        info.Long(0x0F, hash);
        info.Long(0x10, hash ^ 0x5A5A5A5A);
        Send(client, kAnsUserSearchInfoMine, info.Pack(), sequence);
        break;
    }
    case kReqLayerStart:
        session->layer = 0;
        Send(client, kAnsLayerStart, DummyLayer(), sequence);
        break;
    case kReqLayerEnd:
        LeaveLayer(client, true);
        Send(client, kAnsLayerEnd, {}, sequence);
        break;
    case kReqCircleInfoNoticeSet:
        Send(client, kAnsCircleInfoNoticeSet, {}, sequence);
        break;
    case kReqUserBinarySet: {
        const uint32_t offset = in.U32();
        const Bytes binary = in.Lp2();

        if (offset < kHunterBytes)
            std::copy_n(binary.begin(), std::min(binary.size(), kHunterBytes - offset), session->hunter.begin() + offset);

        Send(client, kAnsUserBinarySet, {}, sequence);
        break;
    }
    case kReqUserSearchSet:
        Send(client, kAnsUserSearchSet, {}, sequence);
        break;
    case kReqUserStatusSet:
        Send(client, kAnsUserStatusSet, {}, sequence);
        break;
    case kReqUserBinaryNotice: {
        const uint32_t unknown = in.U8();
        in.Lp2();
        const uint32_t offset = in.U32(), length = in.U32();
        Bytes out(1, char(unknown));
        Lp2(out, session->capcomId);
        U32(out, offset);
        Lp2(out, offset < kHunterBytes ? Bytes(reinterpret_cast<const char*>(session->hunter.data()) + offset, std::min<size_t>(length, kHunterBytes - offset)) : Bytes());
        Broadcast(LayerPlayers(*session), kNtcUserBinaryNotice, out, sequence, session);
        Send(client, kAnsUserBinaryNotice, {}, sequence);
        break;
    }
    case kReqLayerUserList: {
        const Players& players = LayerPlayers(*session);
        Bytes out;
        U32(out, uint32_t(players.Count()));

        for (const Session* player : players.slots)
            if (player)
                out += LayerUser(*player);

        Send(client, kAnsLayerUserList, out, sequence);
        break;
    }
    case kReqFriendList: {
        Bytes out;
        U32(out, 0);
        U32(out, 0);
        Send(client, kAnsFriendList, out, sequence);
        break;
    }
    case kReqBlackList: {
        Bytes out;
        U32(out, 0);
        U32(out, 1);
        Fields dummy;
        dummy.Long(0x01, 0);
        dummy.String(0x02, "");
        dummy.String(0x03, "");
        out += dummy.Pack();
        Send(client, kAnsBlackList, out, sequence);
        break;
    }
    case kReqLayerChildListHead: {
        Bytes out;
        U32(out, 0);
        U32(out, session->layer == 0 ? kGates : kCities);
        Send(client, kAnsLayerChildListHead, out, sequence);
        break;
    }
    case kReqLayerChildListData: {
        const uint32_t first = in.U32(), count = in.U32();
        Bytes out;
        U32(out, first);
        U32(out, count);

        for (uint32_t i = first; i < first + count; ++i) {
            if (session->layer == 0 && i >= 1 && i <= kGates)
                out += GatePack(i, state->world.gates[i - 1]) + PackOptional({});
            else if (session->layer == 1 && i >= 1 && i <= kCities)
                out += CityPack(session->gateId, i, GateOf(*session).cities[i - 1], false) + PackOptional(GateOf(*session).cities[i - 1].optional);
        }

        Send(client, kAnsLayerChildListData, out, sequence);
        break;
    }
    case kReqLayerChildListFoot:
        Send(client, kAnsLayerChildListFoot, {}, sequence);
        break;
    case kReqLayerChildInfo: {
        Bytes out;
        U16(out, 1);
        out += DummyLayer();
        out += Bytes(2, '\0');
        Send(client, kAnsLayerChildInfo, out, sequence);
        break;
    }
    case kReqLayerDown: {
        const uint32_t layerId = in.U16();

        if (session->layer == 0) {
            if (layerId < 1 || layerId > kGates)
                break;

            state->world.players.Remove(session);
            session->gateId = layerId;
            GateOf(*session).players.Add(session);
        } else if (session->layer == 1) {
            if (layerId < 1 || layerId > kCities)
                break;

            City& city = GateOf(*session).cities[layerId - 1];

            if (city.players.Add(session) < 0)
                break;

            GateOf(*session).players.Remove(session);
            session->cityId = layerId;
        }

        ++session->layer;

        if (session->layer == 1) {
            Bytes notice;
            Lp2(notice, session->capcomId);
            notice += LayerUser(*session);
            Broadcast(GateOf(*session).players, kNtcLayerIn, notice, sequence, session);
        }

        Bytes out;
        U16(out, layerId);
        Send(client, kAnsLayerDown, out, sequence);
        break;
    }
    case kReqLayerCreateHead: {
        const uint32_t number = in.U16();

        if (number < 1 || number > kCities) {
            client.closing = true;
            break;
        }

        City& city = GateOf(*session).cities[number - 1];
        Bytes out;
        U16(out, number);
        city.reserved = true;
        Send(client, kAnsLayerCreateHead, out, sequence);
        NotifyCityInfo(*session, number);
        break;
    }
    case kReqLayerCreateSet: {
        const uint32_t number = in.U16();
        ReadItems(in);
        const Optional extra = ReadOptional(in);

        if (number < 1 || number > kCities) {
            client.closing = true;
            break;
        }

        City& city = GateOf(*session).cities[number - 1];
        city.optional = extra;
        city.leader = session;
        city.reserved = false;
        city.players.Add(session);
        GateOf(*session).players.Remove(session);
        session->cityId = number;
        ++session->layer;
        Bytes out;
        U16(out, number);
        Send(client, kAnsLayerCreateSet, out, sequence);
        NotifyCityNumber(*session, number);
        break;
    }
    case kReqLayerCreateFoot: {
        const uint32_t number = in.U16();
        Bytes out;
        U16(out, number);
        Send(client, kAnsLayerCreateFoot, out, sequence);

        if (number >= 1 && number <= kCities)
            NotifyCityInfo(*session, number);

        break;
    }
    case kReqLayerUp:
        LeaveLayer(client, false);
        Send(client, kAnsLayerUp, {}, sequence);
        break;
    case kReqLayerHost: {
        City& city = CityOf(*session);
        Bytes out = data;

        if (city.leader) {
            Lp2(out, city.leader->capcomId);
            Lp2(out, city.leader->hunterName);
        }

        Send(client, kAnsLayerHost, out, sequence);
        Bytes notice;
        Lp2(notice, session->capcomId);
        notice += LayerUser(*session);
        Broadcast(city.players, kNtcLayerIn, notice, sequence, session);
        break;
    }
    case kReqLayerMediationList: {
        constexpr uint32_t kCount = 14;
        Bytes out(1, char(0));
        out += char(kCount);

        for (uint32_t i = 1; i <= kCount; ++i) {
            Fields item;
            item.String(0x01, "Test");
            item.Byte(0x02, uint8_t(i));
            item.Byte(0x03, 0);
            out += item.Pack();
        }

        Send(client, kAnsLayerMediationList, out, sequence);
        break;
    }
    case kNtcLayerUserPosition: {
        Bytes out;
        Lp2(out, session->capcomId);
        out += data;
        Broadcast(LayerPlayers(*session), kNtcLayerUserPosition, out, sequence, session);
        break;
    }
    case kNtcLayerBinary: {
        ReadItems(in);
        Bytes out;
        Lp2(out, session->capcomId);
        out += BinaryInfo(*session, true);
        out += in.Rest();
        Broadcast(LayerPlayers(*session), kNtcLayerBinary, out, sequence, session);
        break;
    }
    case kNtcLayerBinary2: {
        const Bytes partner = in.Lp2();
        ReadItems(in);
        Bytes out;
        Lp2(out, session->capcomId);
        out += BinaryInfo(*session, false);
        out += in.Rest();

        if (const Session* peer = CityOf(*session).players.Find(partner); peer && peer->client)
            Send(*peer->client, kNtcLayerBinary2, out, sequence);

        break;
    }
    case kReqCircleListLayer: {
        const City& city = CityOf(*session);
        Bytes list;
        uint32_t count = 0;

        for (uint32_t i = 0; i < city.circles.size(); ++i)
            if (!city.circles[i].Empty()) {
                list += CirclePack(city.circles[i], i + 1);
                ++count;
            }

        Bytes out;
        U32(out, 0);
        U32(out, count);
        out += list;
        Send(client, kAnsCircleListLayer, out, sequence);
        break;
    }
    case kReqCircleCreate: {
        Items info = ReadItems(in);
        const Optional extra = ReadOptional(in);
        City& city = CityOf(*session);
        const auto free = std::find_if(city.circles.begin(), city.circles.end(), [](const Circle& c) { return c.Empty(); });

        if (free == city.circles.end()) {
            Bytes out;
            U32(out, 1);
            Lp2(out, "<LF=8><BODY><CENTER>No Quest Slot Available<END>");
            Send(client, kAnsCircleCreate | 1, out, sequence);
            break;
        }

        Circle& circle = *free;
        circle.leader = session;

        if (info.count(0x04))
            circle.password = info[0x04].text;

        if (info.count(0x06))
            circle.remarks = info[0x06].text;

        if (info.count(0x0E))
            circle.unknown0e = info[0x0E].number;

        circle.players = Players(info.count(0x09) ? std::clamp<uint32_t>(info[0x09].number, 1, 4) : 4);
        circle.players.Add(session);
        const uint32_t index = uint32_t(free - city.circles.begin());
        session->circleId = int(index);

        for (const auto& [field, value] : extra)
            if (field == 2 && value)
                circle.questId = *value;

        CircleListChange(*session, index + 1, sequence);
        Bytes out;
        U32(out, index + 1);
        Send(client, kAnsCircleCreate, out, sequence);
        break;
    }
    case kReqCircleMatchOptionSet: {
        Items options = ReadItems(in);
        session->standby = options.count(0x03) && options[0x03].number == 1;
        Circle& circle = CityOf(*session).circles[size_t(session->circleId)];
        Fields notice;

        for (const auto& [field, item] : options)
            if (item.type == kItemBinary)
                notice.Binary(field, item.text);
            else if (item.type == kItemWord)
                notice.Word(field, uint16_t(item.number));
            else if (item.type == kItemByte && field == 0x03)
                notice.Byte(field, uint8_t(item.number));

        notice.Byte(0x04, uint8_t(circle.players.Index(session) + 1));
        notice.String(0x05, session->capcomId);
        notice.String(0x06, session->hunterName);
        Broadcast(circle.players, kNtcCircleMatchOptionSet, notice.Pack(), sequence, session);
        Send(client, kAnsCircleMatchOptionSet, {}, sequence);
        break;
    }
    case kReqCircleInfo: {
        const uint32_t index = in.U32();
        Bytes out;
        U32(out, index);

        if (index >= 1 && index <= kCityPlayers)
            out += CirclePack(CityOf(*session).circles[index - 1], index);

        Send(client, kAnsCircleInfo, out, sequence);
        break;
    }
    case kReqCircleInfoSet: {
        const uint32_t index = in.U32();
        const size_t start = in.Offset();
        ReadItems(in);
        const Optional extra = ReadOptional(in);
        const Bytes raw = data.substr(start, in.Offset() - start);

        if (index >= 1 && index <= kCityPlayers) {
            Circle& circle = CityOf(*session).circles[index - 1];
            Bytes notice;
            U32(notice, index);
            notice += raw;
            Broadcast(circle.players, kNtcCircleInfoSet, notice, sequence, session);
            Bytes out;
            U32(out, index);
            Send(client, kAnsCircleInfoSet, out, sequence);
            CircleListChange(*session, index, sequence);
        }

        break;
    }
    case kReqCircleJoin: {
        const uint32_t index = in.U32();
        Items info = ReadItems(in);
        const auto refuse = [&](const char* text) {
            Bytes out;
            U32(out, 1);
            Lp2(out, text);
            Send(client, kAnsCircleJoin | 1, out, sequence);
        };

        if (index < 1 || index > kCityPlayers) {
            refuse("<LF=8><BODY><CENTER>Invalid Quest Slot<END>");
            break;
        }

        Circle& circle = CityOf(*session).circles[index - 1];

        if (circle.password && (!info.count(0x04) || info[0x04].text != *circle.password)) {
            refuse("<LF=8><BODY><CENTER>Wrong Password!<END>");
            break;
        }

        const int player = circle.players.Add(session);

        if (player < 0) {
            refuse("<LF=8><BODY><CENTER>Quest is full!<END>");
            break;
        }

        session->circleId = int(index - 1);
        Bytes out;
        U32(out, index);
        out += char(player + 1);
        Send(client, kAnsCircleJoin, out, sequence);
        Bytes notice;
        U32(notice, index);
        Lp2(notice, session->capcomId);
        Lp2(notice, session->hunterName);
        notice += char(player + 1);
        notice += char(0);
        Broadcast(circle.players, kNtcCircleJoin, notice, sequence, session);
        break;
    }
    case kReqCircleUserList: {
        const Circle& circle = CityOf(*session).circles[size_t(session->circleId)];
        Bytes out;
        U32(out, uint32_t(circle.players.Count()));

        for (size_t i = 0; i < circle.players.slots.size(); ++i)
            if (const Session* player = circle.players.slots[i]) {
                Fields user;
                user.Byte(0x03, player->standby);
                user.Byte(0x04, uint8_t(i + 1));
                user.String(0x05, player->capcomId);
                user.String(0x06, player->hunterName);
                out += user.Pack();
            }

        Send(client, kAnsCircleUserList, out, sequence);
        break;
    }
    case kReqCircleHost: {
        const uint32_t index = in.U32();
        const Circle& circle = CityOf(*session).circles[std::clamp<uint32_t>(index, 1, kCityPlayers) - 1];
        const int leader = circle.leader ? circle.players.Index(circle.leader) : -1;

        if (leader < 0) {
            Bytes out;
            U32(out, 1);
            Lp2(out, "<LF=8><BODY><CENTER>Unknown Quest Leader<END>");
            Send(client, kAnsCircleHost | 1, out, sequence);
            break;
        }

        Bytes out;
        U32(out, index);
        out += char(leader + 1);
        Lp2(out, circle.leader->capcomId);
        Lp2(out, circle.leader->hunterName);
        Send(client, kAnsCircleHost, out, sequence);
        break;
    }
    case kNtcCircleBinary: {
        const uint32_t index = in.U32();
        ReadItems(in);
        Bytes out;
        U32(out, index);
        Lp2(out, session->capcomId);
        out += BinaryInfo(*session, true);
        out += in.Rest();

        if (index >= 1 && index <= kCityPlayers)
            Broadcast(CityOf(*session).circles[index - 1].players, kNtcCircleBinary, out, sequence, session);

        break;
    }
    case kNtcCircleBinary2: {
        const uint32_t index = in.U32();
        const Bytes partner = in.Lp2();
        ReadItems(in);
        Bytes out;
        U32(out, index);
        Lp2(out, session->capcomId);
        out += BinaryInfo(*session, false);
        out += in.Rest();

        if (index >= 1 && index <= kCityPlayers)
            if (const Session* peer = CityOf(*session).circles[index - 1].players.Find(partner); peer && peer->client)
                Send(*peer->client, kNtcCircleBinary2, out, sequence);

        break;
    }
    case kReqCircleLeave: {
        const uint32_t index = in.U32();

        if (session->circleId >= 0)
            LeaveCircle(client, sequence);

        Bytes out;
        U32(out, index);
        Send(client, kAnsCircleLeave, out, sequence);
        break;
    }
    case kReqCircleMatchStart: {
        Circle& circle = CityOf(*session).circles[size_t(session->circleId)];
        circle.departed = true;
        Bytes list;
        uint32_t count = 0;
        std::vector<Session*> left;

        for (size_t i = 0; i < circle.players.slots.size(); ++i)
            if (Session* player = circle.players.slots[i]) {
                if (player->standby) {
                    list += char(i + 1);
                    Lp2(list, player->capcomId);
                    Lp2(list, Bytes(1, '\1'));
                    U16(list, 21);
                    player->inQuest = true;
                    ++count;
                } else {
                    left.push_back(player);
                }
            }

        Bytes body;
        U32(body, count);
        body += list;
        Bytes notice;
        U16(notice, uint32_t(body.size()));
        notice += body;
        U32(notice, 1);
        Broadcast(circle.players, kNtcCircleMatchStart, notice, sequence, nullptr);

        for (Session* player : left)
            if (player->client) {
                Bytes kick(1, '\0');
                Lp2(kick, "");
                Send(*player->client, kNtcCircleKick, kick, sequence);
                LeaveCircle(*player->client, sequence);
            }

        Send(client, kAnsCircleMatchStart, {}, sequence);
        break;
    }
    case kReqCircleMatchEnd:
        Send(client, kAnsCircleMatchEnd, {}, sequence);
        break;
    default:
        std::fprintf(stderr, "MH3 local server: unhandled packet %08X (%u bytes) from port %u\n", id, unsigned(data.size()), client.port);
    }
}

// A session is needed from the first handler on: AnsConnection makes it.
constexpr bool NeedsNoSession(uint32_t id) {
    return id == kAnsConnection || id == kAnsLineCheck || id == kAnsMemoryCheck || id == kNtcCollectionLog;
}

void Ended(Client& client) {
    if (Session* s = client.session) {
        s->client = &client;
        LeaveLayer(client, true);
        s->client = nullptr;

        if (!s->keep) {
            state->world.players.Remove(s);
            state->sessions.remove_if([s](const Session& other) { return &other == s; });
        }
    }

    client.session = nullptr;
    client.ended = true;
}

void Pump(Client& client) {
    uint8_t buffer[4096];

    while (true) {
        const int count = client.connection->Receive(buffer, sizeof(buffer));

        if (count < 0) {
            Ended(client);
            return;
        }

        if (count == 0)
            break;

        client.input.append(reinterpret_cast<char*>(buffer), size_t(count));
    }

    while (client.input.size() >= 8) {
        Reader header(client.input);
        const uint32_t size = header.U16(), sequence = header.U16(), id = header.U32();

        if (client.input.size() < 8 + size_t(size))
            break;

        const Bytes data = client.input.substr(8, size);
        client.input.erase(0, 8 + size_t(size));

        Dump("<", id, client.port, data);

        if (client.session || NeedsNoSession(id))
            Handle(client, id, data, sequence);
    }

    // As MH3SP's on_tick: the game takes a moment to listen after connecting.
    const auto now = Clock::now();

    if (!client.requested && now - client.opened >= std::chrono::milliseconds(1500)) {
        client.requested = true;
        Bytes data;
        U32(data, 0);
        Send(client, kReqConnection, data, 0);
        client.pinged = now;
    } else if (client.requested && now - client.pinged >= std::chrono::seconds(30)) {
        Send(client, kReqLineCheck, {}, 0);
        client.pinged = now;
    }

    while (!client.output.empty()) {
        const int sent = client.connection->Send(reinterpret_cast<const uint8_t*>(client.output.data()), client.output.size());

        if (sent < 0) {
            Ended(client);
            return;
        }

        if (sent == 0)
            break;

        client.output.erase(0, size_t(sent));
    }

    if (client.closing && client.output.empty())
        Ended(client);
}

Bytes Base64(const Bytes& text) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes out;

    for (size_t i = 0; i < text.size(); i += 3) {
        const uint32_t a = uint8_t(text[i]), b = i + 1 < text.size() ? uint8_t(text[i + 1]) : 0, c = i + 2 < text.size() ? uint8_t(text[i + 2]) : 0;
        const uint32_t value = a << 16 | b << 8 | c;
        out += kTable[value >> 18 & 63];
        out += kTable[value >> 12 & 63];
        out += i + 1 < text.size() ? kTable[value >> 6 & 63] : '*';
        out += i + 2 < text.size() ? kTable[value & 63] : '*';
    }

    return out;
}

// DWC's form values: base64 with * for = and other letters for + and /.
Bytes Unbase64(const Bytes& text) {
    uint32_t value = 0, bits = 0;
    Bytes out;

    for (const char c : text) {
        const int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '.' || c == '>' ? 62 : c == '/' || c == '-' || c == '?' ? 63 : -1;

        if (digit < 0)
            continue;

        value = value << 6 | uint32_t(digit);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out += char(value >> bits);
        }
    }

    return out;
}

Bytes FormValue(const Bytes& form, const Bytes& key) {
    size_t at = 0;

    while (at < form.size()) {
        const size_t end = std::min(form.find('&', at), form.size()), equals = form.find('=', at);

        if (equals < end && form.compare(at, equals - at, key) == 0) {
            // URL-decoded, then base64.
            const Bytes raw = form.substr(equals + 1, end - equals - 1);
            Bytes value;

            for (size_t i = 0; i < raw.size(); ++i)
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    value += char(std::strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    value += raw[i];
                }

            return Unbase64(value);
        }

        at = end + 1;
    }

    return {};
}

Bytes NasToken() {
    return "NDS" + RandomText(96);
}

// The account server's answer to one form.
Bytes NasAnswer(const Bytes& path, const Bytes& form) {
    std::vector<std::pair<Bytes, Bytes>> reply;
    char stamp[16];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", std::gmtime(&now));

    if (path == "/ac") {
        const Bytes action = FormValue(form, "action");
        reply.push_back({"retry", "0"});

        if (action == "acctcreate") {
            reply.push_back({"returncd", "002"});
            reply.push_back({"userid", "100000000"});
        } else if (action == "login") {
            reply.push_back({"returncd", "001"});
            reply.push_back({"locator", "gamespy.com"});
            reply.push_back({"challenge", RandomText(8)});
            reply.push_back({"token", NasToken()});
        } else if (action == "svcloc") {
            reply.push_back({"returncd", "007"});
            reply.push_back({"statusdata", "Y"});
            reply.push_back({"svchost", "n/a"});
            reply.push_back({"servicetoken", NasToken()});
        } else {
            reply.push_back({"returncd", "109"});
        }
    } else {
        // The profanity check: no word is bad.
        const Bytes words = FormValue(form, "words");
        const Bytes clean(size_t(std::count(words.begin(), words.end(), '\t')) + 1, '0');
        reply.push_back({"prwords", clean});
        reply.push_back({"returncd", "000"});

        for (const char region : Bytes("ACEJKP"))
            reply.push_back({Bytes("prwords") + region, clean});
    }

    reply.push_back({"datetime", stamp});
    Bytes body;

    for (const auto& [key, value] : reply)
        body += (body.empty() ? "" : "&") + key + "=" + Base64(value);

    return body + "\r\n";
}

// One HTTP connection of the account server.
struct NasClient {
    std::unique_ptr<Connection> connection;
    Bytes input, output;
    bool done = false;
};

void Pump(NasClient& client) {
    uint8_t buffer[2048];

    while (true) {
        const int count = client.connection->Receive(buffer, sizeof(buffer));

        if (count < 0) {
            client.done = true;
            return;
        }

        if (count == 0)
            break;

        client.input.append(reinterpret_cast<char*>(buffer), size_t(count));
    }

    if (const size_t head = client.input.find("\r\n\r\n"); client.output.empty() && head != Bytes::npos) {
        Bytes lower = client.input.substr(0, head);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const size_t lengthAt = lower.find("content-length:");
        const size_t length = lengthAt == Bytes::npos ? 0 : size_t(std::atoi(lower.c_str() + lengthAt + 15));

        if (client.input.size() >= head + 4 + length) {
            const size_t pathAt = client.input.find(' ') + 1;
            const Bytes path = client.input.substr(pathAt, client.input.find(' ', pathAt) - pathAt);
            const Bytes form = client.input.substr(head + 4, length);
            const Bytes body = client.input.rfind("POST", 0) == 0 ? NasAnswer(path, form) : "ok";
            std::fprintf(stderr, "MH3 local server: %s action %s\n", path.c_str(), FormValue(form, "action").c_str());
            client.output = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nNODE: wifiappe1\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            client.input.clear();
        }
    }

    while (!client.output.empty()) {
        const int sent = client.connection->Send(reinterpret_cast<const uint8_t*>(client.output.data()), client.output.size());

        if (sent < 0) {
            client.done = true;
            return;
        }

        if (sent == 0)
            return;

        client.output.erase(0, size_t(sent));

        if (client.output.empty())
            client.done = true;
    }
}

std::thread thread;
std::atomic<bool> running{false};

void Run(std::unique_ptr<Listener> opn, std::unique_ptr<Listener> lmp, std::unique_ptr<Listener> fmp, std::unique_ptr<Listener> nas) {
    std::list<NasClient> accounts;
    const std::pair<Listener*, uint16_t> game[]{{opn.get(), kOpnPort}, {lmp.get(), kLmpPort}, {fmp.get(), kFmpPort}};

    while (running.load(std::memory_order_relaxed)) {
        for (const auto& [listener, port] : game)
            while (auto connection = listener->Accept()) {
                state->clients.emplace_back();
                state->clients.back().connection = std::move(connection);
                state->clients.back().port = port;
            }

        while (auto connection = nas->Accept()) {
            accounts.emplace_back();
            accounts.back().connection = std::move(connection);
        }

        for (auto& client : state->clients)
            if (!client.ended)
                Pump(client);

        state->clients.remove_if([](const Client& client) { return client.ended; });

        for (auto& client : accounts)
            Pump(client);

        accounts.remove_if([](const NasClient& client) { return client.done; });
        PollAvailability();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    delete state;
    state = nullptr;
}
} // namespace

bool Start(const std::string& address) {
    if (running.load())
        return true;

    auto opn = Listen(kOpnPort), lmp = Listen(kLmpPort), fmp = Listen(kFmpPort), nas = Listen(kNasPort);

    if (!opn || !lmp || !fmp || !nas) {
        std::fprintf(stderr, "MH3 local server: cannot listen\n");
        return false;
    }

    state = new State;
    state->address = address;

    for (uint32_t g = 0; g < kGates; ++g) {
        Gate& gate = state->world.gates[g];
        gate.name = "City Gate " + std::to_string(g + 1);

        for (uint32_t c = 0; c < kCities; ++c)
            gate.cities[c].name = "City " + std::to_string(c + 1);
    }

    running.store(true);
    thread = std::thread(Run, std::move(opn), std::move(lmp), std::move(fmp), std::move(nas));
    std::fprintf(stderr, "MH3 local server: hosting at %s\n", address.c_str());
    return true;
}

void Stop() {
    if (!running.exchange(false))
        return;

    thread.join();
}

bool Running() {
    return running.load();
}
} // namespace LocalServer
