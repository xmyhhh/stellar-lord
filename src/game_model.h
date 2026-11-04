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

// 殖民地专业化：每个殖民地只能保留一种定位，改定位要花影响力。
enum class ColonySpecialization {
    kBalanced = 0,    // 均衡：无加成，无惩罚
    kEnergy = 1,      // 能源世界：能源 +45%，食物 -15%
    kMining = 2,      // 矿业世界：矿物 +45%，能源 -10%
    kAgriculture = 3, // 农业世界：食物 +45%，合金 -15%
    kIndustry = 4,    // 工业世界：合金 +45%，食物 -15%
    kResearch = 5,    // 科研世界：科研 +45%，人口上限 -10%
    kTrade = 6,       // 贸易港：ST 卖价 +，影响力 +
    kFortress = 7,    // 要塞世界：舰队上限 +
    kCount = 8,
};

// 殖民地特质：每颗星球天生的环境，不随定位改变。
enum class ColonyTrait {
    kNone = 0,
    kRichMinerals = 1,    // 富矿：矿物 +30%
    kFertile = 2,         // 沃土：食物 +30%
    kResearchRuins = 3,   // 科研遗迹：科研 +20%，事件更频繁
    kTidalLock = 4,       // 潮汐锁定：能源 +20%，人口上限 -10%
    kToxicAtmosphere = 5, // 有毒大气：合金 +20%，人口增长 -20%
    kAncientOrbit = 6,    // 古代轨道：防御 +30%
    kCount = 7,
};

// 殖民地区域：每座殖民地按等级解锁槽位，每个区域可升到 3 级。
enum class DistrictId {
    kPower = 0,       // 能源区：能源 +20%/级
    kMining = 1,      // 矿业区：矿物 +20%/级
    kAgriculture = 2, // 农业区：食物 +20%/级
    kIndustry = 3,    // 工业区：合金 +20%/级
    kResearch = 4,    // 科研区：科研 +20%/级
    kMilitary = 5,    // 军事区：舰队上限 +5/级，防御 +15%/级
    kCulture = 6,     // 文化区：影响力 +2/分钟/级
    kPort = 7,        // 港口区：市场卖价 +5%/级，买价 -3%/级
    kCount = 8,
};

struct Colony {
    int level{1};
    int specialization{0};  // ColonySpecialization
    int trait{0};           // ColonyTrait
    int districts[static_cast<int>(DistrictId::kCount)]{0, 0, 0, 0, 0, 0, 0, 0};
};

// 治国路线。开局所有人都是平民，第一次选择决定这局怎么玩。
enum class FactionId {
    kNone = 0,
    kAnvil = 1,    // 铁砧工业：产能
    kChoir = 2,    // 星海教团：科研
    kCompact = 3,  // 拓荒议会：人口与稳定
    kGuild = 4,    // 自由商盟：贸易与掠夺
    kCount = 5,
};

// 个人事件卡。每个事件有两个选项，没有免费午餐。
enum class EventId {
    kPirateRaid = 0,
    kIonStorm = 1,
    kRefugeeFleet = 2,
    kBlackMarket = 3,
    kMineCollapse = 4,
    kSilenceScan = 5,
    kDeserters = 6,
    kTradeBoom = 7,
    kEchoSignal = 8,
    kArkDistress = 9,
    kSilentTide = 10,
    kCount = 11,
};

struct GameEvent {
    int id{0};
};

// 中立星区目标：除了殖民星域，还有专门爆资源的高风险目标。
enum class SectorTargetId {
    kMiningBelt = 0,
    kSilentOutpost = 1,
    kCount = 2,
};

// 贸易合同：交付一批基础资源，换星际代币（ST）和影响力。
struct TradeContract {
    int resource{1};              // 0 能源 / 1 矿物 / 2 食物 / 3 合金
    double amount{100.0};
    double reward_tokens{50.0};
    double reward_influence{3.0};
    bool done{false};
};

// 黑市商品：稀有资源、雇佣兵、科研缓存、政治献金。
struct BlackMarketOffer {
    int kind{0};                  // 0 遗物 / 1 星核 / 2 数据核心 / 3 雇佣兵 / 4 科研 / 5 影响力
    int stock{1};
    double price{100.0};
    int amount{1};
};

