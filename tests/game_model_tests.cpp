#include "game_model.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

bool NearlyEqual(double a, double b, double epsilon = 1e-6) {
    return std::fabs(a - b) <= epsilon;
}

void TestNewGameStartsAsCommoner() {
    stellar::GameState state = stellar::MakeNewGame(1000.0);
    CHECK(NearlyEqual(state.population, 12.0));
    CHECK(state.fleet == 10);
    CHECK(state.capital_level == 1);
    CHECK(state.colonies.empty());
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kPower)] == 1);
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kMine)] == 1);
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kFarm)] == 1);
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kBarracks)] == 0);
    CHECK(state.day == 1);
}

void TestAdvanceProducesAndStaysNonNegative() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    const double minerals_before = state.minerals;
    const double energy_before = state.energy;
    stellar::Advance(state, 60.0);
    CHECK(state.minerals > minerals_before);
    CHECK(state.energy > 0.0);
    CHECK(state.food >= 0.0);
    CHECK(state.population > 0.0);
    CHECK(state.play_seconds >= 59.0);
}

void TestBuildingCostsAndWallet() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.minerals = 0.0;
    state.energy = 0.0;
    CHECK(!stellar::CanBuild(state, stellar::BuildingId::kMine));
    CHECK(!stellar::Build(state, stellar::BuildingId::kMine));

    state.minerals = 10000.0;
    state.energy = 10000.0;
    state.alloy = 10000.0;
    CHECK(stellar::Build(state, stellar::BuildingId::kMine));
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kMine)] == 2);

    // 升到上限后不能再建。
    for (int i = 0; i < 20; ++i) {
        stellar::Build(state, stellar::BuildingId::kMine);
    }
    CHECK(state.buildings[static_cast<int>(stellar::BuildingId::kMine)] ==
          stellar::kMaxBuildingLevel);
    CHECK(!stellar::CanBuild(state, stellar::BuildingId::kMine));
}

void TestPopulationRespectsCapacity() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.food = 100000.0;
    state.energy = 100000.0;
    stellar::Advance(state, 3600.0);
    CHECK(state.population <= stellar::PopulationCapacity(state) + 1e-6);
    CHECK(state.population >= 12.0);
}

void TestResearch() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    CHECK(!stellar::CanBuyTech(state, stellar::TechId::kMining));
    state.research = 10000.0;
    CHECK(stellar::BuyTech(state, stellar::TechId::kMining));
    CHECK(state.techs[static_cast<int>(stellar::TechId::kMining)] == 1);
    const double cost_after = stellar::TechCost(state, stellar::TechId::kMining);
    CHECK(cost_after > 40.0);
}

void TestColonize() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.alloy = 10000.0;
    state.food = 10000.0;
    state.population = 50.0;
    CHECK(stellar::CanColonize(state));
    CHECK(stellar::Colonize(state));
    CHECK(state.colonies.size() == 1);
    const double capacity_before = stellar::PopulationCapacity(state);
    CHECK(stellar::UpgradeColony(state, 0));
    CHECK(stellar::PopulationCapacity(state) > capacity_before);
}

void TestColonizeRequiresPopulation() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.alloy = 10000.0;
    state.food = 10000.0;
    state.population = 3.0;
    CHECK(!stellar::CanColonize(state));
}

