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
    state.unix_time = unix_time;
    state.play_seconds = 0.0;
    state.day = 1;
    state.rng = 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(unix_time * 1000.0);
    if (state.rng == 0) {
        state.rng = 123456789ull;
    }
    return state;
}

double ProductionMultiplier(const GameState& state) {
    double multiplier = 1.0 + 0.10 * static_cast<double>(std::max(0, state.capital_level - 1));
    multiplier += 0.15 * static_cast<double>(TotalColonyLevels(state));
    return multiplier * LaborFactor(state);
}

double PopulationCapacity(const GameState& state) {
    return 20.0 + 12.0 * static_cast<double>(std::max(1, state.capital_level)) +
           10.0 * static_cast<double>(TotalColonyLevels(state));
}

double LaborFactor(const GameState& state) {
    const double capacity = PopulationCapacity(state);
    if (capacity <= 0.0) {
        return 0.55;
    }
    const double ratio = std::clamp(state.population / capacity, 0.0, 1.0);
    return 0.55 + 0.45 * ratio;
}

int FleetCapacity(const GameState& state) {
    return 20 + 12 * state.capital_level + 8 * TotalColonyLevels(state) +
           6 * state.buildings[static_cast<int>(BuildingId::kBarracks)];
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
    rates.population_capacity = PopulationCapacity(state);
    const bool fed = state.food > 1.0 && state.energy > 1.0;
    rates.population_growth =
        fed ? 0.05 * TechMultiplier(tech(TechId::kAgriculture), 0.25) : -0.08;
    rates.fleet_growth = 0.35 * barracks * TechMultiplier(tech(TechId::kMilitary), 0.15);
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

double TechCost(const GameState& state, TechId id) {
    const int level = std::max(0, state.techs[static_cast<int>(id)]);
    return 40.0 * Pow(2.1, level);
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
    const double discount = 1.0 - 0.08 * static_cast<double>(state.techs[static_cast<int>(TechId::kMilitary)]);
    Cost cost;
    cost.alloy = 2.2 * points * std::max(0.5, discount);
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
        result.loot_minerals = 40.0 + 2.0 * static_cast<double>(defense);
        result.loot_alloy = 15.0 + 1.0 * static_cast<double>(defense);
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        attacker.colonies.push_back(Colony{1});
        ++attacker.battles_won;
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.6));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        ++attacker.battles_lost;
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
        result.loot_minerals = defender.minerals * 0.15;
        result.loot_alloy = defender.alloy * 0.15;
        defender.minerals -= result.loot_minerals;
        defender.alloy -= result.loot_alloy;
        attacker.minerals += result.loot_minerals;
        attacker.alloy += result.loot_alloy;
        ++attacker.battles_won;
        ++defender.battles_lost;
    } else {
        result.attacker_loss = std::max(1, static_cast<int>(attacker.fleet * 0.6));
        result.defender_loss = std::max(1, static_cast<int>(defender.fleet * 0.2));
        attacker.fleet = std::max(0, attacker.fleet - result.attacker_loss);
        defender.fleet = std::max(0, defender.fleet - result.defender_loss);
        ++attacker.battles_lost;
        ++defender.battles_won;
    }
    return result;
}

double Score(const GameState& state) {
    return state.population * 2.0 + static_cast<double>(state.fleet) * 1.5 +
           static_cast<double>(state.capital_level + TotalColonyLevels(state)) * 30.0 +
           state.research * 0.2 + static_cast<double>(state.battles_won) * 15.0;
}

std::string SerializeState(const GameState& state) {
    std::string text;
    text += "version=2\n";
    text += "energy=" + std::to_string(state.energy) + "\n";
    text += "minerals=" + std::to_string(state.minerals) + "\n";
    text += "food=" + std::to_string(state.food) + "\n";
    text += "alloy=" + std::to_string(state.alloy) + "\n";
    text += "population=" + std::to_string(state.population) + "\n";
    text += "research=" + std::to_string(state.research) + "\n";
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

    std::istringstream stream(text);
    std::string line;
    bool any = false;
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
    out_state = state;
    return true;
}

}  // namespace stellar
