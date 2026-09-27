#pragma once

#include "config/section.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Declared config keys. Each table of the config file is a list of Fields over the struct it fills. Reading walks the
// list through Section; describing walks the same list, so the schema is the parser's own declaration.
namespace umbriel::registry {

  // State one load shares across tables, such as effect references resolved once every table is read. Defined by the
  // loader; fields only pass it through to the parsers that need it.
  struct ReadContext;

  // One key as `umbriel config schema` reports it.
  struct KeyDescription {
    explicit KeyDescription(std::string_view keyType) : type(keyType) {}

    [[nodiscard]] KeyDescription withRange(double minimum, double maximum) && {
      min = minimum;
      max = maximum;
      return std::move(*this);
    }
    [[nodiscard]] KeyDescription withValues(std::vector<std::string_view> accepted) && {
      values = std::move(accepted);
      return std::move(*this);
    }
    [[nodiscard]] KeyDescription withFormat(std::string_view meaning) && {
      format = meaning;
      return std::move(*this);
    }

    std::string path;
    std::string_view type;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string_view> values;
    std::string_view format;
    // null when the key has no built-in value.
    nlohmann::ordered_json defaultValue;
  };

  using Descriptions = std::vector<KeyDescription>;

  template <typename T> struct Field {
    std::string_view key;
    std::function<void(Section&, T&, ReadContext&)> read;
    // Appends this key, and any keys beneath it, as found in `defaults`.
    std::function<void(const T& defaults, const std::string& path, Descriptions&)> describe;
  };

  template <typename T> using Fields = std::vector<Field<T>>;

  template <typename T> void readFields(Section& section, const Fields<T>& fields, T& target, ReadContext& context) {
    for (const Field<T>& field : fields) {
      field.read(section, target, context);
    }
  }

  template <typename T>
  void describeFields(const Fields<T>& fields, const T& defaults, const std::string& prefix, Descriptions& out) {
    for (const Field<T>& field : fields) {
      field.describe(defaults, prefix.empty() ? std::string(field.key) : std::format("{}.{}", prefix, field.key), out);
    }
  }

  // Every declared key, with the built-in values as defaults.
  [[nodiscard]] Descriptions describeConfig();

  namespace detail {

    template <typename V> nlohmann::ordered_json toJson(const V& value) { return value; }
    template <typename V> nlohmann::ordered_json toJson(const std::optional<V>& value) {
      return value ? toJson(*value) : nlohmann::ordered_json();
    }

    inline void push(Descriptions& out, KeyDescription entry, const std::string& path) {
      entry.path = path;
      out.push_back(std::move(entry));
    }

    template <typename T, typename Current> auto leaf(KeyDescription shape, Current current) {
      return [shape = std::move(shape), current](const T& defaults, const std::string& path, Descriptions& out) {
        KeyDescription entry = shape;
        entry.defaultValue = current(defaults);
        push(out, std::move(entry), path);
      };
    }

    template <typename V> struct Unwrapped {
      using type = V;
    };
    template <typename V> struct Unwrapped<std::optional<V>> {
      using type = V;
    };

