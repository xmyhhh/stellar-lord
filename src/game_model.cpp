#include "game_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace stellar {

namespace {

double TechMultiplier(int level, double per_level = 0.20) {
    return 1.0 + per_level * static_cast<double>(std::max(0, level));
}

double Pow(double base, int exponent) {
    return std::pow(base, static_cast<double>(std::max(0, exponent)));
}

Cost ScaleCost(const Cost& base, double factor) {
    Cost cost;
    cost.energy = base.energy * factor;
    cost.minerals = base.minerals * factor;
    cost.food = base.food * factor;
    cost.alloy = base.alloy * factor;
    return cost;
}

bool CanPay(const GameState& state, const Cost& cost) {
    return state.energy >= cost.energy && state.minerals >= cost.minerals &&
           state.food >= cost.food && state.alloy >= cost.alloy;
}

void Pay(GameState& state, const Cost& cost) {
    state.energy = std::max(0.0, state.energy - cost.energy);
    state.minerals = std::max(0.0, state.minerals - cost.minerals);
    state.food = std::max(0.0, state.food - cost.food);
    state.alloy = std::max(0.0, state.alloy - cost.alloy);
}

std::uint64_t NextRandom(std::uint64_t& rng) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

double Random01(std::uint64_t& rng) {
    return static_cast<double>(NextRandom(rng) >> 11) / 9007199254740992.0;
}

void AddMorale(GameState& state, double delta) {
    state.morale = std::clamp(state.morale + delta, 0.0, 100.0);
}

void SpendResources(GameState& state, double energy, double minerals, double food, double alloy) {
    state.energy = std::max(0.0, state.energy - energy);
    state.minerals = std::max(0.0, state.minerals - minerals);
    state.food = std::max(0.0, state.food - food);
    state.alloy = std::max(0.0, state.alloy - alloy);
}

void GainResources(GameState& state, double energy, double minerals, double food, double alloy,
                   double research) {
    state.energy += std::max(0.0, energy);
    state.minerals += std::max(0.0, minerals);
    state.food += std::max(0.0, food);
    state.alloy += std::max(0.0, alloy);
    state.research += std::max(0.0, research);
}

double LootMultiplier(const GameState& state) {
    return state.faction == static_cast<int>(FactionId::kGuild) ? 1.25 : 1.0;
}

// --- 星际经济与远征用的确定性随机 -----------------------------------------

std::uint64_t Mix64(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

// 由市场相位和用途生成 0..1 的稳定随机数，保证刷新前后价格不会跳来跳去。
double Hash01(int phase, int salt) {
    const std::uint64_t seed =
        (static_cast<std::uint64_t>(phase) + 1ull) * 0x9E3779B97F4A7C15ull ^
        (static_cast<std::uint64_t>(salt) + 0x165667B19E3779F9ull);
    return static_cast<double>(Mix64(seed) >> 11) / 9007199254740992.0;
}

double BaseSellPrice(int resource) {
    switch (resource) {
    case 0: return 0.10;   // 能源
    case 1: return 0.32;   // 矿物
    case 2: return 0.28;   // 食物
    case 3: return 1.05;   // 合金
    default: return 0.0;
    }
}

// 信誉高时卖价更好、买价更低；0 信誉比 50 信誉贵约 20%。
double ReputationFactor(const GameState& state) {
    const double reputation = std::clamp(static_cast<double>(state.reputation), 0.0, 100.0);
    return 1.0 + (reputation - 50.0) / 50.0 * 0.10;
}

int ExpeditionMoraleDelta(int outcome) {
    switch (outcome) {
    case 0: return 2;
    case 1: return 1;
    case 2: return -2;
    case 3: return -5;
    case 4: return -10;
    default: return 0;
    }
}

int ExpeditionReputationDelta(int outcome) {
    switch (outcome) {
    case 0: return 3;
    case 1: return 2;
    case 2: return 0;
    case 3: return -1;
    case 4: return -4;
    default: return 0;
    }
}

void RefreshCommandCards(GameState& state, int phase) {
    state.card_phase = phase;
    state.card_expires = state.play_seconds + kCommandCardWindowSeconds;
    state.cards_available = true;
    for (int i = 0; i < kCommandCardChoices; ++i) {
        int id = std::min(
            static_cast<int>(CommandCardId::kCount) - 1,
            static_cast<int>(Hash01(phase, 700 + i * 17) *
                             static_cast<double>(CommandCardId::kCount)));
        bool duplicate = true;
        while (duplicate) {
            duplicate = false;
            for (int j = 0; j < i; ++j) {
                if (state.card_offers[j] == id) {
                    duplicate = true;
                    id = (id + 1) % static_cast<int>(CommandCardId::kCount);
                    break;
                }
            }
        }
        state.card_offers[i] = id;
    }
}

void EnsureCommandCards(GameState& state) {
    const int phase = MarketPhase(state);
    if (state.card_phase != phase) {
        RefreshCommandCards(state, phase);
    }
    if (state.cards_available &&
        state.play_seconds > state.card_expires + 1e-9) {
        state.cards_available = false;
    }
}

std::string JoinNumbers(const int* values, int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            text.push_back(',');
        }
        text += std::to_string(values[i]);
    }
    return text;
}

}  // namespace

// 这两个函数在下方定义；开局和反序列化时需要在公共 API 之前先准备好市场。
void RefreshMarket(GameState& state, int phase);
void EnsureMarket(GameState& state);

GameState MakeNewGame(double unix_time) {
    GameState state;
    state.energy = 60.0;
    state.minerals = 90.0;
    state.food = 40.0;
    state.alloy = 30.0;
    state.population = 12.0;
    state.research = 0.0;
    state.capital_level = 1;
    state.buildings[static_cast<int>(BuildingId::kPower)] = 1;
    state.buildings[static_cast<int>(BuildingId::kMine)] = 1;
    state.buildings[static_cast<int>(BuildingId::kFarm)] = 1;
    state.fleet = 10;
    state.morale = 70.0;
    state.faction = static_cast<int>(FactionId::kNone);
    state.wonders = 0;
    state.events.clear();
    state.next_event_seconds = 120.0;
    state.unix_time = unix_time;
    state.play_seconds = 0.0;
    state.day = 1;
    state.rng = 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(unix_time * 1000.0);
    if (state.rng == 0) {
        state.rng = 123456789ull;
    }
    state.influence = 0.0;
    state.tokens = 0.0;
    state.relics = 0;
    state.cores = 0;
    state.data_cores = 0;
    state.reputation = 50;
    state.contracts_done = 0;
    state.expeditions_done = 0;
    state.market_phase = -1;
    state.contracts.clear();
    state.black_market.clear();
    state.card_phase = MarketPhase(state);
    state.card_expires = 0.0;
    state.cards_available = false;
    state.card_offers[0] = 0;
    state.card_offers[1] = 1;
    state.card_offers[2] = 2;
    state.cards_taken = 0;
    state.expeditions.clear();
    state.expedition_reports.clear();
    EnsureMarket(state);
    return state;
}

double ProductionMultiplier(const GameState& state) {
    double multiplier = 1.0 + 0.10 * static_cast<double>(std::max(0, state.capital_level - 1));
    multiplier += 0.15 * static_cast<double>(TotalColonyLevels(state));
    return multiplier * LaborFactor(state) * MoraleFactor(state);
}

double PopulationCapacity(const GameState& state) {
    double capacity = 20.0 + 12.0 * static_cast<double>(std::max(1, state.capital_level)) +
                      10.0 * static_cast<double>(TotalColonyLevels(state));
    if (HasFaction(state, FactionId::kCompact)) {
        capacity *= 1.25;
    }
    return capacity;
}

double LaborFactor(const GameState& state) {
    const double capacity = PopulationCapacity(state);
    if (capacity <= 0.0) {
        return 0.55;
    }
    const double ratio = std::clamp(state.population / capacity, 0.0, 1.0);
    return 0.55 + 0.45 * ratio;
}

double MoraleFactor(const GameState& state) {
    if (state.morale >= 80.0) {
        return 1.15;
    }
    if (state.morale >= 50.0) {
        return 1.0;
    }
    if (state.morale >= 30.0) {
        return 0.90;
    }
    if (state.morale >= 10.0) {
        return 0.75;
    }
    return 0.55;
}

int FleetCapacity(const GameState& state) {
    int capacity = 20 + 12 * state.capital_level + 8 * TotalColonyLevels(state) +
                   6 * state.buildings[static_cast<int>(BuildingId::kBarracks)];
    if (HasWonder(state, kWonderOrbitalDock)) {
        capacity = static_cast<int>(static_cast<double>(capacity) * 1.5);
    }
    return capacity;
}

int TotalColonyLevels(const GameState& state) {
    int total = 0;
    for (const Colony& colony : state.colonies) {
        total += std::max(1, colony.level);
    }
    return total;
}

