#include "check.h"
#include "config/store.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

using umbriel::ConfigDiagnostic;
using umbriel::ConfigStore;
using umbriel::LayoutMode;
using umbriel::ModifierKey;

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
          ),
          m_includePath(m_path.string() + ".include") {
      std::filesystem::remove(m_includePath);
    }
    ~TempConfig() {
      std::filesystem::remove(m_path);
      std::filesystem::remove(m_includePath);
    }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;

    void write(const std::string& contents) const {
      std::ofstream stream(m_path);
      stream << contents;
    }

    void writeInclude(const std::string& contents) const {
      std::ofstream stream(m_includePath);
      stream << contents;
    }

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] std::string includeName() const { return m_includePath.filename().string(); }

  private:
    std::filesystem::path m_path;
    std::filesystem::path m_includePath;
  };
} // namespace

UMBRIEL_TEST(sharedLayoutAndNumberReadersPreserveConfigBehavior) {
  const TempConfig file;
  file.write(R"(
unknown_root_key = true
[general]
prefer_no_csd = false

[appearance]
prefer_no_csd = true


[layout]
mode = "dwindle"
width_presets = [0.05, 0.5, 2.0]

[layout.scrolling]
center_underfull_strip = false
always_center_single_column = true

[output.DP-1]
workspaces = ["dev"]
scale = 9.0

[[workspace]]
name = "dev"

[workspace.layout]
mode = "scrolling"
width_presets = [0.25, 0.75]

[workspace.layout.scrolling]
center_underfull_strip = true
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
  CHECK(!store.config().layout.scrolling.centerUnderfullStrip);
  CHECK(store.config().appearance.preferNoCsd);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].scale.has_value());
  CHECK_EQ(*store.config().outputs[0].scale, 4.0);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.mode == LayoutMode::Scrolling);
  CHECK(store.config().workspaceRules[0].layout.widthPresets.has_value());
  CHECK_EQ(store.config().workspaceRules[0].layout.widthPresets->size(), size_t{2});
  CHECK(store.config().workspaceRules[0].layout.scrolling.centerUnderfullStrip == true);
  CHECK(containsDiagnostic(store, "unknown key unknown_root_key"));
  CHECK(containsDiagnostic(store, "output.DP-1.scale = 9"));
  CHECK(containsDiagnostic(store, "unknown key layout.scrolling.always_center_single_column"));
  CHECK(containsDiagnostic(store, "unknown key general.prefer_no_csd"));
}

