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
    defender.colonies.push_back(stellar::Colony{1});
    defender.minerals = 500.0;
    defender.alloy = 200.0;

    const stellar::BattleResult result = stellar::AttackPlayer(attacker, defender);
    CHECK(result.won);
    CHECK(result.captured);
    CHECK(attacker.colonies.size() == 1);
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
    CHECK(!stellar::DeserializeState("", loaded));
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
    TestNeutralBattle();
    TestPlayerBattleCapturesColony();
    TestSerializeRoundTrip();

    if (g_failures == 0) {
        std::printf("all stellar model tests passed\n");
        return 0;
    }
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
}