Rates ComputeRates(const GameState& state) {
    Rates rates;
    const double production = ProductionMultiplier(state);
    const auto building = [&state](BuildingId id) {
        return std::max(0, state.buildings[static_cast<int>(id)]);
    };
    const auto tech = [&state](TechId id) {
        return std::max(0, state.techs[static_cast<int>(id)]);
    };

    const double power = static_cast<double>(building(BuildingId::kPower));
    const double mine = static_cast<double>(building(BuildingId::kMine));
    const double farm = static_cast<double>(building(BuildingId::kFarm));
    const double factory = static_cast<double>(building(BuildingId::kFactory));
    const double lab = static_cast<double>(building(BuildingId::kLab));
    const double barracks = static_cast<double>(building(BuildingId::kBarracks));

    rates.energy = 2.2 * power * TechMultiplier(tech(TechId::kEnergy)) * production -
                   0.03 * state.population;
    rates.minerals = 1.8 * mine * TechMultiplier(tech(TechId::kMining)) * production;
    rates.food = 1.4 * farm * TechMultiplier(tech(TechId::kAgriculture)) * production -
                 0.04 * state.population;
    rates.alloy = 0.9 * factory * TechMultiplier(tech(TechId::kIndustry)) * production;
    rates.research = 0.45 * lab * production + 0.01 * state.population;

    if (HasWonder(state, kWonderDysonRing)) {
        rates.energy *= 2.0;
    }
    if (HasWonder(state, kWonderKnowledgeArk)) {
        rates.research *= 2.0;
    }

    switch (static_cast<FactionId>(state.faction)) {
    case FactionId::kAnvil:
        rates.alloy *= 1.30;
        rates.energy -= 0.06 * power;
        break;
    case FactionId::kChoir:
        rates.research *= 1.35;
        rates.energy *= 0.96;
        break;
    case FactionId::kCompact:
        rates.minerals *= 0.96;
        break;
    case FactionId::kGuild:
        rates.alloy *= 0.95;
        break;
    case FactionId::kNone:
    case FactionId::kCount:
    default:
        break;
    }

    rates.population_capacity = PopulationCapacity(state);
    const bool fed = state.food > 1.0 && state.energy > 1.0;
    rates.population_growth =
        fed ? 0.05 * TechMultiplier(tech(TechId::kAgriculture), 0.25) : -0.08;
    if (HasFaction(state, FactionId::kCompact) && fed) {
        rates.population_growth *= 1.15;
    }
    rates.fleet_growth = 0.35 * barracks * TechMultiplier(tech(TechId::kMilitary), 0.15);
    if (HasWonder(state, kWonderOrbitalDock)) {
        rates.fleet_growth *= 1.5;
    }
    rates.fleet_capacity = static_cast<double>(FleetCapacity(state));
    return rates;
}

void Advance(GameState& state, double dt_seconds) {
    if (!(dt_seconds > 0.0)) {
        return;
    }
    const double total = std::min(dt_seconds, 3600.0);
    const int steps = std::max(1, static_cast<int>(std::ceil(total)));
    const double step = total / static_cast<double>(steps);

    for (int i = 0; i < steps; ++i) {
        const Rates rates = ComputeRates(state);

        state.energy += rates.energy * step;
        state.minerals += rates.minerals * step;
        state.food += rates.food * step;
        state.research += rates.research * step;

        // 影响力来自领土与行政：殖民地越多，增长越快。
        state.influence +=
            0.004 * static_cast<double>(1 + state.capital_level + TotalColonyLevels(state)) * step;

        // 工厂消耗矿物；矿物不足时按比例减产。
        const double factory_levels =
            static_cast<double>(std::max(0, state.buildings[static_cast<int>(BuildingId::kFactory)]));
        double alloy_rate = rates.alloy;
        if (factory_levels > 0.0) {
            const double needed = 0.55 * factory_levels * step;
            if (state.minerals < needed && needed > 0.0) {
                const double scale = std::max(0.0, state.minerals / needed);
                alloy_rate *= scale;
                state.minerals = 0.0;
            } else {
                state.minerals -= needed;
            }
        }
        state.alloy += alloy_rate * step;

        // 实验室耗电，军营耗粮。
        const double lab_levels =
            static_cast<double>(std::max(0, state.buildings[static_cast<int>(BuildingId::kLab)]));
        const double barracks_levels = static_cast<double>(
            std::max(0, state.buildings[static_cast<int>(BuildingId::kBarracks)]));
        double energy_cost = 0.25 * lab_levels * step;
        if (state.energy < energy_cost) {
            energy_cost = std::max(0.0, state.energy);
        }
        state.energy -= energy_cost;
        double food_cost = 0.20 * barracks_levels * step;
        if (state.food < food_cost) {
            food_cost = std::max(0.0, state.food);
        }
        state.food -= food_cost;

        // 舰队由军营自动训练，受人口与容量限制。
        double fleet_points = rates.fleet_growth * step;
        const double fleet_capacity = rates.fleet_capacity;
        if (static_cast<double>(state.fleet) + fleet_points > fleet_capacity) {
            fleet_points = std::max(0.0, fleet_capacity - static_cast<double>(state.fleet));
        }
        const double pop_cost = fleet_points * 0.5;
        if (state.population > 3.0) {
            state.population = std::max(3.0, state.population - pop_cost);
        }
        state.fleet += static_cast<int>(fleet_points);
        if (fleet_points > 0.0) {
            state.fleet += (Random01(state.rng) < (fleet_points - std::floor(fleet_points))) ? 1 : 0;
        }

        // 人口增长。
        double growth = rates.population_growth;
        if (growth > 0.0) {
            const double capacity = rates.population_capacity;
            if (state.population >= capacity) {
                growth = 0.0;
            }
        }
        state.population = std::max(0.0, state.population + growth * step);
        if (state.population > rates.population_capacity) {
            state.population = rates.population_capacity;
        }

        state.energy = std::max(0.0, state.energy);
        state.minerals = std::max(0.0, state.minerals);
        state.food = std::max(0.0, state.food);
        state.alloy = std::max(0.0, state.alloy);
        state.fleet = std::clamp(state.fleet, 0, FleetCapacity(state) * 2);
        state.play_seconds += step;
        state.unix_time += step;

        // 市场每 15 分钟换一批合同和黑市库存。
        EnsureMarket(state);
        EnsureCommandCards(state);

        // 远征返航：收益和损失在出发时已确定，这里只负责揭示并结算。
        for (auto expedition_it = state.expeditions.begin();
             expedition_it != state.expeditions.end();) {
            if (expedition_it->return_seconds > state.play_seconds + 1e-9) {
                ++expedition_it;
                continue;
            }
            ExpeditionReport report;
            report.destination = expedition_it->destination;
            report.stance = expedition_it->stance;
            report.outcome = expedition_it->outcome;
            report.relics = expedition_it->relics;
            report.cores = expedition_it->cores;
            report.data_cores = expedition_it->data_cores;
            report.tokens = expedition_it->tokens;
            report.research = expedition_it->research;
            report.influence = expedition_it->influence;
            report.fleet_loss = expedition_it->fleet_loss;
            report.return_seconds = state.play_seconds;

            const int survivors = std::max(0, expedition_it->fleet - expedition_it->fleet_loss);
            state.fleet = std::min(FleetCapacity(state) * 2, state.fleet + survivors);
            state.relics += std::max(0, expedition_it->relics);
            state.cores += std::max(0, expedition_it->cores);
            state.data_cores += std::max(0, expedition_it->data_cores);
            state.tokens += std::max(0.0, expedition_it->tokens);
            state.research += std::max(0.0, expedition_it->research);
            state.influence += std::max(0.0, expedition_it->influence);
            AddMorale(state, static_cast<double>(
                                  ExpeditionMoraleDelta(expedition_it->outcome)));
            state.reputation = std::clamp(
                state.reputation + ExpeditionReputationDelta(expedition_it->outcome), 0, 100);
            ++state.expeditions_done;

            state.expedition_reports.push_back(report);
            while (state.expedition_reports.size() >
                   static_cast<std::size_t>(kMaxExpeditionReports)) {
                state.expedition_reports.erase(state.expedition_reports.begin());
            }
            expedition_it = state.expeditions.erase(expedition_it);
        }

        // 事件卡：每 1.5-3 分钟来一张；超过队列上限时，最旧的一张按保守选项结算。
        if (state.play_seconds + 1e-9 >= state.next_event_seconds) {
            const int event_count = static_cast<int>(EventId::kCount);
            const int id = std::min(event_count - 1,
                                    static_cast<int>(Random01(state.rng) * event_count));
            state.events.push_back(GameEvent{id});
            state.next_event_seconds = state.play_seconds + 90.0 + Random01(state.rng) * 90.0;
            while (state.events.size() > static_cast<std::size_t>(kMaxPendingEvents)) {
                ResolveEvent(state, 0, false);
            }
        }
        if (state.population > PopulationCapacity(state)) {
            state.population = PopulationCapacity(state);
        }
    }
    state.day = 1 + static_cast<int>(state.play_seconds / 86400.0);
}

