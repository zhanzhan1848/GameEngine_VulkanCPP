#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/ProceduralRoomPack.h"

#include <set>
#include <string>

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestTileCountIs12() {
    TEST_ASSERT_EQ(12u, ProceduralRoomPack::kTileCount, "must be 12 tiles");
    return TestResult::Passed;
}

TestResult TestTileDefsCoverMatrix() {
    bool seen_size[3]  = {false, false, false};
    bool seen_doors[4] = {false, false, false, false};
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        const auto& def = ProceduralRoomPack::TileDefs()[i];
        u32 size_idx = (def.footprint_cells - 3) / 2;
        TEST_ASSERT(size_idx < 3u, "size must be 3/5/7");
        seen_size[size_idx] = true;
        TEST_ASSERT(def.door_mask < 16u, "door_mask must fit 4 bits");
        seen_doors[0] |= (def.door_mask == 0x1);
        seen_doors[1] |= (def.door_mask == 0x3);
        seen_doors[2] |= (def.door_mask == 0xC);
        seen_doors[3] |= (def.door_mask == 0xF);
    }
    for (u32 i = 0; i < 3; ++i) TEST_ASSERT(seen_size[i],  "all sizes present");
    for (u32 i = 0; i < 4; ++i) TEST_ASSERT(seen_doors[i], "all door configs present");
    return TestResult::Passed;
}

TestResult TestTileNamesAreUnique() {
    std::set<std::string> names;
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        names.insert(ProceduralRoomPack::TileDefs()[i].name);
    }
    TEST_ASSERT_EQ(12u, names.size(), "all names unique");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ProceduralRoomPack");
    TEST_CASE(suite, "TileCountIs12",        TestTileCountIs12);
    TEST_CASE(suite, "TileDefsCoverMatrix",  TestTileDefsCoverMatrix);
    TEST_CASE(suite, "TileNamesAreUnique",   TestTileNamesAreUnique);
    suite.RunAllTests();
    return 0;
}