void TestColonySpecialization() {
    stellar::GameState base = stellar::MakeNewGame(0.0);
    base.colonies.push_back(stellar::Colony{2});

    stellar::GameState energy = base;
    energy.colonies[0].specialization =
        static_cast<int>(stellar::ColonySpecialization::kEnergy);
    CHECK(stellar::ComputeRates(energy).energy >
          stellar::ComputeRates(base).energy);
    CHECK(stellar::ComputeRates(energy).food <
          stellar::ComputeRates(base).food);

    stellar::GameState mining = base;
    mining.colonies[0].specialization =
        static_cast<int>(stellar::ColonySpecialization::kMining);
    CHECK(stellar::ComputeRates(mining).minerals >
          stellar::ComputeRates(base).minerals);
    CHECK(stellar::ComputeRates(mining).energy <
          stellar::ComputeRates(base).energy);

    stellar::GameState research = base;
    research.colonies[0].specialization =
        static_cast<int>(stellar::ColonySpecialization::kResearch);
    CHECK(stellar::ComputeRates(research).research >
          stellar::ComputeRates(base).research);
    CHECK(stellar::PopulationCapacity(research) <
          stellar::PopulationCapacity(base));

    stellar::GameState trade = base;
    trade.colonies[0].specialization =
        static_cast<int>(stellar::ColonySpecialization::kTrade);
    CHECK(stellar::MarketSellPrice(trade, 1) >
          stellar::MarketSellPrice(base, 1));
    CHECK(stellar::ComputeRates(trade).influence >
          stellar::ComputeRates(base).influence);

    stellar::GameState fortress = base;
    fortress.colonies[0].specialization =
        static_cast<int>(stellar::ColonySpecialization::kFortress);
    CHECK(stellar::FleetCapacity(fortress) ==
          stellar::FleetCapacity(base) + 8);

    // 第一次定位免费，之后每次改定位消耗 10 影响力。
    stellar::GameState doctrine = stellar::MakeNewGame(0.0);
    doctrine.colonies.push_back(stellar::Colony{1});
    CHECK(stellar::CanChangeColonySpecialization(doctrine, 0));
    CHECK(stellar::SetColonySpecialization(
        doctrine, 0, stellar::ColonySpecialization::kEnergy));
    CHECK(doctrine.influence == 0.0);
    CHECK(!stellar::SetColonySpecialization(
        doctrine, 0, stellar::ColonySpecialization::kMining));

    doctrine.influence = 25.0;
    CHECK(stellar::SetColonySpecialization(
        doctrine, 0, stellar::ColonySpecialization::kMining));
    CHECK(NearlyEqual(doctrine.influence, 15.0));
    CHECK(stellar::CycleColonySpecialization(doctrine, 0));
    CHECK(doctrine.colonies[0].specialization ==
          static_cast<int>(stellar::ColonySpecialization::kAgriculture));
    CHECK(NearlyEqual(doctrine.influence, 5.0));
    CHECK(!stellar::CanChangeColonySpecialization(doctrine, 0));

    // 专业化必须随存档一起往返，并兼容旧的纯等级格式。
    const std::string text = stellar::SerializeState(doctrine);
    CHECK(text.rfind("version=6\n", 0) == 0);
    stellar::GameState loaded{};
    CHECK(stellar::DeserializeState(text, loaded));
    CHECK(loaded.colonies.size() == 1);
    CHECK(loaded.colonies[0].specialization ==
          static_cast<int>(stellar::ColonySpecialization::kAgriculture));

    stellar::GameState legacy{};
    CHECK(stellar::DeserializeState("version=5\ncolonies=2,1\n", legacy));
    CHECK(legacy.colonies.size() == 2);
    CHECK(legacy.colonies[0].level == 2);
    CHECK(legacy.colonies[0].specialization == 0);
}

void TestNeutralBattle() {
    stellar::GameState weak = stellar::MakeNewGame(0.0);
    weak.fleet = 5;
    const stellar::BattleResult lost = stellar::AttackNeutral(weak);
    CHECK(!lost.won);
    CHECK(weak.battles_lost == 1);
    CHECK(weak.colonies.empty());

    stellar::GameState strong = stellar::MakeNewGame(0.0);
    strong.fleet = 500;
    const std::size_t colonies_before = strong.colonies.size();
    const stellar::BattleResult won = stellar::AttackNeutral(strong);
    CHECK(won.won);
    CHECK(won.captured);
    CHECK(strong.colonies.size() == colonies_before + 1);
    CHECK(strong.battles_won == 1);
    CHECK(strong.fleet < 500);
}