Cost BuildingUpgradeCost(const GameState& state, BuildingId id) {
    const int level = std::max(0, state.buildings[static_cast<int>(id)]);
    Cost base;
    switch (id) {
    case BuildingId::kPower:
        base.minerals = 28.0;
        break;
    case BuildingId::kMine:
        base.minerals = 32.0;
        base.energy = 10.0;
        break;
    case BuildingId::kFarm:
        base.minerals = 26.0;
        base.energy = 6.0;
        break;
    case BuildingId::kFactory:
        base.minerals = 42.0;
        base.alloy = 10.0;
        break;
    case BuildingId::kLab:
        base.minerals = 36.0;
        base.energy = 22.0;
        break;
    case BuildingId::kBarracks:
        base.minerals = 30.0;
        base.alloy = 16.0;
        break;
    case BuildingId::kCount:
    default:
        break;
    }
    return ScaleCost(base, Pow(1.55, level));
}

bool CanBuild(const GameState& state, BuildingId id) {
    if (static_cast<int>(id) < 0 || static_cast<int>(id) >= static_cast<int>(BuildingId::kCount)) {
        return false;
    }
    if (state.buildings[static_cast<int>(id)] >= kMaxBuildingLevel) {
        return false;
    }
    return CanPay(state, BuildingUpgradeCost(state, id));
}

bool Build(GameState& state, BuildingId id) {
    if (!CanBuild(state, id)) {
        return false;
    }
    Pay(state, BuildingUpgradeCost(state, id));
    ++state.buildings[static_cast<int>(id)];
    return true;
}

Cost CapitalUpgradeCost(const GameState& state) {
    Cost base;
    base.minerals = 120.0;
    base.alloy = 40.0;
    return ScaleCost(base, Pow(1.7, std::max(0, state.capital_level - 1)) *
                                (state.capital_level > 6 ? 1.35 : 1.0));
}

bool UpgradeCapital(GameState& state) {
    if (state.capital_level >= kMaxCapitalLevel) {
        return false;
    }
    const Cost cost = CapitalUpgradeCost(state);
    if (!CanPay(state, cost)) {
        return false;
    }
    Pay(state, cost);
    ++state.capital_level;
    return true;
}

Cost ColonyUpgradeCost(const GameState& state, std::size_t index) {
    Cost base;
    base.minerals = 70.0;
    base.alloy = 25.0;
    if (index < state.colonies.size()) {
        return ScaleCost(base, Pow(1.5, std::max(0, state.colonies[index].level - 1)));
    }
    return ScaleCost(base, 1.0);
}

bool UpgradeColony(GameState& state, std::size_t index) {
    if (index >= state.colonies.size()) {
        return false;
    }
    if (state.colonies[index].level >= kMaxColonyLevel) {
        return false;
    }
    const Cost cost = ColonyUpgradeCost(state, index);
    if (!CanPay(state, cost)) {
        return false;
    }
    Pay(state, cost);
    ++state.colonies[index].level;
    return true;
}

bool ChooseFaction(GameState& state, int faction) {
    if (state.faction != static_cast<int>(FactionId::kNone)) {
        return false;
    }
    if (faction <= static_cast<int>(FactionId::kNone) ||
        faction >= static_cast<int>(FactionId::kCount)) {
        return false;
    }
    state.faction = faction;
    if (faction == static_cast<int>(FactionId::kCompact)) {
        AddMorale(state, 8.0);
    }
    if (faction == static_cast<int>(FactionId::kChoir)) {
        state.research += 40.0;
    }
    return true;
}

bool HasFaction(const GameState& state, FactionId faction) {
    return state.faction == static_cast<int>(faction);
}

Cost WonderCost(const GameState& state, int wonder) {
    (void)state;
    Cost cost;
    switch (wonder) {
    case kWonderDysonRing:
        cost.minerals = 600.0;
        cost.alloy = 260.0;
        break;
    case kWonderKnowledgeArk:
        cost.minerals = 700.0;
        cost.alloy = 320.0;
        break;
    case kWonderOrbitalDock:
        cost.minerals = 800.0;
        cost.alloy = 400.0;
        break;
    default:
        break;
    }
    return cost;
}

bool HasWonder(const GameState& state, int wonder) {
    if (wonder < 0 || wonder >= kWonderCount) {
        return false;
    }
    return (state.wonders & (1u << wonder)) != 0;
}

bool CanBuildWonder(const GameState& state, int wonder) {
    if (wonder < 0 || wonder >= kWonderCount || HasWonder(state, wonder)) {
        return false;
    }
    if (wonder == kWonderKnowledgeArk && state.research < 300.0) {
        return false;
    }
    return CanPay(state, WonderCost(state, wonder));
}

bool BuildWonder(GameState& state, int wonder) {
    if (!CanBuildWonder(state, wonder)) {
        return false;
    }
    Pay(state, WonderCost(state, wonder));
    state.wonders |= (1u << wonder);
    AddMorale(state, 5.0);
    return true;
}

int WonderCount(const GameState& state) {
    int count = 0;
    for (int i = 0; i < kWonderCount; ++i) {
        if (HasWonder(state, i)) {
            ++count;
        }
    }
    return count;
}

// --- 时代 -----------------------------------------------------------------

int EraIndex(const GameState& state) {
    const double seconds = std::max(0.0, state.play_seconds);
    if (seconds < 600.0) {
        return 0;   // 苏醒：教学
    }
    if (seconds < 7200.0) {
        return 1;   // 第一时代：立足
    }
    if (seconds < 14400.0) {
        return 2;   // 第二时代：扩张
    }
    if (seconds < 21000.0) {
        return 3;   // 第三时代：审计
    }
    return 4;       // 结算
}

double NextEraIn(const GameState& state) {
    const double seconds = std::max(0.0, state.play_seconds);
    const double boundaries[4] = {600.0, 7200.0, 14400.0, 21000.0};
    for (double boundary : boundaries) {
        if (seconds < boundary) {
            return boundary - seconds;
        }
    }
    return 0.0;
}

// --- 市场、合同与黑市 -----------------------------------------------------

int MarketPhase(const GameState& state) {
    return std::max(0, static_cast<int>(std::max(0.0, state.play_seconds) /
                                         kMarketPhaseSeconds));
}

double MarketSellPrice(const GameState& state, int resource) {
    if (resource < 0 || resource >= kTradeResourceCount) {
        return 0.0;
    }
    const int phase = MarketPhase(state);
    const double noise = 0.85 + 0.30 * Hash01(phase, 601 + resource);
    double price = BaseSellPrice(resource) * noise * ReputationFactor(state);
    if (state.faction == static_cast<int>(FactionId::kGuild)) {
        price *= 1.08;
    }
    return std::max(0.01, price);
}

double MarketBuyPrice(const GameState& state, int resource) {
    if (resource < 0 || resource >= kTradeResourceCount) {
        return 0.0;
    }
    const int phase = MarketPhase(state);
    const double noise = 0.85 + 0.30 * Hash01(phase, 601 + resource);
    double price = BaseSellPrice(resource) * noise * 2.35 / ReputationFactor(state);
    if (state.faction == static_cast<int>(FactionId::kGuild)) {
        price *= 0.92;
    }
    return std::max(0.01, price);
}

void RefreshMarket(GameState& state, int phase) {
    state.market_phase = phase;
    state.contracts.clear();
    state.black_market.clear();

    // 三份轮换合同。数量、赏金和需要的资源都由相位决定，刷新前不会变。
    for (int i = 0; i < kMaxContracts; ++i) {
        TradeContract contract;
        contract.resource = std::min(
            kTradeResourceCount - 1,
            static_cast<int>(Hash01(phase, 101 + i * 13) * kTradeResourceCount));
        contract.amount = 60.0 + std::floor(Hash01(phase, 201 + i * 13) * 90.0);
        const double unit = MarketSellPrice(state, contract.resource);
        contract.reward_tokens =
            contract.amount * unit * (1.25 + 0.25 * Hash01(phase, 301 + i * 13)) + 8.0;
        if (state.faction == static_cast<int>(FactionId::kGuild)) {
            contract.reward_tokens *= 1.20;
        }
        contract.reward_influence = 2.0 + std::floor(Hash01(phase, 401 + i * 13) * 5.0);
        contract.done = false;
        state.contracts.push_back(contract);
    }

    auto add_offer = [&state, phase](int kind, int amount, double base_price, int salt) {
        BlackMarketOffer offer;
        offer.kind = kind;
        offer.amount = amount;
        offer.stock = 1;
        double price = base_price * (0.90 + 0.20 * Hash01(phase, salt)) / ReputationFactor(state);
        if (state.faction == static_cast<int>(FactionId::kGuild)) {
            price *= 0.90;
        }
        offer.price = std::max(1.0, price);
        state.black_market.push_back(offer);
    };

    const int rare_kind = std::min(2, static_cast<int>(Hash01(phase, 501) * 3.0));
    const double rare_price = rare_kind == 0 ? 150.0 : (rare_kind == 1 ? 300.0 : 200.0);
    add_offer(rare_kind, 1, rare_price, 502);

    const int service_kind = 3 + std::min(2, static_cast<int>(Hash01(phase, 503) * 3.0));
    const int service_amount =
        service_kind == 3 ? 6 : (service_kind == 4 ? 160 : 45);
    const double service_price =
        service_kind == 3 ? 110.0 : (service_kind == 4 ? 95.0 : 80.0);
    add_offer(service_kind, service_amount, service_price, 504);

    const int third_kind =
        (rare_kind + 1 + static_cast<int>(Hash01(phase, 505) * 2.0)) % 3;
    const double third_price = third_kind == 0 ? 150.0 : (third_kind == 1 ? 300.0 : 200.0);
    add_offer(third_kind, 1, third_price, 506);
}