// 领主指令卡：每 15 分钟抽三张，短时间内选一张，给玩家一次主动决策。
enum class CommandCardId {
    kEmergencyLevy = 0,       // 紧急征召：+5 舰，-3 人口，-8 士气
    kResearchBreakthrough = 1,// 科研突破：+150 科研，-80 能源
    kColonySubsidy = 2,       // 殖民补贴：+220 矿，+100 合金，-60 食物
    kEmergencyLoan = 3,       // 紧急贷款：+260 ST，-10 士气，-3 信誉
    kTalentIntake = 4,        // 人才引进：+3 人口，-60 科研
    kDiplomaticRound = 5,     // 外交斡旋：+90 影响力，-90 合金
    kBlackMarketRun = 6,      // 黑市采购：+1 数据核心，-100 ST
    kSilenceListening = 7,    // 静默监听：+1 数据核心，-60 能源，-4 士气
    kCount = 8,
};

// 远征目的地与风险姿态。结果在出发时确定，返航时才揭示。
enum class ExpeditionDestination {
    kFrontier = 0,  // 未探索扇区：低风险，地图与少量资源
    kRuins = 1,     // 遗迹带：遗物、数据核心、回声剧情
    kAnomaly = 2,   // 深空异常：星核、稀有科技
    kArk = 3,       // 漂流方舟：数据核心、人口、旧科技
    kCount = 4,
};

enum class ExpeditionStance {
    kCautious = 0,  // 谨慎：收益 -20%，损失 -30%
    kDeep = 1,      // 深入：收益 +30%，损失 +50%
    kCount = 2,
};

struct Expedition {
    int destination{0};
    int stance{1};
    int fleet{0};
    double depart_seconds{0.0};
    double return_seconds{0.0};
    int outcome{1};               // 0 大成功 / 1 成功 / 2 部分成功 / 3 失败 / 4 灾难
    int relics{0};
    int cores{0};
    int data_cores{0};
    double tokens{0.0};
    double research{0.0};
    double influence{0.0};
    int fleet_loss{0};
};

struct ExpeditionReport {
    int destination{0};
    int stance{1};
    int outcome{1};
    int relics{0};
    int cores{0};
    int data_cores{0};
    double tokens{0.0};
    double research{0.0};
    double influence{0.0};
    int fleet_loss{0};
    double return_seconds{0.0};
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
    double morale{70.0};

    // 星际经济：影响力、星际代币与稀有资源。
    double influence{0.0};
    double tokens{0.0};
    int relics{0};
    int cores{0};
    int data_cores{0};
    int reputation{50};             // 0-100，影响市场价格与合同
    int contracts_done{0};
    int expeditions_done{0};

    // 市场每 15 分钟刷新一次；合同与黑市库存都跟着这个相位走。
    int market_phase{-1};
    std::vector<TradeContract> contracts;
    std::vector<BlackMarketOffer> black_market;

    // 指令卡：和市场经济同相位刷新，出现后 120 秒内有效。
    int card_phase{-1};
    double card_expires{0.0};
    bool cards_available{false};
    int card_offers[3]{0, 0, 0};
    int cards_taken{0};

    // 远征队与返航报告。
    std::vector<Expedition> expeditions;
    std::vector<ExpeditionReport> expedition_reports;

    // 帝国
    int capital_level{1};
    int faction{0};             // FactionId
    unsigned int wonders{0};    // bit 0..2
    std::vector<Colony> colonies;
    std::vector<GameEvent> events;
    double next_event_seconds{120.0};

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
    double influence{0.0};
    double population_capacity{0.0};
    double population_growth{0.0};
    double fleet_growth{0.0};
    double fleet_capacity{0.0};
};

constexpr int kMaxBuildingLevel = 10;
constexpr int kMaxTechLevel = 5;
constexpr int kMaxCapitalLevel = 10;
constexpr int kMaxColonyLevel = 5;
constexpr int kMaxDistrictLevel = 3;
constexpr int kMaxDistrictSlots = 5;
constexpr double kColonySpecializationCost = 10.0;  // 影响力
constexpr int kMaxPendingEvents = 4;
constexpr int kWonderCount = 3;

// 贸易 / 黑市 / 远征
constexpr int kTradeResourceCount = 4;     // 能源 / 矿物 / 食物 / 合金
constexpr int kMaxContracts = 3;
constexpr int kMaxBlackMarketOffers = 3;
constexpr int kMaxExpeditions = 3;
constexpr int kMaxExpeditionReports = 6;
constexpr double kMarketPhaseSeconds = 900.0;  // 15 分钟刷新一次
constexpr double kTradeLotSize = 50.0;
constexpr int kCommandCardChoices = 3;
constexpr double kCommandCardWindowSeconds = 120.0;

// 奇观编号：0 戴森环 / 1 知识方舟 / 2 轨道船坞。
constexpr int kWonderDysonRing = 0;
constexpr int kWonderKnowledgeArk = 1;
constexpr int kWonderOrbitalDock = 2;

// 平民起点：所有人加入时完全一致。
GameState MakeNewGame(double unix_time);