void TestPlayerBattleCapturesColony() {
    stellar::GameState attacker = stellar::MakeNewGame(0.0);
    attacker.fleet = 500;
    stellar::GameState defender = stellar::MakeNewGame(0.0);
    defender.fleet = 10;
    defender.colonies.push_back(
        stellar::Colony{2, static_cast<int>(stellar::ColonySpecialization::kFortress)});
    defender.minerals = 500.0;
    defender.alloy = 200.0;

    const stellar::BattleResult result = stellar::AttackPlayer(attacker, defender);
    CHECK(result.won);
    CHECK(result.captured);
    CHECK(attacker.colonies.size() == 1);
    CHECK(attacker.colonies[0].level == 2);
    CHECK(attacker.colonies[0].specialization ==
          static_cast<int>(stellar::ColonySpecialization::kFortress));
    CHECK(defender.colonies.empty());
    CHECK(defender.minerals < 500.0);
}

void TestSerializeRoundTrip() {
    stellar::GameState state = stellar::MakeNewGame(12345.0);
    state.minerals = 321.5;
    state.population = 44.25;
    state.capital_level = 3;
    state.buildings[static_cast<int>(stellar::BuildingId::kFactory)] = 4;
    state.techs[static_cast<int>(stellar::TechId::kIndustry)] = 2;
    state.colonies.push_back(stellar::Colony{2});
    state.colonies.push_back(stellar::Colony{1});
    state.fleet = 77;
    state.morale = 63.5;
    state.faction = static_cast<int>(stellar::FactionId::kGuild);
    state.wonders = (1u << stellar::kWonderDysonRing) |
                    (1u << stellar::kWonderOrbitalDock);
    state.events.push_back(stellar::GameEvent{static_cast<int>(stellar::EventId::kIonStorm)});
    state.events.push_back(stellar::GameEvent{static_cast<int>(stellar::EventId::kTradeBoom)});
    state.next_event_seconds = 900.5;

    const std::string text = stellar::SerializeState(state);
    stellar::GameState loaded{};
    CHECK(stellar::DeserializeState(text, loaded));
    CHECK(NearlyEqual(loaded.minerals, 321.5));
    CHECK(NearlyEqual(loaded.population, 44.25));
    CHECK(loaded.capital_level == 3);
    CHECK(loaded.buildings[static_cast<int>(stellar::BuildingId::kFactory)] == 4);
    CHECK(loaded.techs[static_cast<int>(stellar::TechId::kIndustry)] == 2);
    CHECK(loaded.colonies.size() == 2);
    CHECK(loaded.fleet == 77);
    CHECK(NearlyEqual(loaded.morale, 63.5));
    CHECK(loaded.faction == static_cast<int>(stellar::FactionId::kGuild));
    CHECK(stellar::HasWonder(loaded, stellar::kWonderDysonRing));
    CHECK(stellar::HasWonder(loaded, stellar::kWonderOrbitalDock));
    CHECK(!stellar::HasWonder(loaded, stellar::kWonderKnowledgeArk));
    CHECK(loaded.events.size() == 2);
    CHECK(loaded.events[0].id == static_cast<int>(stellar::EventId::kIonStorm));
    CHECK(NearlyEqual(loaded.next_event_seconds, 900.5));
    CHECK(!stellar::DeserializeState("", loaded));
}