void EnsureMarket(GameState& state) {
    const int phase = MarketPhase(state);
    if (state.market_phase != phase || state.contracts.empty() ||
        state.black_market.empty()) {
        RefreshMarket(state, phase);
    }
}

bool SellResource(GameState& state, int resource, double amount) {
    EnsureMarket(state);
    if (resource < 0 || resource >= kTradeResourceCount || amount <= 0.0) {
        return false;
    }
    double* wallet = nullptr;
    switch (resource) {
    case 0: wallet = &state.energy; break;
    case 1: wallet = &state.minerals; break;
    case 2: wallet = &state.food; break;
    case 3: wallet = &state.alloy; break;
    default: return false;
    }
    if (*wallet + 1e-9 < amount) {
        return false;
    }
    *wallet = std::max(0.0, *wallet - amount);
    state.tokens += MarketSellPrice(state, resource) * amount;
    return true;
}

bool BuyResource(GameState& state, int resource, double amount) {
    EnsureMarket(state);
    if (resource < 0 || resource >= kTradeResourceCount || amount <= 0.0) {
        return false;
    }
    const double cost = MarketBuyPrice(state, resource) * amount;
    if (state.tokens + 1e-9 < cost) {
        return false;
    }
    state.tokens = std::max(0.0, state.tokens - cost);
    switch (resource) {
    case 0: state.energy += amount; break;
    case 1: state.minerals += amount; break;
    case 2: state.food += amount; break;
    case 3: state.alloy += amount; break;
    default: return false;
    }
    return true;
}

bool FulfillContract(GameState& state, std::size_t index) {
    EnsureMarket(state);
    if (index >= state.contracts.size()) {
        return false;
    }
    TradeContract& contract = state.contracts[index];
    if (contract.done) {
        return false;
    }
    double* wallet = nullptr;
    switch (contract.resource) {
    case 0: wallet = &state.energy; break;
    case 1: wallet = &state.minerals; break;
    case 2: wallet = &state.food; break;
    case 3: wallet = &state.alloy; break;
    default: return false;
    }
    if (*wallet + 1e-9 < contract.amount) {
        return false;
    }
    *wallet = std::max(0.0, *wallet - contract.amount);
    state.tokens += contract.reward_tokens;
    state.influence += contract.reward_influence;
    state.reputation = std::min(100, state.reputation + 2);
    ++state.contracts_done;
    contract.done = true;
    return true;
}

bool BuyBlackMarket(GameState& state, std::size_t index) {
    EnsureMarket(state);
    if (index >= state.black_market.size()) {
        return false;
    }
    BlackMarketOffer& offer = state.black_market[index];
    if (offer.stock <= 0 || state.tokens + 1e-9 < offer.price) {
        return false;
    }
    state.tokens = std::max(0.0, state.tokens - offer.price);
    --offer.stock;
    switch (offer.kind) {
    case 0: state.relics += 1; break;
    case 1: state.cores += 1; break;
    case 2: state.data_cores += 1; break;
    case 3:
        state.fleet = std::min(FleetCapacity(state) * 2,
                               state.fleet + std::max(0, offer.amount));
        break;
    case 4: state.research += static_cast<double>(std::max(0, offer.amount)); break;
    case 5: state.influence += static_cast<double>(std::max(0, offer.amount)); break;
    default: break;
    }
    return true;
}

// --- 指令卡 ---------------------------------------------------------------

bool CommandCardsAvailable(const GameState& state) {
    return state.cards_available && state.card_phase == MarketPhase(state) &&
           state.play_seconds <= state.card_expires + 1e-9;
}

double CommandCardSecondsLeft(const GameState& state) {
    if (!CommandCardsAvailable(state)) {
        return 0.0;
    }
    return std::max(0.0, state.card_expires - state.play_seconds);
}

int CommandCardOffer(const GameState& state, int index) {
    if (index < 0 || index >= kCommandCardChoices) {
        return -1;
    }
    return state.card_offers[index];
}

bool ChooseCommandCard(GameState& state, int index) {
    EnsureCommandCards(state);
    if (!CommandCardsAvailable(state) || index < 0 || index >= kCommandCardChoices) {
        return false;
    }
    const int card = state.card_offers[index];
    if (card < 0 || card >= static_cast<int>(CommandCardId::kCount)) {
        return false;
    }
    switch (static_cast<CommandCardId>(card)) {
    case CommandCardId::kEmergencyLevy:
        state.fleet = std::min(FleetCapacity(state) * 2, state.fleet + 5);
        state.population = std::max(3.0, state.population - 3.0);
        AddMorale(state, -8.0);
        break;
    case CommandCardId::kResearchBreakthrough:
        state.research += 150.0;
        state.energy = std::max(0.0, state.energy - 80.0);
        break;
    case CommandCardId::kColonySubsidy:
        state.minerals += 220.0;
        state.alloy += 100.0;
        state.food = std::max(0.0, state.food - 60.0);
        break;
    case CommandCardId::kEmergencyLoan:
        state.tokens += 260.0;
        state.reputation = std::max(0, state.reputation - 3);
        AddMorale(state, -10.0);
        break;
    case CommandCardId::kTalentIntake:
        state.population = std::min(PopulationCapacity(state), state.population + 3.0);
        state.research = std::max(0.0, state.research - 60.0);
        break;
    case CommandCardId::kDiplomaticRound:
        state.influence += 90.0;
        state.alloy = std::max(0.0, state.alloy - 90.0);
        break;
    case CommandCardId::kBlackMarketRun:
        if (state.tokens + 1e-9 < 100.0) {
            return false;
        }
        state.tokens -= 100.0;
        state.data_cores += 1;
        break;
    case CommandCardId::kSilenceListening:
        state.data_cores += 1;
        state.energy = std::max(0.0, state.energy - 60.0);
        AddMorale(state, -4.0);
        break;
    case CommandCardId::kCount:
    default:
        return false;
    }
    state.cards_available = false;
    ++state.cards_taken;
    return true;
}

// --- 远征 -----------------------------------------------------------------

int ExpeditionFleetCost(ExpeditionDestination destination, ExpeditionStance stance) {
    int cost = 0;
    switch (destination) {
    case ExpeditionDestination::kFrontier: cost = 4; break;
    case ExpeditionDestination::kRuins: cost = 6; break;
    case ExpeditionDestination::kAnomaly: cost = 8; break;
    case ExpeditionDestination::kArk: cost = 7; break;
    case ExpeditionDestination::kCount:
    default: return 0;
    }
    if (stance == ExpeditionStance::kDeep) {
        cost += 2;
    }
    return cost;
}

double ExpeditionDurationSeconds(ExpeditionDestination destination) {
    switch (destination) {
    case ExpeditionDestination::kFrontier: return 600.0;    // 10 分钟
    case ExpeditionDestination::kRuins: return 1200.0;      // 20 分钟
    case ExpeditionDestination::kAnomaly: return 1800.0;    // 30 分钟
    case ExpeditionDestination::kArk: return 1500.0;        // 25 分钟
    case ExpeditionDestination::kCount:
    default: return 3600.0;
    }
}

int ActiveExpeditionCount(const GameState& state) {
    return static_cast<int>(state.expeditions.size());
}

