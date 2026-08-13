#include "check.h"
#include "config/store.h"

using umbriel::Config;
using umbriel::ConfigChange;
using umbriel::Keybind;
using umbriel::LayerRule;
using umbriel::OutputRule;
using umbriel::WindowRule;

UMBRIEL_TEST(anIdenticalConfigChangesNothing) {
  const Config before;
  const Config after;
  const ConfigChange change = ConfigChange::between(before, after);
  // The whole point of the item: a reload that parsed the same file again must
  // report nothing to do, so nothing is re-applied and nothing flickers.
  CHECK(!change.any());
}

UMBRIEL_TEST(aFirstLoadReportsEverything) {
  const ConfigChange change = ConfigChange::everything();
  CHECK(change.any());
  CHECK(change.appearance);
  CHECK(change.input);
  CHECK(change.outputs);
}

UMBRIEL_TEST(eachSectionIsReportedOnItsOwn) {
  const Config before;

  {
    Config after;
    after.appearance.borderWidth += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.appearance);
    // A border width change must not reprogram keyboards or reconfigure outputs.
    CHECK(!change.input);
    CHECK(!change.outputs);
    CHECK(!change.keybinds);
  }
  {
    Config after;
    after.input.keyboard.repeatRate += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.input);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.layout.gap += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.layout);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.general.xwayland = !after.general.xwayland;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.general);
    CHECK(!change.input);
  }
  {
    Config after;
    after.overview.zoom += 0.1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.overview);
    CHECK(!change.layout);
  }
  {
    Config after;
    after.workspaces.backAndForth = !after.workspaces.backAndForth;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.workspaces);
    CHECK(!change.workspaceRules);
  }
}

UMBRIEL_TEST(nestedAppearanceChangesAreCaught) {
  const Config before;
  Config after;
  // Nested structs need their own comparison, so a change buried a level down is
  // exactly what a defaulted operator== would miss if one were forgotten.
  after.appearance.blur.radius += 1;
  CHECK(ConfigChange::between(before, after).appearance);

  Config shadowed;
  shadowed.appearance.shadow.offsetX += 1;
  CHECK(ConfigChange::between(before, shadowed).appearance);

  Config scrolled;
  scrolled.layout.scrolling.defaultWidthFraction += 0.1;
  CHECK(ConfigChange::between(before, scrolled).layout);

  Config focused;
  focused.input.focus.followsMouse = !focused.input.focus.followsMouse;
  CHECK(ConfigChange::between(before, focused).input);
}

UMBRIEL_TEST(colorChangesAreCaught) {
  const Config before;
  Config after;
  after.appearance.borderFocused[0] += 0.1F;
  CHECK(ConfigChange::between(before, after).appearance);
}

UMBRIEL_TEST(listSectionsAreCompared) {
  const Config before;

  {
    Config after;
    after.keybinds.push_back(Keybind{});
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.keybinds);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.outputs.push_back(OutputRule{});
    CHECK(ConfigChange::between(before, after).outputs);
  }
  {
    Config after;
    after.windowRules.push_back(WindowRule{});
    CHECK(ConfigChange::between(before, after).windowRules);
  }
  {
    Config after;
    after.layerRules.push_back(LayerRule{});
    CHECK(ConfigChange::between(before, after).layerRules);
  }
}

UMBRIEL_TEST(ruleEqualityIgnoresTheCompiledRegex) {
  // Two rules built from the same pattern are the same rule, even though their
  // std::regex members are distinct objects that cannot be compared at all.
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  first.appIdRegex = std::regex(first.appIdPattern);
  WindowRule second;
  second.appIdPattern = "kitty";
  second.appIdRegex = std::regex(second.appIdPattern);
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(!ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(ruleEqualityStillSeesAPatternChange) {
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  WindowRule second;
  second.appIdPattern = "foot";
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(ruleEqualitySeesAnOptionChangeUnderTheSamePattern) {
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  WindowRule second;
  second.appIdPattern = "kitty";
  second.opacity = 0.9;
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(ConfigChange::between(before, after).windowRules);
}

int main() { return RUN_TESTS(); }
