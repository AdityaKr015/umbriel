#include "check.h"
#include "config/store.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using umbriel::ConfigDiagnostic;
using umbriel::ConfigStore;
using umbriel::LayoutMode;

namespace {
  bool containsDiagnostic(const ConfigStore& store, const std::string& text) {
    for (const ConfigDiagnostic& diagnostic : store.diagnostics()) {
      if (diagnostic.message.contains(text)) {
        return true;
      }
    }
    return false;
  }

  class TempConfig {
  public:
    TempConfig()
        : m_path(
              std::filesystem::temp_directory_path() / ("umbriel-config-load-" + std::to_string(getpid()) + ".toml")
          ) {}
    ~TempConfig() { std::filesystem::remove(m_path); }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;

    void write(const std::string& contents) const {
      std::ofstream stream(m_path);
      stream << contents;
    }

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
  };
} // namespace

UMBRIEL_TEST(sharedLayoutAndNumberReadersPreserveConfigBehavior) {
  const TempConfig file;
  file.write(R"(
unknown_root_key = true

[layout]
mode = "dwindle"
width_presets = [0.05, 0.5, 2.0]

[output.DP-1]
workspaces = ["dev"]
scale = 9.0

[[workspace]]
name = "dev"

[workspace.layout]
mode = "scrolling"
width_presets = [0.25, 0.75]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().layout.mode == LayoutMode::Dwindle);
  CHECK_EQ(store.config().layout.widthPresets.size(), size_t{3});
  CHECK_EQ(store.config().layout.widthPresets[0], 0.1);
  CHECK_EQ(store.config().layout.widthPresets[1], 0.5);
  CHECK_EQ(store.config().layout.widthPresets[2], 1.0);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].scale.has_value());
  CHECK_EQ(*store.config().outputs[0].scale, 4.0);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.mode == LayoutMode::Scrolling);
  CHECK(store.config().workspaceRules[0].layout.widthPresets.has_value());
  CHECK_EQ(store.config().workspaceRules[0].layout.widthPresets->size(), size_t{2});
  CHECK(containsDiagnostic(store, "unknown key unknown_root_key"));
  CHECK(containsDiagnostic(store, "output.DP-1.scale = 9"));
}

int main() { return RUN_TESTS(); }