bool LaunchExpedition(GameState& state, ExpeditionDestination destination,
                      ExpeditionStance stance) {
    if (destination < ExpeditionDestination::kFrontier ||
        destination >= ExpeditionDestination::kCount || stance < ExpeditionStance::kCautious ||
        stance >= ExpeditionStance::kCount) {
        return false;
    }
    if (ActiveExpeditionCount(state) >= kMaxExpeditions) {
        return false;
    }
    const int cost = ExpeditionFleetCost(destination, stance);
    if (cost <= 0 || state.fleet < cost) {
        return false;
    }

    Expedition expedition;
    expedition.destination = static_cast<int>(destination);
    expedition.stance = static_cast<int>(stance);
    expedition.fleet = cost;
    expedition.depart_seconds = state.play_seconds;
    expedition.return_seconds = state.play_seconds + ExpeditionDurationSeconds(destination);

    // 结果在出发时确定。玩家只能等返航报告，不能靠反复读档改结果。
    double weight[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
    switch (destination) {
    case ExpeditionDestination::kFrontier:
        weight[0] = 0.08; weight[1] = 0.55; weight[2] = 0.25; weight[3] = 0.09; weight[4] = 0.03;
        break;
    case ExpeditionDestination::kRuins:
        weight[0] = 0.15; weight[1] = 0.45; weight[2] = 0.25; weight[3] = 0.10; weight[4] = 0.05;
        break;
    case ExpeditionDestination::kAnomaly:
        weight[0] = 0.20; weight[1] = 0.40; weight[2] = 0.20; weight[3] = 0.13; weight[4] = 0.07;
        break;
    case ExpeditionDestination::kArk:
        weight[0] = 0.12; weight[1] = 0.48; weight[2] = 0.25; weight[3] = 0.10; weight[4] = 0.05;
        break;
    default:
        return false;
    }
    if (stance == ExpeditionStance::kCautious) {
        weight[0] -= 0.05;
        weight[1] += 0.02;
        weight[2] += 0.04;
        weight[3] += 0.01;
        weight[4] -= 0.02;
    } else {
        weight[0] += 0.05;
        weight[1] += 0.01;
        weight[2] -= 0.03;
        weight[3] += 0.02;
        weight[4] += 0.01;
    }
    double sum = 0.0;
    for (double& value : weight) {
        value = std::max(0.0, value);
        sum += value;
    }
    if (sum <= 0.0) {
        return false;
    }
    double roll = Random01(state.rng);
    int outcome = 4;
    double cumulative = 0.0;
    for (int i = 0; i < 5; ++i) {
        cumulative += weight[i] / sum;
        if (roll <= cumulative) {
            outcome = i;
            break;
        }
    }
    expedition.outcome = outcome;

    const double outcome_loot[5] = {2.0, 1.0, 0.35, 0.0, 0.0};
    const double stance_loot =
        stance == ExpeditionStance::kCautious ? 0.80 : 1.30;
    const double loot = outcome_loot[outcome] * stance_loot;

    double base_relic = 0.0;
    double base_core = 0.0;
    double base_data = 0.0;
    double base_tokens = 0.0;
    double base_research = 0.0;
    double base_influence = 0.0;
    switch (destination) {
    case ExpeditionDestination::kFrontier:
        base_relic = 0.15; base_data = 0.10; base_tokens = 40.0; base_research = 25.0; base_influence = 2.0;
        break;
    case ExpeditionDestination::kRuins:
        base_relic = 1.00; base_core = 0.03; base_data = 0.55; base_tokens = 60.0; base_research = 70.0; base_influence = 4.0;
        break;
    case ExpeditionDestination::kAnomaly:
        base_relic = 0.35; base_core = 0.45; base_data = 0.35; base_tokens = 95.0; base_research = 95.0; base_influence = 5.0;
        break;
    case ExpeditionDestination::kArk:
        base_relic = 0.25; base_core = 0.08; base_data = 1.00; base_tokens = 80.0; base_research = 75.0; base_influence = 7.0;
        break;
    default:
        break;
    }

    auto grant_units = [&state](double expected) {
        if (expected <= 0.0) {
            return 0;
        }
        int whole = static_cast<int>(std::floor(expected));
        if (Random01(state.rng) < expected - static_cast<double>(whole)) {
            ++whole;
        }
        return whole;
    };
    expedition.relics = grant_units(base_relic * loot);
    expedition.cores = grant_units(base_core * loot);
    expedition.data_cores = grant_units(base_data * loot);
    double token_multiplier = 1.0;
    if (state.faction == static_cast<int>(FactionId::kGuild)) {
        token_multiplier = 1.20;
    }
    expedition.tokens = base_tokens * loot * token_multiplier;
    expedition.research = base_research * loot;
    expedition.influence = base_influence * loot;

    int loss = 0;
    switch (outcome) {
    case 0:
        loss = destination == ExpeditionDestination::kFrontier ? 0 : 1;
        break;
    case 1:
        loss = destination == ExpeditionDestination::kFrontier
                   ? 0
                   : (destination == ExpeditionDestination::kAnomaly ? 2 : 1);
        break;
    case 2:
        loss = std::max(1, cost / 3);
        break;
    case 3:
        loss = std::max(1, cost / 2);
        break;
    case 4:
        loss = cost;
        break;
    default:
        break;
    }
    if (stance == ExpeditionStance::kCautious) {
        loss = std::max(0, loss - 1);
    } else {
        loss = std::min(cost, loss + 1);
    }
    expedition.fleet_loss = loss;

    state.fleet = std::max(0, state.fleet - cost);
    state.expeditions.push_back(expedition);
    return true;
}

void ClearExpeditionReports(GameState& state) {
    state.expedition_reports.clear();
}

double TechCost(const GameState& state, TechId id) {
    const int level = std::max(0, state.techs[static_cast<int>(id)]);
    double cost = 40.0 * Pow(2.1, level);
    if (HasWonder(state, kWonderKnowledgeArk)) {
        cost *= 0.75;
    }
    return cost;
}

bool CanBuyTech(const GameState& state, TechId id) {
    if (static_cast<int>(id) < 0 || static_cast<int>(id) >= static_cast<int>(TechId::kCount)) {
        return false;
    }
    if (state.techs[static_cast<int>(id)] >= kMaxTechLevel) {
        return false;
    }
    return state.research >= TechCost(state, id);
}

bool BuyTech(GameState& state, TechId id) {
    if (!CanBuyTech(state, id)) {
        return false;
    }
    state.research -= TechCost(state, id);
    ++state.techs[static_cast<int>(id)];
    return true;
}

Cost FleetBuildCost(const GameState& state, int fleet_points) {
    const double points = static_cast<double>(std::max(1, fleet_points));
    double discount = 1.0 - 0.08 * static_cast<double>(state.techs[static_cast<int>(TechId::kMilitary)]);
    if (HasFaction(state, FactionId::kGuild)) {
        discount -= 0.15;
    }
    Cost cost;
    cost.alloy = 2.2 * points * std::max(0.4, discount);
    cost.food = 1.2 * points;
    return cost;
}

bool BuildFleet(GameState& state, int fleet_points) {
    if (fleet_points <= 0 || state.buildings[static_cast<int>(BuildingId::kBarracks)] <= 0) {
        return false;
    }
    if (state.population < 3.0 + 0.5 * static_cast<double>(fleet_points)) {
        return false;
    }
    if (state.fleet + fleet_points > FleetCapacity(state)) {
        return false;
    }
    const Cost cost = FleetBuildCost(state, fleet_points);
    if (!CanPay(state, cost)) {
        return false;
    }
    Pay(state, cost);
    state.population = std::max(3.0, state.population - 0.5 * static_cast<double>(fleet_points));
    state.fleet += fleet_points;
    return true;
}

Cost ColonizeCost(const GameState& state) {
    const double discount = 1.0 - 0.10 * static_cast<double>(state.techs[static_cast<int>(TechId::kColonization)]);
    Cost base;
    base.alloy = 60.0;
    base.food = 40.0;
    Cost cost = ScaleCost(base, Pow(1.55, static_cast<int>(state.colonies.size())));
    cost.alloy *= std::max(0.5, discount);
    cost.food *= std::max(0.5, discount);
    return cost;
}

bool CanColonize(const GameState& state) {
    if (state.population < 8.0) {
        return false;
    }
    return CanPay(state, ColonizeCost(state));
}

bool Colonize(GameState& state) {
    if (!CanColonize(state)) {
        return false;
    }
    Pay(state, ColonizeCost(state));
    state.population = std::max(3.0, state.population - 4.0);
    state.colonies.push_back(Colony{1});
    return true;
}

void AddEvent(GameState& state, EventId id) {
    const int event_id = static_cast<int>(id);
    if (event_id < 0 || event_id >= static_cast<int>(EventId::kCount)) {
        return;
    }
    state.events.push_back(GameEvent{event_id});
    while (state.events.size() > static_cast<std::size_t>(kMaxPendingEvents)) {
        ResolveEvent(state, 0, false);
    }
}

bool ResolveEvent(GameState& state, std::size_t index, bool choice_a) {
    if (index >= state.events.size()) {
        return false;
    }
    const int event_id = state.events[index].id;
    state.events.erase(state.events.begin() + static_cast<std::ptrdiff_t>(index));

    switch (static_cast<EventId>(event_id)) {
    case EventId::kPirateRaid:
        if (choice_a) {
            SpendResources(state, 0.0, 0.0, 40.0, 20.0);
            AddMorale(state, 6.0);
            GainResources(state, 0.0, 0.0, 0.0, 30.0, 0.0);
        } else {
            SpendResources(state, 0.0, 50.0, 0.0, 0.0);
            AddMorale(state, -4.0);
        }
        break;
    case EventId::kIonStorm:
        if (choice_a) {
            SpendResources(state, 60.0, 0.0, 0.0, 0.0);
            state.research += 90.0;
            AddMorale(state, -3.0);
        } else {
            SpendResources(state, 20.0, 0.0, 0.0, 0.0);
            AddMorale(state, 1.0);
        }
        break;
    case EventId::kRefugeeFleet:
        if (choice_a) {
            SpendResources(state, 0.0, 0.0, 80.0, 0.0);
            state.population += 6.0;
            AddMorale(state, 8.0);
        } else {
            SpendResources(state, 0.0, 0.0, 30.0, 0.0);
            state.research += 30.0;
            AddMorale(state, 2.0);
        }
        break;
    case EventId::kBlackMarket:
        if (choice_a) {
            SpendResources(state, 0.0, 120.0, 0.0, 0.0);
            state.research += 180.0;
        } else {
            GainResources(state, 0.0, 0.0, 0.0, 45.0, 0.0);
            AddMorale(state, 4.0);
        }
        break;
    case EventId::kMineCollapse:
        if (choice_a) {
            SpendResources(state, 40.0, 70.0, 0.0, 0.0);
            AddMorale(state, 10.0);
        } else {
            SpendResources(state, 0.0, 30.0, 0.0, 0.0);
            AddMorale(state, -5.0);
        }
        break;
    case EventId::kSilenceScan:
        if (choice_a) {
            SpendResources(state, 50.0, 0.0, 0.0, 0.0);
            state.research += 40.0;
            AddMorale(state, 2.0);
        } else {
            state.research += 120.0;
            state.data_cores += 1;
            state.fleet = std::max(0, state.fleet - 5);
            AddMorale(state, -4.0);
        }
        break;
    case EventId::kDeserters:
        if (choice_a) {
            SpendResources(state, 0.0, 0.0, 60.0, 0.0);
            state.fleet = std::min(FleetCapacity(state), state.fleet + 12);
            AddMorale(state, -3.0);
        } else {
            GainResources(state, 0.0, 0.0, 0.0, 60.0, 0.0);
            AddMorale(state, 2.0);
        }
        break;
    case EventId::kTradeBoom:
        if (choice_a) {
            SpendResources(state, 0.0, 60.0, 0.0, 0.0);
            GainResources(state, 0.0, 0.0, 0.0, 110.0, 0.0);
            state.tokens += 25.0;
            state.reputation = std::min(100, state.reputation + 1);
            AddMorale(state, 2.0);
        } else {
            state.research += 50.0;
            AddMorale(state, 1.0);
        }
        break;
    case EventId::kEchoSignal:
        if (choice_a) {
            SpendResources(state, 60.0, 0.0, 0.0, 0.0);
            state.data_cores += 1;
            AddMorale(state, 2.0);
        } else {
            state.tokens += 50.0;
            AddMorale(state, -1.0);
        }
        break;
    case EventId::kArkDistress:
        if (choice_a) {
            SpendResources(state, 0.0, 0.0, 100.0, 20.0);
            state.population = std::min(PopulationCapacity(state), state.population + 4.0);
            state.influence += 8.0;
            AddMorale(state, 5.0);
        } else {
            GainResources(state, 0.0, 0.0, 0.0, 80.0, 0.0);
            state.tokens += 30.0;
            AddMorale(state, -4.0);
        }
        break;
    case EventId::kSilentTide:
        if (choice_a) {
            SpendResources(state, 80.0, 0.0, 0.0, 0.0);
            state.research += 50.0;
            AddMorale(state, 1.0);
        } else {
            state.fleet = std::max(0, state.fleet - 6);
            state.cores += 1;
            state.research += 80.0;
            AddMorale(state, -5.0);
        }
        break;
    case EventId::kCount:
    default:
        break;
    }

    if (state.population > PopulationCapacity(state)) {
        state.population = PopulationCapacity(state);
    }
    state.energy = std::max(0.0, state.energy);
    state.minerals = std::max(0.0, state.minerals);
    state.food = std::max(0.0, state.food);
    state.alloy = std::max(0.0, state.alloy);
    return true;
}

int NeutralDefense(const GameState& state) {
    return 10 + 6 * TotalColonyLevels(state) + 4 * state.battles_won +
           3 * state.capital_level;
}

BattleResult AttackNeutral(GameState& attacker) {
    BattleResult result;
    const int defense = NeutralDefense(attacker);
    if (attacker.fleet < 5) {
        return result;
    }
    const double power =
        static_cast<double>(attacker.fleet) *
        (1.0 + 0.08 * static_cast<double>(attacker.techs[static_cast<int>(TechId::kMilitary)]));
    if (power >= static_cast<double>(defense)) {
        result.won = true;
        result.captured = true;
        result.attacker_loss = std::max(1, defense / 3);
        result.loot_minerals = (40.0 + 2.0 * static_cast<double>(defense)) * LootMultiplier(attacker);
        result.loot_alloy = (15.0 + 1.0 * static_cast<double>(defense)) * LootMultiplier(attacker);
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        attacker.colonies.push_back(Colony{1});
        ++attacker.battles_won;
        AddMorale(attacker, 2.0);
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.6));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        ++attacker.battles_lost;
        AddMorale(attacker, -6.0);
    }
    return result;
}

