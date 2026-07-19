#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

TEST(ProtobufSchema, AllProtoFilesExist) {
    auto proto_dir = fs::path(__FILE__).parent_path().parent_path().parent_path()
                     / "proto" / "socrates" / "v1";
    ASSERT_TRUE(fs::exists(proto_dir)) << "Proto directory not found";

    std::vector<std::string> expected = {
        "model_manifest.proto",
        "control_plane.proto",
        "data_plane.proto",
        "trace.proto",
    };

    for (const auto& name : expected) {
        EXPECT_TRUE(fs::exists(proto_dir / name))
            << "Missing proto file: " << name;
    }
}

TEST(ProtobufSchema, ModelManifest_HasRequiredMessages) {
    auto path = fs::path(__FILE__).parent_path().parent_path().parent_path()
                / "proto" / "socrates" / "v1" / "model_manifest.proto";
    auto content = read_file(path);
    EXPECT_NE(content.find("ModelManifestEnvelope"), std::string::npos);
    EXPECT_NE(content.find("ModelManifest"), std::string::npos);
    EXPECT_NE(content.find("Graph"), std::string::npos);
    EXPECT_NE(content.find("QuantizationIdentity"), std::string::npos);
    EXPECT_NE(content.find("ArtifactSet"), std::string::npos);
    EXPECT_NE(content.find("ExecutionProfile"), std::string::npos);
    EXPECT_NE(content.find("CalibrationEntry"), std::string::npos);
    EXPECT_NE(content.find("IntegrityConstraints"), std::string::npos);
}

TEST(ProtobufSchema, ControlPlane_HasRequiredMessages) {
    auto path = fs::path(__FILE__).parent_path().parent_path().parent_path()
                / "proto" / "socrates" / "v1" / "control_plane.proto";
    auto content = read_file(path);
    EXPECT_NE(content.find("DiscoveryAnnouncement"), std::string::npos);
    EXPECT_NE(content.find("MembershipSnapshot"), std::string::npos);
    EXPECT_NE(content.find("CapabilityReport"), std::string::npos);
    EXPECT_NE(content.find("ElectionMessage"), std::string::npos);
    EXPECT_NE(content.find("ScheduleRequest"), std::string::npos);
    EXPECT_NE(content.find("PipelinePlan"), std::string::npos);
    EXPECT_NE(content.find("ControlPlane"), std::string::npos);
}

TEST(ProtobufSchema, DataPlane_HasRequiredMessages) {
    auto path = fs::path(__FILE__).parent_path().parent_path().parent_path()
                / "proto" / "socrates" / "v1" / "data_plane.proto";
    auto content = read_file(path);
    EXPECT_NE(content.find("ActivationFrame"), std::string::npos);
    EXPECT_NE(content.find("TokenEvent"), std::string::npos);
    EXPECT_NE(content.find("GenerateRequest"), std::string::npos);
    EXPECT_NE(content.find("DataPlane"), std::string::npos);
}

TEST(ProtobufSchema, Trace_HasRequiredMessages) {
    auto path = fs::path(__FILE__).parent_path().parent_path().parent_path()
                / "proto" / "socrates" / "v1" / "trace.proto";
    auto content = read_file(path);
    EXPECT_NE(content.find("Trace"), std::string::npos);
    EXPECT_NE(content.find("TraceSpan"), std::string::npos);
    EXPECT_NE(content.find("TraceExport"), std::string::npos);
}

TEST(ProtobufSchema, NoReservedFieldDuplicates) {
    // Field numbers across the package should not conflict at the top level.
    // This test verifies our schemas don't define the same enum in multiple files.
    auto dir = fs::path(__FILE__).parent_path().parent_path().parent_path()
               / "proto" / "socrates" / "v1";

    for (const auto& entry : fs::directory_iterator(dir)) {
        auto content = read_file(entry.path());
        // BackendKind should only be in model_manifest
        if (entry.path().filename() != "model_manifest.proto") {
            EXPECT_EQ(content.find("enum BackendKind"), std::string::npos)
                << entry.path().filename() << " redefines BackendKind";
            EXPECT_EQ(content.find("enum ComputeUnit"), std::string::npos)
                << entry.path().filename() << " redefines ComputeUnit";
        }
    }
}

}  // namespace