UMBRIEL_TEST(modKeyIsUserConfigurable) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[general]\nmod_key = \"Ctrl\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().general.modKey == ModifierKey::Control);

  file.write("[general]\nmod_key = \"win\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().general.modKey == ModifierKey::Super);

  file.write("[general]\nmod_key = \"Meta\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().general.modKey.has_value());
  CHECK(containsDiagnostic(store, "unknown general.mod_key"));
}

UMBRIEL_TEST(semanticColorsLoadFromTheirOwnSection) {
  const TempConfig file;
  file.write(R"(
[colors]
background = "#01020304"
text_primary = "#11121314"
text_muted = "#21222324"
accent_primary = "#31323334"
accent_secondary = "#41424344"
warning = "#51525354"
error = "#61626364"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& colors = store.config().colors;

  CHECK(result.success);
  CHECK_EQ(colors.background[0], 1.0F / 255.0F);
  CHECK_EQ(colors.background[3], 4.0F / 255.0F);
  CHECK_EQ(colors.textPrimary[0], 17.0F / 255.0F);
  CHECK_EQ(colors.textMuted[0], 33.0F / 255.0F);
  CHECK_EQ(colors.accentPrimary[0], 49.0F / 255.0F);
  CHECK_EQ(colors.accentSecondary[0], 65.0F / 255.0F);
  CHECK_EQ(colors.warning[0], 81.0F / 255.0F);
  CHECK_EQ(colors.error[0], 97.0F / 255.0F);
  CHECK(!containsDiagnostic(store, "unknown key colors"));
}

UMBRIEL_TEST(missingIncludesRemainPendingUntilTheyLoad) {
  const TempConfig file;
  file.write("[include]\nfiles = [\"" + file.includeName() + "\"]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult missing = store.reload();

  CHECK(missing.success);
  CHECK(store.missingIncludes());
  CHECK(containsDiagnostic(store, "include not found"));

  file.writeInclude("[colors]\naccent_primary = \"#123456FF\"\n");
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK(!store.missingIncludes());
  CHECK(!containsDiagnostic(store, "include not found"));
  CHECK_EQ(store.config().colors.accentPrimary[0], 18.0F / 255.0F);
}

UMBRIEL_TEST(mainFileOverridesIncludedFiles) {
  // Noctalia's rendered theme lands in an include file; the user's root
  // config must win on conflicts while still picking up keys the include
  // alone provides. This is what lets users override generated theme colors.
  const TempConfig file;
  file.write(
      R"(
[colors]
accent_primary = "#ABCDEF00"
[include]
files = [")"
      + file.includeName()
      + R"("]
)"
  );
  file.writeInclude("[colors]\naccent_primary = \"#123456FF\"\nbackground = \"#222222FF\"\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK_EQ(store.config().colors.accentPrimary[0], 171.0F / 255.0F);
  CHECK_EQ(store.config().colors.background[0], 34.0F / 255.0F);
}

UMBRIEL_TEST(activationPolicyLoadsGloballyAndPerWindow) {
  const TempConfig file;
  file.write(R"(
[general]
focus_on_activate = true

[[window_rule]]
match.app_id = "^game$"
focus_on_activate = false
default_position = { x = 32, y = 48, anchor = "bottom_left" }

[[window_rule]]
match.app_id = "^centered$"
default_position = { x = 0, y = 0 }
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().general.focusOnActivate);
  CHECK_EQ(store.config().windowRules.size(), size_t{2});
  CHECK(store.config().windowRules[0].focusOnActivate.has_value());
  CHECK(!*store.config().windowRules[0].focusOnActivate);
  CHECK(store.config().windowRules[0].defaultPosition.has_value());
  CHECK_EQ(store.config().windowRules[0].defaultPosition->x, 32);
  CHECK_EQ(store.config().windowRules[0].defaultPosition->y, 48);
  CHECK(store.config().windowRules[0].defaultPosition->anchor == umbriel::WindowPositionAnchor::BottomLeft);
  CHECK(store.config().windowRules[1].defaultPosition.has_value());
  CHECK(store.config().windowRules[1].defaultPosition->anchor == umbriel::WindowPositionAnchor::Center);
}

UMBRIEL_TEST(deviceInputOverridesLoadAndMatchExactNames) {
  const TempConfig file;
  file.write(R"(
[input.keyboard]
layout = "us"
repeat_rate = 25

[input.touchpad]
tap = true
natural_scroll = true

[input.mouse]
accel_profile = "custom 0.2 0.0 0.5 1.0 2.0"
sensitivity = 0.25

[[input.device]]
name = "Acme Split Keyboard"
layout = ""
variant = ""
repeat_rate = 40
repeat_delay = 250

[[input.device]]
name = "Acme Precision Touchpad"
tap = false
natural_scroll = false

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = -0.5
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& input = store.config().input;

  CHECK(result.success);
  CHECK(input.mouse.accelProfile.kind == umbriel::AccelProfile::Kind::Custom);
  CHECK_EQ(input.mouse.accelProfile.step, 0.2);
  CHECK_EQ(input.mouse.accelProfile.points, std::vector<double>({0.0, 0.5, 1.0, 2.0}));
  CHECK_EQ(input.mouse.sensitivity, 0.25);
  CHECK_EQ(input.devices.size(), size_t{3});

  const auto* keyboard = input.findDevice("Acme Split Keyboard");
  CHECK(keyboard != nullptr);
  if (keyboard != nullptr) {
    CHECK(keyboard->layout == std::optional<std::string>(""));
    CHECK(keyboard->variant == std::optional<std::string>(""));
    CHECK(keyboard->repeatRate == std::optional<int>(40));
    CHECK(keyboard->repeatDelay == std::optional<int>(250));
  }

  const auto* touchpad = input.findDevice("Acme Precision Touchpad");
  CHECK(touchpad != nullptr);
  if (touchpad != nullptr) {
    CHECK(touchpad->tap == std::optional<bool>(false));
    CHECK(touchpad->naturalScroll == std::optional<bool>(false));
  }

  const auto* mouse = input.findDevice("Acme Gaming Mouse");
  CHECK(mouse != nullptr);
  if (mouse != nullptr) {
    CHECK(mouse->accelProfile.has_value());
    CHECK(mouse->accelProfile->kind == umbriel::AccelProfile::Kind::Flat);
    CHECK(mouse->sensitivity == std::optional<double>(-0.5));
  }

  CHECK(input.findDevice("acme split keyboard") == nullptr);
  CHECK(input.findDevice("Acme") == nullptr);
}

UMBRIEL_TEST(mouseAccelerationDefaultsToFlat) {
  const umbriel::Config defaults;
  CHECK(defaults.input.mouse.accelProfile.kind == umbriel::AccelProfile::Kind::Flat);
  CHECK_EQ(defaults.input.mouse.sensitivity, 0.0);
}

UMBRIEL_TEST(invalidCustomAccelerationCurveIsRejected) {
  const TempConfig file;
  file.write(R"(
[input.mouse]
accel_profile = "custom 0.2 1.0"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().input.mouse.accelProfile.kind == umbriel::AccelProfile::Kind::Flat);
  CHECK(containsDiagnostic(store, "custom <step> <points...>"));
}

UMBRIEL_TEST(keyboardOptionsLoadGloballyAndPerDevice) {
  const TempConfig file;
  file.write(R"(
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"

[[input.device]]
name = "Acme Split Keyboard"
layout = "us,fr"
options = "grp:win_space_toggle"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& input = store.config().input;

  CHECK(result.success);
  CHECK(!containsDiagnostic(store, "unknown key input.keyboard.options"));
  CHECK(!containsDiagnostic(store, "invalid XKB configuration"));
  CHECK_EQ(input.keyboard.layout, std::string{"us,de"});
  CHECK_EQ(input.keyboard.options, std::string{"grp:alt_shift_toggle"});

  const auto* device = input.findDevice("Acme Split Keyboard");
  CHECK(device != nullptr);
  if (device != nullptr) {
    CHECK(device->layout == std::optional<std::string>("us,fr"));
    CHECK(device->options == std::optional<std::string>("grp:win_space_toggle"));
  }
}

int main() { return RUN_TESTS(); }