Rates ComputeRates(const GameState& state);
void Advance(GameState& state, double dt_seconds);

// 派生值
double ProductionMultiplier(const GameState& state);
double PopulationCapacity(const GameState& state);
double LaborFactor(const GameState& state);
double MoraleFactor(const GameState& state);
int FleetCapacity(const GameState& state);
int TotalColonyLevels(const GameState& state);
double ColonyDefenseMultiplier(const GameState& state);
double EventChanceMultiplier(const GameState& state);
double Score(const GameState& state);

// 治国路线：一局只能选一次。
bool ChooseFaction(GameState& state, int faction);
bool HasFaction(const GameState& state, FactionId faction);

// 奇观
Cost WonderCost(const GameState& state, int wonder);
bool CanBuildWonder(const GameState& state, int wonder);
bool BuildWonder(GameState& state, int wonder);
bool HasWonder(const GameState& state, int wonder);
int WonderCount(const GameState& state);

// 时代：苏醒 / 立足 / 扩张 / 审计 / 结算。
int EraIndex(const GameState& state);
double NextEraIn(const GameState& state);

// 市场：买卖基础资源，价格随相位、信誉和阵营波动。
int MarketPhase(const GameState& state);
double MarketSellPrice(const GameState& state, int resource);
double MarketBuyPrice(const GameState& state, int resource);
bool SellResource(GameState& state, int resource, double amount = kTradeLotSize);
bool BuyResource(GameState& state, int resource, double amount = kTradeLotSize);

// 合同：交付基础资源，换 ST、影响力和信誉。
bool FulfillContract(GameState& state, std::size_t index);

// 黑市：用 ST 买稀有资源、雇佣兵、科研或影响力。
bool BuyBlackMarket(GameState& state, std::size_t index);

// 指令卡：每 15 分钟抽三张，120 秒内选一张。
bool CommandCardsAvailable(const GameState& state);
double CommandCardSecondsLeft(const GameState& state);
int CommandCardOffer(const GameState& state, int index);
bool ChooseCommandCard(GameState& state, int index);

// 远征：从现有舰队抽船，结果在出发时确定，返航时结算。
int ExpeditionFleetCost(ExpeditionDestination destination, ExpeditionStance stance);
double ExpeditionDurationSeconds(ExpeditionDestination destination);
int ActiveExpeditionCount(const GameState& state);
bool LaunchExpedition(GameState& state, ExpeditionDestination destination,
                      ExpeditionStance stance);
void ClearExpeditionReports(GameState& state);

// 事件：最多排队 4 条，超出时最旧一条按保守选项自动结算。
void AddEvent(GameState& state, EventId id);
bool ResolveEvent(GameState& state, std::size_t index, bool choice_a);

// 建筑
Cost BuildingUpgradeCost(const GameState& state, BuildingId id);
bool CanBuild(const GameState& state, BuildingId id);
bool Build(GameState& state, BuildingId id);

// 开发
Cost CapitalUpgradeCost(const GameState& state);
bool UpgradeCapital(GameState& state);
Cost ColonyUpgradeCost(const GameState& state, std::size_t index);
bool UpgradeColony(GameState& state, std::size_t index);

// 殖民地专业化：第一次选择免费，之后每次改定位消耗影响力。
bool CanChangeColonySpecialization(const GameState& state, std::size_t index);
bool SetColonySpecialization(GameState& state, std::size_t index,
                             ColonySpecialization specialization);
bool CycleColonySpecialization(GameState& state, std::size_t index);

// 殖民地区域：槽位随殖民地等级解锁，每个区域 0–3 级。
int DistrictSlots(const Colony& colony);
int DistrictCount(const Colony& colony);
Cost DistrictCost(const GameState& state, std::size_t colony_index, int district);
bool CanDevelopDistrict(const GameState& state, std::size_t colony_index, int district);
bool DevelopDistrict(GameState& state, std::size_t colony_index, int district);

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
    double loot_research{0.0};
    double loot_tokens{0.0};
};

int NeutralDefense(const GameState& state);
BattleResult AttackNeutral(GameState& attacker);

// 高风险中立目标：资源丰厚，但不会直接给殖民地。
int SectorDefense(const GameState& state, SectorTargetId target);
BattleResult AttackSector(GameState& attacker, SectorTargetId target);

// 劫掠：抢资源但不占领，风险更小；占领：打赢后夺走殖民地。
BattleResult RaidPlayer(GameState& attacker, GameState& defender);
BattleResult AttackPlayer(GameState& attacker, GameState& defender);

// 存档 / 网络快照
std::string SerializeState(const GameState& state);
bool DeserializeState(const std::string& text, GameState& out_state);

}  // namespace stellar