void TestFactionChoice() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    CHECK(!stellar::HasFaction(state, stellar::FactionId::kAnvil));
    CHECK(!stellar::ChooseFaction(state, 0));
    CHECK(!stellar::ChooseFaction(state, static_cast<int>(stellar::FactionId::kCount)));
    CHECK(stellar::ChooseFaction(state, static_cast<int>(stellar::FactionId::kCompact)));
    CHECK(stellar::HasFaction(state, stellar::FactionId::kCompact));
    CHECK(NearlyEqual(state.morale, 78.0));
    CHECK(!stellar::ChooseFaction(state, static_cast<int>(stellar::FactionId::kAnvil)));

    stellar::GameState choir = stellar::MakeNewGame(0.0);
    CHECK(stellar::ChooseFaction(choir, static_cast<int>(stellar::FactionId::kChoir)));
    CHECK(choir.research >= 40.0);
    CHECK(stellar::ComputeRates(choir).research >
          stellar::ComputeRates(stellar::MakeNewGame(0.0)).research);

    stellar::GameState guild = stellar::MakeNewGame(0.0);
    stellar::GameState plain = stellar::MakeNewGame(0.0);
    CHECK(stellar::ChooseFaction(guild, static_cast<int>(stellar::FactionId::kGuild)));
    CHECK(stellar::FleetBuildCost(guild, 10).alloy <
          stellar::FleetBuildCost(plain, 10).alloy);
}

void TestMoraleFactor() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.morale = 90.0;
    CHECK(NearlyEqual(stellar::MoraleFactor(state), 1.15));
    state.morale = 60.0;
    CHECK(NearlyEqual(stellar::MoraleFactor(state), 1.0));
    state.morale = 40.0;
    CHECK(NearlyEqual(stellar::MoraleFactor(state), 0.90));
    state.morale = 20.0;
    CHECK(NearlyEqual(stellar::MoraleFactor(state), 0.75));
    state.morale = 5.0;
    CHECK(NearlyEqual(stellar::MoraleFactor(state), 0.55));

    stellar::GameState happy = stellar::MakeNewGame(0.0);
    happy.morale = 90.0;
    stellar::GameState grim = stellar::MakeNewGame(0.0);
    grim.morale = 5.0;
    CHECK(stellar::ComputeRates(happy).minerals > stellar::ComputeRates(grim).minerals);
}

void TestWonderMechanics() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.minerals = 100000.0;
    state.alloy = 100000.0;
    CHECK(!stellar::CanBuildWonder(state, stellar::kWonderKnowledgeArk));
    CHECK(!stellar::BuildWonder(state, stellar::kWonderKnowledgeArk));

    const int capacity_before = stellar::FleetCapacity(state);
    CHECK(stellar::BuildWonder(state, stellar::kWonderDysonRing));
    CHECK(stellar::HasWonder(state, stellar::kWonderDysonRing));
    CHECK(stellar::WonderCount(state) == 1);
    CHECK(!stellar::BuildWonder(state, stellar::kWonderDysonRing));
    CHECK(stellar::FleetCapacity(state) == capacity_before);
    state.energy = 100000.0;
    CHECK(stellar::ComputeRates(state).energy > 0.0);

    state.research = 400.0;
    CHECK(stellar::CanBuildWonder(state, stellar::kWonderKnowledgeArk));
    CHECK(stellar::BuildWonder(state, stellar::kWonderKnowledgeArk));
    CHECK(stellar::WonderCount(state) == 2);

    const int capacity_before_dock = stellar::FleetCapacity(state);
    CHECK(stellar::BuildWonder(state, stellar::kWonderOrbitalDock));
    CHECK(stellar::FleetCapacity(state) > capacity_before_dock);
    CHECK(stellar::WonderCount(state) == 3);
    CHECK(!stellar::CanBuildWonder(state, stellar::kWonderOrbitalDock));
}

