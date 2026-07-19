from conan import ConanFile


class SocratesEngineConan(ConanFile):
    name = "socrates"
    version = "0.1.0"
    description = "Edge AI Runtime Engine"
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("spdlog/1.14.1")
        self.requires("nlohmann_json/3.11.3")
        self.requires("tl-expected/20190710")
        self.requires("sqlite3/3.46.0")
        self.requires("gtest/1.15.0")

    def build_requirements(self):
        self.tool_requires("cmake/[>=3.22]")

    def configure(self):
        self.options["*"].shared = False