int SectorDefense(const GameState& state, SectorTargetId target) {
    switch (target) {
    case SectorTargetId::kMiningBelt:
        return 18 + 4 * TotalColonyLevels(state) + 3 * state.battles_won;
    case SectorTargetId::kSilentOutpost:
        return 55 + 7 * TotalColonyLevels(state) + 5 * state.battles_won;
    case SectorTargetId::kCount:
    default:
        return 1000000;
    }
}

BattleResult AttackSector(GameState& attacker, SectorTargetId target) {
    BattleResult result;
    const int defense = SectorDefense(attacker, target);
    if (attacker.fleet < 5 || target == SectorTargetId::kCount) {
        return result;
    }
    const double power =
        static_cast<double>(attacker.fleet) *
        (1.0 + 0.08 * static_cast<double>(attacker.techs[static_cast<int>(TechId::kMilitary)]));
    if (power >= static_cast<double>(defense)) {
        result.won = true;
        result.attacker_loss = std::max(1, defense / 4);
        if (target == SectorTargetId::kMiningBelt) {
            result.loot_minerals = (180.0 + 4.0 * static_cast<double>(defense)) * LootMultiplier(attacker);
            result.loot_alloy = 35.0 * LootMultiplier(attacker);
        } else {
            result.loot_alloy = (80.0 + 2.0 * static_cast<double>(defense)) * LootMultiplier(attacker);
            result.loot_research = (160.0 + 2.0 * static_cast<double>(defense)) * LootMultiplier(attacker);
            AddMorale(attacker, -4.0);
        }
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        attacker.research += result.loot_research;
        ++attacker.battles_won;
        if (target == SectorTargetId::kMiningBelt) {
            AddMorale(attacker, 2.0);
        }
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.55));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        ++attacker.battles_lost;
        AddMorale(attacker, -5.0);
    }
    return result;
}

BattleResult RaidPlayer(GameState& attacker, GameState& defender) {
    BattleResult result;
    const double attack_power =
        static_cast<double>(attacker.fleet) *
        (1.0 + 0.08 * static_cast<double>(attacker.techs[static_cast<int>(TechId::kMilitary)]));
    const double defense_power =
        6.0 + 0.62 * static_cast<double>(defender.fleet) +
        4.0 * static_cast<double>(TotalColonyLevels(defender)) +
        3.0 * static_cast<double>(defender.capital_level);

    if (attack_power >= defense_power) {
        result.won = true;
        result.attacker_loss = std::max(1, static_cast<int>(defense_power / 6.0));
        result.defender_loss = std::max(1, static_cast<int>(defender.fleet * 0.25));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        defender.fleet = std::max(0, defender.fleet - result.defender_loss);

        const double base_minerals = defender.minerals * 0.18;
        const double base_alloy = defender.alloy * 0.18;
        const double base_research = defender.research * 0.10;
        defender.minerals = std::max(0.0, defender.minerals - base_minerals);
        defender.alloy = std::max(0.0, defender.alloy - base_alloy);
        defender.research = std::max(0.0, defender.research - base_research);
        result.loot_minerals = base_minerals * LootMultiplier(attacker);
        result.loot_alloy = base_alloy * LootMultiplier(attacker);
        result.loot_research = base_research * LootMultiplier(attacker);
        // 国库最多被抢走 10%；星际代币不计入霸业值，但会被抢。
        const double stolen_tokens = std::min(defender.tokens * 0.10, defender.tokens);
        defender.tokens = std::max(0.0, defender.tokens - stolen_tokens);
        result.loot_tokens = stolen_tokens * LootMultiplier(attacker);
        attacker.tokens += result.loot_tokens;
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        attacker.research += result.loot_research;
        attacker.reputation = std::max(0, attacker.reputation - 6);
        ++attacker.battles_won;
        ++defender.battles_lost;
        AddMorale(attacker, 1.0);
        AddMorale(defender, -4.0);
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.5));
        result.defender_loss = std::max(1, static_cast<int>(defender.fleet * 0.15));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        defender.fleet = std::max(0, defender.fleet - result.defender_loss);
        attacker.reputation = std::max(0, attacker.reputation - 2);
        ++attacker.battles_lost;
        ++defender.battles_won;
        AddMorale(attacker, -5.0);
    }
    return result;
}

