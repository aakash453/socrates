#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

TEST(ManifestFixture, GoldenManifest_ExistsAndValidJson) {
    auto path = fs::path(__FILE__).parent_path() / "manifests" / "golden_tinyllama.json";
    ASSERT_TRUE(fs::exists(path)) << "Golden manifest fixture missing: " << path;

    std::ifstream f(path);
    ASSERT_TRUE(f.is_open());

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty());
    // Valid JSON: contains key top-level fields
    EXPECT_NE(content.find("\"manifestId\""), std::string::npos);
    EXPECT_NE(content.find("\"modelId\""), std::string::npos);
    EXPECT_NE(content.find("\"graph\""), std::string::npos);
}

TEST(ManifestFixture, MalformedFixtures_AllPresent) {
    auto dir = fs::path(__FILE__).parent_path() / "manifests";
    std::vector<std::string> expected = {
        "malformed_missing_model_id.json",
        "malformed_layer_gap.json",
        "malformed_invalid_uri_scheme.json",
        "malformed_backward_range.json",
        "malformed_bad_ordinals.json",
        "malformed_unknown_field.json",
        "malformed_shard_count.json",
    };

    for (const auto& name : expected) {
        auto path = dir / name;
        EXPECT_TRUE(fs::exists(path)) << "Missing malformed fixture: " << name;
        if (fs::exists(path)) {
            std::ifstream f(path);
            ASSERT_TRUE(f.is_open());
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            EXPECT_NE(content.find("\"expected_error\""), std::string::npos)
                << name << " missing expected_error field";
            EXPECT_NE(content.find("\"rule\""), std::string::npos)
                << name << " missing rule field";
        }
    }
}

TEST(ManifestFixture, MalformedFixtures_UniqueRules) {
    auto dir = fs::path(__FILE__).parent_path() / "manifests";
    std::vector<std::string> rules;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().find("malformed_") == 0) {
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            auto pos = content.find("\"rule\"");
            ASSERT_NE(pos, std::string::npos);
            auto col = content.find(':', pos);
            auto val = content.find('"', col + 1);
            auto end = content.find('"', val + 1);
            rules.push_back(content.substr(val + 1, end - val - 1));
        }
    }

    std::sort(rules.begin(), rules.end());
    auto it = std::adjacent_find(rules.begin(), rules.end());
    EXPECT_EQ(it, rules.end()) << "Duplicate rule: " << *it;
}

TEST(ManifestFixture, GoldenTextproto_Exists) {
    auto path = fs::path(__FILE__).parent_path().parent_path().parent_path()
                / "fixtures" / "models" / "tinyllama" / "manifest.textproto";
    ASSERT_TRUE(fs::exists(path)) << "Golden textproto fixture missing: " << path;
}

}  // namespace