void TestEventQueueAndResolution() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.next_event_seconds = 1.0;
    stellar::Advance(state, 1.0);
    CHECK(!state.events.empty());
    CHECK(state.events.size() <=
          static_cast<std::size_t>(stellar::kMaxPendingEvents));

    for (int i = 0; i < 12; ++i) {
        stellar::AddEvent(state, stellar::EventId::kTradeBoom);
    }
    CHECK(state.events.size() ==
          static_cast<std::size_t>(stellar::kMaxPendingEvents));
    CHECK(!stellar::ResolveEvent(state, state.events.size(), true));
    CHECK(stellar::ResolveEvent(state, 0, true));
    CHECK(state.events.size() ==
          static_cast<std::size_t>(stellar::kMaxPendingEvents) - 1);

    stellar::GameState refugee = stellar::MakeNewGame(0.0);
    refugee.food = 1000.0;
    const double food_before = refugee.food;
    const double population_before = refugee.population;
    stellar::AddEvent(refugee, stellar::EventId::kRefugeeFleet);
    CHECK(stellar::ResolveEvent(refugee, 0, true));
    CHECK(refugee.population > population_before);
    CHECK(refugee.food < food_before);
    CHECK(refugee.morale > 70.0);

    stellar::GameState echo = stellar::MakeNewGame(0.0);
    echo.energy = 100.0;
    stellar::AddEvent(echo, stellar::EventId::kEchoSignal);
    CHECK(stellar::ResolveEvent(echo, 0, true));
    CHECK(echo.data_cores == 1);

    stellar::GameState ark = stellar::MakeNewGame(0.0);
    stellar::AddEvent(ark, stellar::EventId::kArkDistress);
    CHECK(stellar::ResolveEvent(ark, 0, false));
    CHECK(ark.tokens > 0.0);
    CHECK(ark.morale < 70.0);
}

void TestSectorRaids() {
    stellar::GameState weak = stellar::MakeNewGame(0.0);
    weak.fleet = 5;
    const stellar::BattleResult lost =
        stellar::AttackSector(weak, stellar::SectorTargetId::kMiningBelt);
    CHECK(!lost.won);
    CHECK(weak.fleet < 5);

    stellar::GameState miner = stellar::MakeNewGame(0.0);
    miner.fleet = 500;
    const double minerals_before = miner.minerals;
    const stellar::BattleResult belt =
        stellar::AttackSector(miner, stellar::SectorTargetId::kMiningBelt);
    CHECK(belt.won);
    CHECK(belt.loot_minerals > 0.0);
    CHECK(miner.minerals > minerals_before);
    CHECK(miner.colonies.empty());

    stellar::GameState scout = stellar::MakeNewGame(0.0);
    scout.fleet = 500;
    const double research_before = scout.research;
    const stellar::BattleResult outpost =
        stellar::AttackSector(scout, stellar::SectorTargetId::kSilentOutpost);
    CHECK(outpost.won);
    CHECK(outpost.loot_research > 0.0);
    CHECK(scout.research > research_before);
    CHECK(scout.morale < 70.0);
}

void TestRaidDoesNotCapture() {
    stellar::GameState attacker = stellar::MakeNewGame(0.0);
    attacker.fleet = 500;
    stellar::GameState defender = stellar::MakeNewGame(0.0);
    defender.fleet = 10;
    defender.minerals = 1000.0;
    defender.alloy = 500.0;
    defender.research = 500.0;
    defender.tokens = 200.0;
    defender.colonies.push_back(stellar::Colony{2});

    const stellar::BattleResult result = stellar::RaidPlayer(attacker, defender);
    CHECK(result.won);
    CHECK(!result.captured);
    CHECK(attacker.colonies.empty());
    CHECK(defender.colonies.size() == 1);
    CHECK(attacker.minerals > 90.0);
    CHECK(attacker.alloy > 30.0);
    CHECK(attacker.research > 0.0);
    CHECK(attacker.tokens > 0.0);
    CHECK(result.loot_tokens > 0.0);
    CHECK(attacker.reputation < 50);
    CHECK(defender.minerals < 1000.0);
    CHECK(defender.alloy < 500.0);
    CHECK(defender.research < 500.0);
    CHECK(defender.tokens < 200.0);
}