BattleResult AttackPlayer(GameState& attacker, GameState& defender) {
    BattleResult result;
    const double attack_power =
        static_cast<double>(attacker.fleet) *
        (1.0 + 0.08 * static_cast<double>(attacker.techs[static_cast<int>(TechId::kMilitary)]));
    const double defense_power =
        8.0 + 0.5 * static_cast<double>(defender.fleet) +
        6.0 * static_cast<double>(TotalColonyLevels(defender)) +
        4.0 * static_cast<double>(defender.capital_level) +
        0.06 * static_cast<double>(defender.techs[static_cast<int>(TechId::kMilitary)]);

    if (attack_power >= defense_power) {
        result.won = true;
        result.attacker_loss = std::max(1, static_cast<int>(defense_power / 4.0));
        result.defender_loss = std::max(1, static_cast<int>(defender.fleet * 0.5));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        defender.fleet = std::max(0, defender.fleet - result.defender_loss);
        if (!defender.colonies.empty()) {
            result.captured = true;
            attacker.colonies.push_back(defender.colonies.back());
            defender.colonies.pop_back();
        }
        const double base_minerals = defender.minerals * 0.15;
        const double base_alloy = defender.alloy * 0.15;
        defender.minerals = std::max(0.0, defender.minerals - base_minerals);
        defender.alloy = std::max(0.0, defender.alloy - base_alloy);
        result.loot_minerals = base_minerals * LootMultiplier(attacker);
        result.loot_alloy = base_alloy * LootMultiplier(attacker);
        const double stolen_tokens = std::min(defender.tokens * 0.15, defender.tokens);
        defender.tokens = std::max(0.0, defender.tokens - stolen_tokens);
        result.loot_tokens = stolen_tokens * LootMultiplier(attacker);
        attacker.tokens += result.loot_tokens;
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        attacker.reputation = std::max(0, attacker.reputation - 10);
        ++attacker.battles_won;
        ++defender.battles_lost;
        AddMorale(attacker, 2.0);
        AddMorale(defender, -6.0);
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.6));
        result.defender_loss = std::max(1, static_cast<int>(defender.fleet * 0.2));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        defender.fleet = std::max(0, defender.fleet - result.defender_loss);
        attacker.reputation = std::max(0, attacker.reputation - 4);
        ++attacker.battles_lost;
        ++defender.battles_won;
        AddMorale(attacker, -6.0);
        AddMorale(defender, 2.0);
    }
    return result;
}

double Score(const GameState& state) {
    return state.population * 2.0 + static_cast<double>(state.fleet) * 1.5 +
           static_cast<double>(state.capital_level + TotalColonyLevels(state)) * 30.0 +
           state.research * 0.2 + static_cast<double>(state.battles_won) * 15.0 +
           static_cast<double>(WonderCount(state)) * 120.0 + state.morale * 0.5 +
           state.influence * 0.4 + static_cast<double>(state.relics) * 40.0 +
           static_cast<double>(state.cores) * 80.0 +
           static_cast<double>(state.data_cores) * 50.0 +
           static_cast<double>(state.contracts_done) * 10.0 +
           static_cast<double>(state.expeditions_done) * 12.0 +
           static_cast<double>(state.cards_taken) * 3.0;
}

std::string SerializeState(const GameState& state) {
    std::string text;
    text += "version=5\n";
    text += "energy=" + std::to_string(state.energy) + "\n";
    text += "minerals=" + std::to_string(state.minerals) + "\n";
    text += "food=" + std::to_string(state.food) + "\n";
    text += "alloy=" + std::to_string(state.alloy) + "\n";
    text += "population=" + std::to_string(state.population) + "\n";
    text += "research=" + std::to_string(state.research) + "\n";
    text += "morale=" + std::to_string(state.morale) + "\n";
    text += "influence=" + std::to_string(state.influence) + "\n";
    text += "tokens=" + std::to_string(state.tokens) + "\n";
    text += "relics=" + std::to_string(state.relics) + "\n";
    text += "cores=" + std::to_string(state.cores) + "\n";
    text += "data_cores=" + std::to_string(state.data_cores) + "\n";
    text += "reputation=" + std::to_string(state.reputation) + "\n";
    text += "contracts_done=" + std::to_string(state.contracts_done) + "\n";
    text += "expeditions_done=" + std::to_string(state.expeditions_done) + "\n";
    text += "market_phase=" + std::to_string(state.market_phase) + "\n";
    text += "faction=" + std::to_string(state.faction) + "\n";
    text += "wonders=" + std::to_string(state.wonders) + "\n";
    text += "next_event=" + std::to_string(state.next_event_seconds) + "\n";
    text += "capital=" + std::to_string(state.capital_level) + "\n";
    text += "buildings=" +
            JoinNumbers(state.buildings, static_cast<int>(BuildingId::kCount)) + "\n";
    text += "techs=" + JoinNumbers(state.techs, static_cast<int>(TechId::kCount)) + "\n";
    text += "fleet=" + std::to_string(state.fleet) + "\n";
    text += "wins=" + std::to_string(state.battles_won) + "\n";
    text += "losses=" + std::to_string(state.battles_lost) + "\n";
    text += "colonies=";
    for (std::size_t i = 0; i < state.colonies.size(); ++i) {
        if (i > 0) {
            text.push_back(',');
        }
        text += std::to_string(state.colonies[i].level);
    }
    text += "\n";
    text += "events=";
    for (std::size_t i = 0; i < state.events.size(); ++i) {
        if (i > 0) {
            text.push_back(',');
        }
        text += std::to_string(state.events[i].id);
    }
    text += "\n";
    for (const TradeContract& contract : state.contracts) {
        text += "contract=" + std::to_string(contract.resource) + "|" +
                std::to_string(contract.amount) + "|" +
                std::to_string(contract.reward_tokens) + "|" +
                std::to_string(contract.reward_influence) + "|" +
                std::string(contract.done ? "1" : "0") + "\n";
    }
    for (const BlackMarketOffer& offer : state.black_market) {
        text += "market=" + std::to_string(offer.kind) + "|" +
                std::to_string(offer.stock) + "|" + std::to_string(offer.price) + "|" +
                std::to_string(offer.amount) + "\n";
    }
    text += "command_cards=" + std::to_string(state.card_phase) + "|" +
            std::to_string(state.card_expires) + "|" +
            std::string(state.cards_available ? "1" : "0") + "|" +
            std::to_string(state.card_offers[0]) + "|" +
            std::to_string(state.card_offers[1]) + "|" +
            std::to_string(state.card_offers[2]) + "|" +
            std::to_string(state.cards_taken) + "\n";
    for (const Expedition& expedition : state.expeditions) {
        text += "expedition=" + std::to_string(expedition.destination) + "|" +
                std::to_string(expedition.stance) + "|" + std::to_string(expedition.fleet) +
                "|" + std::to_string(expedition.depart_seconds) + "|" +
                std::to_string(expedition.return_seconds) + "|" +
                std::to_string(expedition.outcome) + "|" +
                std::to_string(expedition.relics) + "|" +
                std::to_string(expedition.cores) + "|" +
                std::to_string(expedition.data_cores) + "|" +
                std::to_string(expedition.tokens) + "|" +
                std::to_string(expedition.research) + "|" +
                std::to_string(expedition.influence) + "|" +
                std::to_string(expedition.fleet_loss) + "\n";
    }
    for (const ExpeditionReport& report : state.expedition_reports) {
        text += "report=" + std::to_string(report.destination) + "|" +
                std::to_string(report.stance) + "|" + std::to_string(report.outcome) + "|" +
                std::to_string(report.relics) + "|" + std::to_string(report.cores) + "|" +
                std::to_string(report.data_cores) + "|" + std::to_string(report.tokens) +
                "|" + std::to_string(report.research) + "|" +
                std::to_string(report.influence) + "|" +
                std::to_string(report.fleet_loss) + "|" +
                std::to_string(report.return_seconds) + "\n";
    }
    text += "unix_time=" + std::to_string(state.unix_time) + "\n";
    text += "play_seconds=" + std::to_string(state.play_seconds) + "\n";
    text += "day=" + std::to_string(state.day) + "\n";
    text += "rng=" + std::to_string(state.rng) + "\n";
    return text;
}

