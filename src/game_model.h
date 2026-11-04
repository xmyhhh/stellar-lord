#pragma once

// 星际领主 (Stellar Lord) —— 纯逻辑层。
// 这个文件不包含任何 Win32 / 渲染代码，客户端与服务端共用同一套规则。

#include <cstdint>
#include <string>
#include <vector>

namespace stellar {

enum class BuildingId {
    kPower = 0,   // 电站：能源
    kMine = 1,    // 矿场：矿物
    kFarm = 2,    // 农场：食物
    kFactory = 3, // 工厂：合金
    kLab = 4,     // 实验室：科研点
    kBarracks = 5,// 军营：舰队
    kCount = 6,
};

enum class TechId {
    kEnergy = 0,       // 能源效率
    kMining = 1,       // 采矿效率
    kAgriculture = 2,  // 农业与人口
    kIndustry = 3,     // 工业效率
    kMilitary = 4,     // 军事训练
    kColonization = 5, // 殖民技术
    kCount = 6,
};

struct Cost {
    double energy{0.0};
    double minerals{0.0};
    double food{0.0};
    double alloy{0.0};
};

struct Colony {
    int level{1};
};

struct GameState {
    // 资源
    double energy{0.0};
    double minerals{0.0};
    double food{0.0};
    double alloy{0.0};

    // 人口与科研
    double population{0.0};
    double research{0.0};

    // 帝国
    int capital_level{1};
    std::vector<Colony> colonies;

    int buildings[static_cast<int>(BuildingId::kCount)]{0, 0, 0, 0, 0, 0};
    int techs[static_cast<int>(TechId::kCount)]{0, 0, 0, 0, 0, 0};

    // 军队
    int fleet{0};
    int battles_won{0};
    int battles_lost{0};

    // 时间
    double unix_time{0.0};
    double play_seconds{0.0};
    int day{1};

    std::uint64_t rng{0x9E3779B97F4A7C15ull};
};

struct Rates {
    double energy{0.0};
    double minerals{0.0};
    double food{0.0};
    double alloy{0.0};
    double research{0.0};
    double population_capacity{0.0};
    double population_growth{0.0};
    double fleet_growth{0.0};
    double fleet_capacity{0.0};
};

constexpr int kMaxBuildingLevel = 10;
constexpr int kMaxTechLevel = 5;
constexpr int kMaxCapitalLevel = 10;
constexpr int kMaxColonyLevel = 5;

// 平民起点：所有人加入时完全一致。
GameState MakeNewGame(double unix_time);

Rates ComputeRates(const GameState& state);
void Advance(GameState& state, double dt_seconds);

// 派生值
double ProductionMultiplier(const GameState& state);
double PopulationCapacity(const GameState& state);
double LaborFactor(const GameState& state);
int FleetCapacity(const GameState& state);
int TotalColonyLevels(const GameState& state);
double Score(const GameState& state);

// 建筑
Cost BuildingUpgradeCost(const GameState& state, BuildingId id);
bool CanBuild(const GameState& state, BuildingId id);
bool Build(GameState& state, BuildingId id);

// 开发
Cost CapitalUpgradeCost(const GameState& state);
bool UpgradeCapital(GameState& state);
Cost ColonyUpgradeCost(const GameState& state, std::size_t index);
bool UpgradeColony(GameState& state, std::size_t index);

// 科研
double TechCost(const GameState& state, TechId id);
bool CanBuyTech(const GameState& state, TechId id);
bool BuyTech(GameState& state, TechId id);

// 军队
Cost FleetBuildCost(const GameState& state, int fleet_points);
bool BuildFleet(GameState& state, int fleet_points);

// 殖民
Cost ColonizeCost(const GameState& state);
bool CanColonize(const GameState& state);
bool Colonize(GameState& state);

// 占领
struct BattleResult {
    bool won{false};
    bool captured{false};
    int attacker_loss{0};
    int defender_loss{0};
    double loot_minerals{0.0};
    double loot_alloy{0.0};
};

int NeutralDefense(const GameState& state);
BattleResult AttackNeutral(GameState& attacker);
BattleResult AttackPlayer(GameState& attacker, GameState& defender);

// 存档 / 网络快照
std::string SerializeState(const GameState& state);
bool DeserializeState(const std::string& text, GameState& out_state);

}  // namespace stellar