void TestMarketTradeAndContracts() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    CHECK(!state.contracts.empty());
    CHECK(!state.black_market.empty());
    CHECK(state.market_phase == 0);
    CHECK(stellar::MarketSellPrice(state, 1) > 0.0);
    CHECK(stellar::MarketBuyPrice(state, 1) >
          stellar::MarketSellPrice(state, 1));

    const double minerals_before = state.minerals;
    const double tokens_before = state.tokens;
    CHECK(stellar::SellResource(state, 1, 50.0));
    CHECK(state.minerals < minerals_before);
    CHECK(state.tokens > tokens_before);

    const double tokens_after_sale = state.tokens;
    CHECK(stellar::BuyResource(state, 1, 10.0));
    CHECK(state.tokens < tokens_after_sale);

    // 合同必须交付指定资源，且只能完成一次。
    state.contracts[0].resource = 0;
    state.contracts[0].amount = 20.0;
    state.contracts[0].reward_tokens = 30.0;
    state.contracts[0].reward_influence = 4.0;
    state.contracts[0].done = false;
    state.energy = 100.0;
    const double tokens_before_contract = state.tokens;
    const double influence_before = state.influence;
    CHECK(stellar::FulfillContract(state, 0));
    CHECK(state.tokens >= tokens_before_contract + 30.0 - 1e-6);
    CHECK(state.influence >= influence_before + 4.0 - 1e-6);
    CHECK(state.contracts_done == 1);
    CHECK(!stellar::FulfillContract(state, 0));

    // 跨过 15 分钟后市场相位必须刷新。
    state.play_seconds = stellar::kMarketPhaseSeconds + 1.0;
    state.minerals = 1000.0;
    CHECK(stellar::SellResource(state, 1, 10.0));
    CHECK(state.market_phase == 1);
}

void TestBlackMarket() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    state.tokens = 2000.0;
    CHECK(state.black_market.size() ==
          static_cast<std::size_t>(stellar::kMaxBlackMarketOffers));
    const double tokens_before = state.tokens;
    const int relics_before = state.relics;
    const int cores_before = state.cores;
    const int data_before = state.data_cores;
    CHECK(stellar::BuyBlackMarket(state, 0));
    CHECK(state.tokens < tokens_before);
    CHECK(state.black_market[0].stock == 0);
    CHECK(!stellar::BuyBlackMarket(state, 0));
    CHECK(state.relics + state.cores + state.data_cores >
          relics_before + cores_before + data_before);

    // 第二件是服务类商品：雇佣兵 / 科研 / 影响力，至少要有一种变化。
    state.tokens = 2000.0;
    const int fleet_before = state.fleet;
    const double research_before = state.research;
    const double influence_before = state.influence;
    CHECK(stellar::BuyBlackMarket(state, 1));
    CHECK(state.fleet > fleet_before || state.research > research_before ||
          state.influence > influence_before);
}

void TestExpeditions() {
    stellar::GameState state = stellar::MakeNewGame(1234.0);
    state.fleet = 200;
    CHECK(stellar::LaunchExpedition(state, stellar::ExpeditionDestination::kRuins,
                                    stellar::ExpeditionStance::kCautious));
    CHECK(stellar::ActiveExpeditionCount(state) == 1);
    CHECK(state.fleet ==
          200 - stellar::ExpeditionFleetCost(stellar::ExpeditionDestination::kRuins,
                                             stellar::ExpeditionStance::kCautious));
    CHECK(!state.expeditions.empty());
    const stellar::Expedition planned = state.expeditions[0];
    CHECK(planned.return_seconds > planned.depart_seconds);
    CHECK(planned.outcome >= 0 && planned.outcome <= 4);

    // 结果在出发时已经写好：返航报告必须和出发时的预计算结果一致。
    state.fleet = 500;
    stellar::Advance(state, 1300.0);
    CHECK(stellar::ActiveExpeditionCount(state) == 0);
    CHECK(state.expedition_reports.size() == 1);
    CHECK(state.expeditions_done == 1);
    CHECK(state.expedition_reports[0].outcome == planned.outcome);
    CHECK(state.expedition_reports[0].relics == planned.relics);
    CHECK(state.expedition_reports[0].fleet_loss == planned.fleet_loss);

    // 最多三支远征队，且没有舰队时不能出发。
    stellar::GameState many = stellar::MakeNewGame(999.0);
    many.fleet = 1000;
    CHECK(stellar::LaunchExpedition(many, stellar::ExpeditionDestination::kFrontier,
                                    stellar::ExpeditionStance::kDeep));
    CHECK(stellar::LaunchExpedition(many, stellar::ExpeditionDestination::kRuins,
                                    stellar::ExpeditionStance::kDeep));
    CHECK(stellar::LaunchExpedition(many, stellar::ExpeditionDestination::kAnomaly,
                                    stellar::ExpeditionStance::kCautious));
    CHECK(!stellar::LaunchExpedition(many, stellar::ExpeditionDestination::kArk,
                                     stellar::ExpeditionStance::kCautious));
    stellar::GameState empty_fleet = stellar::MakeNewGame(0.0);
    empty_fleet.fleet = 0;
    CHECK(!stellar::LaunchExpedition(empty_fleet, stellar::ExpeditionDestination::kFrontier,
                                     stellar::ExpeditionStance::kCautious));

    stellar::ClearExpeditionReports(state);
    CHECK(state.expedition_reports.empty());
}