    inline bool sameName(std::string_view a, std::string_view b, bool foldCase) {
      if (!foldCase) {
        return a == b;
      }
      return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
      });
    }

  } // namespace detail

  template <typename T, typename V> Field<T> integer(std::string_view key, int minimum, int maximum, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.integer(key, minimum, maximum, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("int").withRange(minimum, maximum), [=](const T& d) {
          return detail::toJson(d.*member);
        }),
    };
  }

  template <typename T, typename V> Field<T> real(std::string_view key, double minimum, double maximum, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.real(key, minimum, maximum, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("float").withRange(minimum, maximum), [=](const T& d) {
          return detail::toJson(d.*member);
        }),
    };
  }

  template <typename T, typename V> Field<T> boolean(std::string_view key, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.boolean(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("bool"), [=](const T& d) { return detail::toJson(d.*member); }),
    };
  }

  template <typename T, typename V> Field<T> text(std::string_view key, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.text(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("string"), [=](const T& d) { return detail::toJson(d.*member); }),
    };
  }

  template <typename T> Field<T> strings(std::string_view key, std::vector<std::string> T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.strings(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("string_array"), [=](const T& d) {
          return nlohmann::ordered_json(d.*member);
        }),
    };
  }

  // `#rrggbbaa`, the form a color is written in.
  [[nodiscard]] inline std::string formatColor(const std::array<float, 4>& color) {
    std::string text = "#";
    for (const float component : color) {
      text += std::format("{:02x}", static_cast<int>(std::lround(std::clamp(component, 0.0F, 1.0F) * 255.0F)));
    }
    return text;
  }

  template <typename T, typename V> Field<T> color(std::string_view key, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target, ReadContext&) { s.color(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("color"), [=](const T& d) -> nlohmann::ordered_json {
          if constexpr (std::is_same_v<V, std::array<float, 4>>) {
            return formatColor(d.*member);
          } else {
            return d.*member ? nlohmann::ordered_json(formatColor(*(d.*member))) : nlohmann::ordered_json();
          }
        }),
    };
  }

  template <typename E> struct Choice {
    std::string_view name;
    E value;
    // Accepted, but neither listed nor reported as the default: `Logo` for `Super`.
    bool alias = false;
  };

  template <typename E> using Choices = std::vector<Choice<E>>;

  enum class Case : std::uint8_t { Exact, Fold };

  // The listed spellings of `choices`, and the name `value` is reported by.
  template <typename E> [[nodiscard]] std::vector<std::string_view> choiceNames(const Choices<E>& choices) {
    std::vector<std::string_view> names;
    names.reserve(choices.size());
    for (const Choice<E>& option : choices) {
      if (!option.alias) {
        names.push_back(option.name);
      }
    }
    return names;
  }

  template <typename E> [[nodiscard]] nlohmann::ordered_json choiceName(const Choices<E>& choices, const E& value) {
    const auto found =
        std::ranges::find_if(choices, [&](const Choice<E>& option) { return !option.alias && option.value == value; });
    return found == choices.end() ? nlohmann::ordered_json() : nlohmann::ordered_json(found->name);
  }

  // `"a"`, `"a" or "b"`, `"a", "b", or "c"`.
  [[nodiscard]] inline std::string quotedList(const std::vector<std::string_view>& names) {
    std::string out;
    for (size_t index = 0; index < names.size(); ++index) {
      if (index > 0) {
        out += names.size() == 2 ? " or " : index + 1 == names.size() ? ", or " : ", ";
      }
      out += std::format("\"{}\"", names[index]);
    }
    return out;
  }

  // The choice `node` names, or nullopt after reporting `path` as ignored.
  template <typename E>
  std::optional<E>
  readChoice(Section& s, const toml::node& node, const std::string& path, const Choices<E>& choices, Case match) {
    const auto value = node.value<std::string_view>();
    const auto found = std::ranges::find_if(choices, [&](const Choice<E>& option) {
      return value && detail::sameName(*value, option.name, match == Case::Fold);
    });
    if (found != choices.end()) {
      return found->value;
    }
    const std::string given = value ? std::format(" \"{}\"", *value) : std::string();
    s.warn(node, std::format("ignoring {}{} (expected {})", path, given, quotedList(choiceNames(choices))));
    return std::nullopt;
  }

  // Store a parsed value, when there is one, into a plain or optional target.
  template <typename V, typename E> void assign(V& target, std::optional<E> value) {
    if (!value) {
      return;
    }
    if constexpr (std::is_same_v<V, std::optional<E>>) {
      target = std::move(value);
    } else {
      target = std::move(*value);
    }
  }

  // A string naming one of `choices`. Anything else is reported and leaves the target alone.
  template <typename T, typename V>
  Field<T> choice(
      std::string_view key, V T::* member, Choices<typename detail::Unwrapped<V>::type> choices,
      Case match = Case::Exact
  ) {
    using E = typename detail::Unwrapped<V>::type;
    KeyDescription shape = KeyDescription("enum").withValues(choiceNames(choices));
    return {
        .key = key,
        .read =
            [=](Section& s, T& target, ReadContext&) {
              if (const toml::node* node = s.take(key)) {
                assign(target.*member, readChoice(s, *node, s.qualified(key), choices, match));
              }
            },
        .describe = detail::leaf<T>(std::move(shape), [=](const T& d) -> nlohmann::ordered_json {
          if constexpr (std::is_same_v<V, E>) {
            return choiceName(choices, d.*member);
          } else {
            return d.*member ? choiceName(choices, *(d.*member)) : nlohmann::ordered_json();
          }
        }),
    };
  }

  template <typename T> using Parse = std::function<void(const toml::node&, const std::string& path, T&, ReadContext&)>;
  template <typename T> using Current = std::function<nlohmann::ordered_json(const T&)>;

  // A key whose parsing is its own: `parse` gets the node and the key's full path for its diagnostics. `current`
  // reports the built-in value, when there is one; `extra` lists keys beneath it, such as a number-or-table's axes.
  template <typename T>
  Field<T> custom(
      std::string_view key, KeyDescription shape, std::type_identity_t<Parse<T>> parse,
      std::type_identity_t<Current<T>> current = nullptr, Descriptions extra = {}
  ) {
    return {
        .key = key,
        .read =
            [=](Section& s, T& target, ReadContext& context) {
              if (const toml::node* node = s.take(key)) {
                parse(*node, s.qualified(key), target, context);
              }
            },
        .describe =
            [shape = std::move(shape), current = std::move(current),
             extra = std::move(extra)](const T& defaults, const std::string& path, Descriptions& out) {
              KeyDescription entry = shape;
              if (current) {
                entry.defaultValue = current(defaults);
              }
              detail::push(out, std::move(entry), path);
              for (const KeyDescription& child : extra) {
                detail::push(out, child, std::format("{}.{}", path, child.path));
              }
            },
    };
  }

  // A nested table reached through `project`, which maps the parent (const or not) to it. `after` runs once the table
  // is read, when the key is present, for checks across its keys.
  template <typename T, typename Project>
  Field<T> table(
      std::string_view key, Project project,
      const Fields<std::remove_cvref_t<decltype(std::declval<Project>()(std::declval<T&>()))>>& fields,
      std::type_identity_t<std::function<
          void(const toml::node&, std::remove_cvref_t<decltype(std::declval<Project>()(std::declval<T&>()))>&)>>
          after = nullptr
  ) {
    return {
        .key = key,
        .read =
            [=, &fields](Section& s, T& target, ReadContext& context) {
              s.sub(key, [&](Section& child) { readFields(child, fields, project(target), context); });
              if (after) {
                if (const toml::node* node = s.node(key)) {
                  after(*node, project(target));
                }
              }
            },
        .describe =
            [=, &fields](const T& defaults, const std::string& path, Descriptions& out) {
              detail::push(out, KeyDescription("table"), path);
              describeFields(fields, project(defaults), path, out);
            },
    };
  }

  template <typename T, typename C>
  Field<T> table(
      std::string_view key, C T::* member, const Fields<C>& fields,
      std::type_identity_t<std::function<void(const toml::node&, C&)>> after = nullptr
  ) {
    return table<T>(key, [member](auto& parent) -> auto& { return parent.*member; }, fields, std::move(after));
  }

  // A table whose keys are names the user picks, such as environment variables. `read` gets the table's Section,
  // which reports no unknown keys; `entry` describes what each name holds.
  template <typename T>
  Field<T>
  map(std::string_view key, KeyDescription entry,
      std::type_identity_t<std::function<void(Section&, T&, ReadContext&)>> read, Descriptions entryKeys = {},
      std::string_view containerType = "map") {
    return {
        .key = key,
        .read =
            [=](Section& s, T& target, ReadContext& context) {
              s.sub(key, [&](Section& child) {
                child.freeform();
                read(child, target, context);
              });
            },
        .describe =
            [entry = std::move(entry), entryKeys = std::move(entryKeys),
             containerType](const T&, const std::string& path, Descriptions& out) {
              detail::push(out, KeyDescription(containerType), path);
              const std::string name = path + ".<name>";
              detail::push(out, entry, name);
              for (const KeyDescription& child : entryKeys) {
                detail::push(out, child, std::format("{}.{}", name, child.path));
              }
            },
    };
  }

  // An array of tables, one entry per rule. `accept` sees each read entry with the parent it lands in, whose array
  // holds the entries kept so far, and decides whether it is kept; it reports its own reasons.
  template <typename T, typename C>
  Field<T> rules(
      std::string_view key, std::vector<C> T::* member, const Fields<C>& fields,
      std::type_identity_t<
          std::function<bool(const toml::node& entry, const std::string& context, C&, const T& parent, ReadContext&)>>
          accept
  ) {
    return {
        .key = key,
        .read =
            [=, &fields](Section& s, T& target, ReadContext& context) {
              const toml::node* node = s.take(key);
              if (node == nullptr) {
                return;
              }
              const std::string path = s.qualified(key);
              const toml::array* entries = node->as_array();
              if (entries == nullptr) {
                s.error(*node, std::format("{} must be a [[{}]] array of tables", path, path));
                return;
              }
              size_t index = 0;
              for (const toml::node& entry : *entries) {
                const std::string entryPath = std::format("{}[{}]", path, index++);
                const toml::table* entryTable = entry.as_table();
                if (entryTable == nullptr) {
                  s.error(entry, std::format("{} must be a table", entryPath));
                  continue;
                }
                C rule;
                {
                  Section keys(*entryTable, entryPath, s.diagnostics());
                  readFields(keys, fields, rule, context);
                }
                if (accept(entry, entryPath, rule, target, context)) {
                  (target.*member).push_back(std::move(rule));
                }
              }
            },
        .describe =
            [&fields](const T&, const std::string& path, Descriptions& out) {
              detail::push(out, KeyDescription("array_of_tables"), path);
              describeFields(fields, C{}, path + "[]", out);
            },
    };
  }

} // namespace umbriel::registry