bool DeserializeState(const std::string& text, GameState& out_state) {
    GameState state = MakeNewGame(0.0);
    state.colonies.clear();
    state.buildings[0] = state.buildings[1] = state.buildings[2] = state.buildings[3] =
        state.buildings[4] = state.buildings[5] = 0;
    state.fleet = 0;
    state.contracts.clear();
    state.black_market.clear();
    state.expeditions.clear();
    state.expedition_reports.clear();
    state.market_phase = -1;

    auto split_numbers = [](const std::string& value) {
        std::vector<double> numbers;
        std::istringstream parts(value);
        std::string item;
        while (std::getline(parts, item, '|')) {
            numbers.push_back(std::atof(item.c_str()));
        }
        return numbers;
    };

    std::istringstream stream(text);
    std::string line;
    bool any = false;
    bool has_next_event = false;
    bool has_market_data = false;
    bool has_market_phase = false;
    bool has_card_data = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equal);
        const std::string value = line.substr(equal + 1);
        any = true;
        if (key == "energy") {
            state.energy = std::atof(value.c_str());
        } else if (key == "minerals") {
            state.minerals = std::atof(value.c_str());
        } else if (key == "food") {
            state.food = std::atof(value.c_str());
        } else if (key == "alloy") {
            state.alloy = std::atof(value.c_str());
        } else if (key == "population") {
            state.population = std::atof(value.c_str());
        } else if (key == "research") {
            state.research = std::atof(value.c_str());
        } else if (key == "morale") {
            state.morale = std::clamp(std::atof(value.c_str()), 0.0, 100.0);
        } else if (key == "influence") {
            state.influence = std::max(0.0, std::atof(value.c_str()));
        } else if (key == "tokens") {
            state.tokens = std::max(0.0, std::atof(value.c_str()));
        } else if (key == "relics") {
            state.relics = std::max(0, std::atoi(value.c_str()));
        } else if (key == "cores") {
            state.cores = std::max(0, std::atoi(value.c_str()));
        } else if (key == "data_cores") {
            state.data_cores = std::max(0, std::atoi(value.c_str()));
        } else if (key == "reputation") {
            state.reputation = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (key == "contracts_done") {
            state.contracts_done = std::max(0, std::atoi(value.c_str()));
        } else if (key == "expeditions_done") {
            state.expeditions_done = std::max(0, std::atoi(value.c_str()));
        } else if (key == "market_phase") {
            state.market_phase = std::max(-1, std::atoi(value.c_str()));
            has_market_phase = true;
            has_market_data = true;
        } else if (key == "contract") {
            const std::vector<double> numbers = split_numbers(value);
            if (numbers.size() >= 5 &&
                state.contracts.size() < static_cast<std::size_t>(kMaxContracts)) {
                TradeContract contract;
                contract.resource = std::clamp(static_cast<int>(numbers[0]), 0,
                                               kTradeResourceCount - 1);
                contract.amount = std::max(0.0, numbers[1]);
                contract.reward_tokens = std::max(0.0, numbers[2]);
                contract.reward_influence = std::max(0.0, numbers[3]);
                contract.done = numbers[4] != 0.0;
                state.contracts.push_back(contract);
                has_market_data = true;
            }
        } else if (key == "market") {
            const std::vector<double> numbers = split_numbers(value);
            if (numbers.size() >= 4 &&
                state.black_market.size() <
                    static_cast<std::size_t>(kMaxBlackMarketOffers)) {
                BlackMarketOffer offer;
                offer.kind = std::clamp(static_cast<int>(numbers[0]), 0, 5);
                offer.stock = std::clamp(static_cast<int>(numbers[1]), 0, 99);
                offer.price = std::max(0.0, numbers[2]);
                offer.amount = std::max(0, static_cast<int>(numbers[3]));
                state.black_market.push_back(offer);
                has_market_data = true;
            }
        } else if (key == "command_cards") {
            const std::vector<double> numbers = split_numbers(value);
            if (numbers.size() >= 7) {
                state.card_phase = std::max(-1, static_cast<int>(numbers[0]));
                state.card_expires = std::max(0.0, numbers[1]);
                state.cards_available = numbers[2] != 0.0;
                state.card_offers[0] = std::clamp(
                    static_cast<int>(numbers[3]), 0,
                    static_cast<int>(CommandCardId::kCount) - 1);
                state.card_offers[1] = std::clamp(
                    static_cast<int>(numbers[4]), 0,
                    static_cast<int>(CommandCardId::kCount) - 1);
                state.card_offers[2] = std::clamp(
                    static_cast<int>(numbers[5]), 0,
                    static_cast<int>(CommandCardId::kCount) - 1);
                state.cards_taken = std::max(0, static_cast<int>(numbers[6]));
                has_card_data = true;
            }
        } else if (key == "expedition") {
            const std::vector<double> numbers = split_numbers(value);
            if (numbers.size() >= 13 &&
                state.expeditions.size() < static_cast<std::size_t>(kMaxExpeditions)) {
                Expedition expedition;
                expedition.destination = std::clamp(
                    static_cast<int>(numbers[0]), 0,
                    static_cast<int>(ExpeditionDestination::kCount) - 1);
                expedition.stance = std::clamp(
                    static_cast<int>(numbers[1]), 0,
                    static_cast<int>(ExpeditionStance::kCount) - 1);
                expedition.fleet = std::max(0, static_cast<int>(numbers[2]));
                expedition.depart_seconds = std::max(0.0, numbers[3]);
                expedition.return_seconds = std::max(0.0, numbers[4]);
                expedition.outcome = std::clamp(static_cast<int>(numbers[5]), 0, 4);
                expedition.relics = std::max(0, static_cast<int>(numbers[6]));
                expedition.cores = std::max(0, static_cast<int>(numbers[7]));
                expedition.data_cores = std::max(0, static_cast<int>(numbers[8]));
                expedition.tokens = std::max(0.0, numbers[9]);
                expedition.research = std::max(0.0, numbers[10]);
                expedition.influence = std::max(0.0, numbers[11]);
                expedition.fleet_loss = std::max(0, static_cast<int>(numbers[12]));
                state.expeditions.push_back(expedition);
            }
        } else if (key == "report") {
            const std::vector<double> numbers = split_numbers(value);
            if (numbers.size() >= 11 &&
                state.expedition_reports.size() <
                    static_cast<std::size_t>(kMaxExpeditionReports)) {
                ExpeditionReport report;
                report.destination = std::clamp(
                    static_cast<int>(numbers[0]), 0,
                    static_cast<int>(ExpeditionDestination::kCount) - 1);
                report.stance = std::clamp(
                    static_cast<int>(numbers[1]), 0,
                    static_cast<int>(ExpeditionStance::kCount) - 1);
                report.outcome = std::clamp(static_cast<int>(numbers[2]), 0, 4);
                report.relics = std::max(0, static_cast<int>(numbers[3]));
                report.cores = std::max(0, static_cast<int>(numbers[4]));
                report.data_cores = std::max(0, static_cast<int>(numbers[5]));
                report.tokens = std::max(0.0, numbers[6]);
                report.research = std::max(0.0, numbers[7]);
                report.influence = std::max(0.0, numbers[8]);
                report.fleet_loss = std::max(0, static_cast<int>(numbers[9]));
                report.return_seconds = std::max(0.0, numbers[10]);
                state.expedition_reports.push_back(report);
            }
        } else if (key == "faction") {
            state.faction = std::clamp(std::atoi(value.c_str()), 0,
                                       static_cast<int>(FactionId::kCount) - 1);
        } else if (key == "wonders") {
            state.wonders = static_cast<unsigned int>(std::max(0, std::atoi(value.c_str()))) &
                            ((1u << kWonderCount) - 1u);
        } else if (key == "next_event") {
            state.next_event_seconds = std::max(0.0, std::atof(value.c_str()));
            has_next_event = true;
        } else if (key == "events") {
            state.events.clear();
            std::istringstream numbers(value);
            std::string item;
            while (std::getline(numbers, item, ',') && state.events.size() <
                                                               static_cast<std::size_t>(
                                                                   kMaxPendingEvents)) {
                if (item.empty()) {
                    continue;
                }
                const int id = std::atoi(item.c_str());
                if (id >= 0 && id < static_cast<int>(EventId::kCount)) {
                    state.events.push_back(GameEvent{id});
                }
            }
        } else if (key == "capital") {
            state.capital_level = std::clamp(std::atoi(value.c_str()), 1, kMaxCapitalLevel);
        } else if (key == "fleet") {
            state.fleet = std::max(0, std::atoi(value.c_str()));
        } else if (key == "wins") {
            state.battles_won = std::max(0, std::atoi(value.c_str()));
        } else if (key == "losses") {
            state.battles_lost = std::max(0, std::atoi(value.c_str()));
        } else if (key == "unix_time") {
            state.unix_time = std::atof(value.c_str());
        } else if (key == "play_seconds") {
            state.play_seconds = std::atof(value.c_str());
        } else if (key == "day") {
            state.day = std::max(1, std::atoi(value.c_str()));
        } else if (key == "rng") {
            state.rng = static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
            if (state.rng == 0) {
                state.rng = 0x9E3779B97F4A7C15ull;
            }
        } else if (key == "buildings" || key == "techs") {
            int values[6]{};
            int index = 0;
            std::istringstream numbers(value);
            std::string item;
            while (std::getline(numbers, item, ',') && index < 6) {
                values[index++] = std::max(0, std::atoi(item.c_str()));
            }
            for (int i = 0; i < 6; ++i) {
                if (key == "buildings") {
                    state.buildings[i] = std::min(values[i], kMaxBuildingLevel);
                } else {
                    state.techs[i] = std::min(values[i], kMaxTechLevel);
                }
            }
        } else if (key == "colonies") {
            state.colonies.clear();
            std::istringstream numbers(value);
            std::string item;
            while (std::getline(numbers, item, ',')) {
                if (item.empty()) {
                    continue;
                }
                state.colonies.push_back(
                    Colony{std::clamp(std::atoi(item.c_str()), 1, kMaxColonyLevel)});
            }
        }
    }

    if (!any) {
        return false;
    }
    if (!has_next_event) {
        state.next_event_seconds = state.play_seconds + 120.0;
    }
    if (state.next_event_seconds < state.play_seconds) {
        state.next_event_seconds = state.play_seconds + 60.0;
    }
    if (!has_market_data || !has_market_phase ||
        state.market_phase != MarketPhase(state) || state.contracts.empty() ||
        state.black_market.empty()) {
        RefreshMarket(state, MarketPhase(state));
    }
    if (!has_card_data) {
        state.card_phase = MarketPhase(state);
        state.card_expires = 0.0;
        state.cards_available = false;
        state.card_offers[0] = 0;
        state.card_offers[1] = 1;
        state.card_offers[2] = 2;
        state.cards_taken = 0;
    }
    out_state = state;
    return true;
}

}  // namespace stellar