void TestErasAndInfluence() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    CHECK(stellar::EraIndex(state) == 0);
    CHECK(stellar::NextEraIn(state) > 0.0);
    stellar::Advance(state, 60.0);
    CHECK(state.influence > 0.0);

    state.play_seconds = 700.0;
    CHECK(stellar::EraIndex(state) == 1);
    state.play_seconds = 8000.0;
    CHECK(stellar::EraIndex(state) == 2);
    state.play_seconds = 15000.0;
    CHECK(stellar::EraIndex(state) == 3);
    state.play_seconds = 22000.0;
    CHECK(stellar::EraIndex(state) == 4);
    CHECK(stellar::NextEraIn(state) == 0.0);
}

void TestCommandCards() {
    stellar::GameState state = stellar::MakeNewGame(0.0);
    CHECK(!stellar::CommandCardsAvailable(state));
    state.tokens = 1000.0;
    state.energy = 1000.0;
    state.food = 1000.0;
    state.alloy = 1000.0;
    state.research = 1000.0;

    // 跨过第一个 15 分钟相位后自动发三张牌。
    state.play_seconds = stellar::kMarketPhaseSeconds + 1.0;
    CHECK(stellar::ChooseCommandCard(state, 0));
    CHECK(state.cards_taken == 1);
    CHECK(!stellar::CommandCardsAvailable(state));
    CHECK(!stellar::ChooseCommandCard(state, 1));

    // 下一个相位会重新发牌。
    state.play_seconds = stellar::kMarketPhaseSeconds * 2.0 + 1.0;
    stellar::Advance(state, 1.0);
    CHECK(stellar::CommandCardsAvailable(state));
    CHECK(stellar::CommandCardSecondsLeft(state) > 0.0);
    CHECK(stellar::CommandCardOffer(state, 0) >= 0);
    CHECK(stellar::CommandCardOffer(state, 2) <
          static_cast<int>(stellar::CommandCardId::kCount));

    // 超过 120 秒窗口后自动失效。
    state.play_seconds = state.card_expires + 1.0;
    CHECK(!stellar::CommandCardsAvailable(state));
    CHECK(!stellar::ChooseCommandCard(state, 0));
}

void TestNewEconomySerialization() {
    stellar::GameState state = stellar::MakeNewGame(7777.0);
    state.play_seconds = 100.0;
    state.influence = 123.5;
    state.tokens = 456.75;
    state.relics = 2;
    state.cores = 1;
    state.data_cores = 3;
    state.reputation = 73;
    state.contracts_done = 2;
    state.expeditions_done = 1;
    state.market_phase = 0;
    state.card_phase = 0;
    state.card_expires = 150.0;
    state.cards_available = true;
    state.card_offers[0] = 3;
    state.card_offers[1] = 4;
    state.card_offers[2] = 5;
    state.cards_taken = 2;
    state.contracts.clear();
    stellar::TradeContract contract;
    contract.resource = 2;
    contract.amount = 120.0;
    contract.reward_tokens = 55.0;
    contract.reward_influence = 4.0;
    state.contracts.push_back(contract);
    state.black_market.clear();
    state.black_market.push_back(stellar::BlackMarketOffer{1, 1, 280.0, 1});
    state.expeditions.clear();
    stellar::Expedition expedition;
    expedition.destination = static_cast<int>(stellar::ExpeditionDestination::kAnomaly);
    expedition.stance = static_cast<int>(stellar::ExpeditionStance::kDeep);
    expedition.fleet = 10;
    expedition.depart_seconds = 100.0;
    expedition.return_seconds = 1900.0;
    expedition.outcome = 0;
    expedition.relics = 1;
    expedition.cores = 1;
    expedition.data_cores = 1;
    expedition.tokens = 100.0;
    expedition.research = 120.0;
    expedition.influence = 5.0;
    expedition.fleet_loss = 2;
    state.expeditions.push_back(expedition);
    state.expedition_reports.clear();
    stellar::ExpeditionReport report;
    report.destination = static_cast<int>(stellar::ExpeditionDestination::kRuins);
    report.stance = static_cast<int>(stellar::ExpeditionStance::kCautious);
    report.outcome = 1;
    report.relics = 1;
    report.tokens = 40.0;
    report.fleet_loss = 1;
    report.return_seconds = 100.0;
    state.expedition_reports.push_back(report);

    const std::string text = stellar::SerializeState(state);
    stellar::GameState loaded{};
    CHECK(stellar::DeserializeState(text, loaded));
    CHECK(NearlyEqual(loaded.influence, 123.5));
    CHECK(NearlyEqual(loaded.tokens, 456.75));
    CHECK(loaded.relics == 2);
    CHECK(loaded.cores == 1);
    CHECK(loaded.data_cores == 3);
    CHECK(loaded.reputation == 73);
    CHECK(loaded.contracts_done == 2);
    CHECK(loaded.expeditions_done == 1);
    CHECK(loaded.cards_taken == 2);
    CHECK(loaded.cards_available);
    CHECK(loaded.card_offers[0] == 3);
    CHECK(loaded.card_offers[1] == 4);
    CHECK(loaded.card_offers[2] == 5);
    CHECK(loaded.contracts.size() == 1);
    CHECK(loaded.contracts[0].resource == 2);
    CHECK(NearlyEqual(loaded.contracts[0].amount, 120.0));
    CHECK(loaded.black_market.size() == 1);
    CHECK(loaded.black_market[0].kind == 1);
    CHECK(loaded.expeditions.size() == 1);
    CHECK(loaded.expeditions[0].destination ==
          static_cast<int>(stellar::ExpeditionDestination::kAnomaly));
    CHECK(loaded.expeditions[0].outcome == 0);
    CHECK(loaded.expedition_reports.size() == 1);
    CHECK(loaded.expedition_reports[0].outcome == 1);
}

}  // namespace

int main() {
    TestNewGameStartsAsCommoner();
    TestAdvanceProducesAndStaysNonNegative();
    TestBuildingCostsAndWallet();
    TestPopulationRespectsCapacity();
    TestResearch();
    TestColonize();
    TestColonizeRequiresPopulation();
    TestColonySpecialization();
    TestNeutralBattle();
    TestPlayerBattleCapturesColony();
    TestFactionChoice();
    TestMoraleFactor();
    TestWonderMechanics();
    TestEventQueueAndResolution();
    TestSectorRaids();
    TestRaidDoesNotCapture();
    TestMarketTradeAndContracts();
    TestBlackMarket();
    TestExpeditions();
    TestErasAndInfluence();
    TestCommandCards();
    TestSerializeRoundTrip();
    TestNewEconomySerialization();

    if (g_failures == 0) {
        std::printf("all stellar model tests passed\n");
        return 0;
    }
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
}
