#pragma once

#include "config/section.h"

#include <algorithm>
#include <cctype>
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
    std::function<void(Section&, T&)> read;
    // Appends this key, and any keys beneath it, as found in `defaults`.
    std::function<void(const T& defaults, const std::string& path, Descriptions&)> describe;
  };

  template <typename T> using Fields = std::vector<Field<T>>;

  template <typename T> void readFields(Section& section, const Fields<T>& fields, T& target) {
    for (const Field<T>& field : fields) {
      field.read(section, target);
    }
  }

  template <typename T>
  void describeFields(const Fields<T>& fields, const T& defaults, const std::string& prefix, Descriptions& out) {
    for (const Field<T>& field : fields) {
      field.describe(defaults, prefix.empty() ? std::string(field.key) : std::format("{}.{}", prefix, field.key), out);
    }
  }

  namespace detail {

    template <typename V> nlohmann::ordered_json toJson(const V& value) { return value; }
    template <typename V> nlohmann::ordered_json toJson(const std::optional<V>& value) {
      return value ? toJson(*value) : nlohmann::ordered_json();
    }

    template <typename T, typename Current> auto leaf(KeyDescription shape, Current current) {
      return [shape = std::move(shape), current](const T& defaults, const std::string& path, Descriptions& out) {
        KeyDescription entry = shape;
        entry.path = path;
        entry.defaultValue = current(defaults);
        out.push_back(std::move(entry));
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

    // `"a"`, `"a" or "b"`, `"a", "b", or "c"`.
    template <typename Choices> std::string quotedList(const Choices& choices) {
      std::string out;
      for (size_t index = 0; index < choices.size(); ++index) {
        if (index > 0) {
          out += choices.size() == 2 ? " or " : index + 1 == choices.size() ? ", or " : ", ";
        }
        out += std::format("\"{}\"", choices[index].name);
      }
      return out;
    }

  } // namespace detail

  template <typename T, typename V> Field<T> integer(std::string_view key, int minimum, int maximum, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target) { s.integer(key, minimum, maximum, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("int").withRange(minimum, maximum), [=](const T& d) {
          return detail::toJson(d.*member);
        }),
    };
  }

  template <typename T, typename V> Field<T> real(std::string_view key, double minimum, double maximum, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target) { s.real(key, minimum, maximum, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("float").withRange(minimum, maximum), [=](const T& d) {
          return detail::toJson(d.*member);
        }),
    };
  }

  template <typename T, typename V> Field<T> boolean(std::string_view key, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target) { s.boolean(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("bool"), [=](const T& d) { return detail::toJson(d.*member); }),
    };
  }

  template <typename T, typename V> Field<T> text(std::string_view key, V T::* member) {
    return {
        .key = key,
        .read = [=](Section& s, T& target) { s.text(key, target.*member); },
        .describe = detail::leaf<T>(KeyDescription("string"), [=](const T& d) { return detail::toJson(d.*member); }),
    };
  }

  template <typename E> struct Choice {
    std::string_view name;
    E value;
  };

  enum class Case : std::uint8_t { Exact, Fold };

  // A string naming one of `choices`. Anything else is reported and leaves the target alone.
  template <typename T, typename V>
  Field<T> choice(
      std::string_view key, V T::* member, std::vector<Choice<typename detail::Unwrapped<V>::type>> choices,
      Case match = Case::Exact
  ) {
    std::vector<std::string_view> names;
    names.reserve(choices.size());
    for (const auto& option : choices) {
      names.push_back(option.name);
    }
    return {
        .key = key,
        .read =
            [=](Section& s, T& target) {
              const toml::node* node = s.take(key);
              if (node == nullptr) {
                return;
              }
              const auto value = node->value<std::string_view>();
              const auto found = std::ranges::find_if(choices, [&](const auto& option) {
                return value && detail::sameName(*value, option.name, match == Case::Fold);
              });
              if (found == choices.end()) {
                const std::string given = value ? std::format(" \"{}\"", *value) : std::string();
                s.warn(
                    *node,
                    std::format("ignoring {}{} (expected {})", s.qualified(key), given, detail::quotedList(choices))
                );
                return;
              }
              target.*member = found->value;
            },
        .describe =
            detail::leaf<T>(KeyDescription("enum").withValues(names), [=](const T& d) -> nlohmann::ordered_json {
              const auto& current = d.*member;
              if constexpr (std::is_same_v<V, typename detail::Unwrapped<V>::type>) {
                const auto found = std::ranges::find(choices, current, &Choice<V>::value);
                return found == choices.end() ? nlohmann::ordered_json() : nlohmann::ordered_json(found->name);
              } else {
                if (!current) {
                  return nullptr;
                }
                const auto found = std::ranges::find(choices, *current, &Choice<typename V::value_type>::value);
                return found == choices.end() ? nlohmann::ordered_json() : nlohmann::ordered_json(found->name);
              }
            }),
    };
  }

  // A key whose parsing is its own: `parse` gets the node and the key's full path for its diagnostics. `extra` lists
  // keys beneath it, such as the axes of a number-or-table key.
  template <typename T>
  Field<T> custom(
      std::string_view key, KeyDescription shape,
      std::type_identity_t<std::function<void(const toml::node&, const std::string& path, T&)>> parse,
      Descriptions extra = {}
  ) {
    return {
        .key = key,
        .read =
            [=](Section& s, T& target) {
              if (const toml::node* node = s.take(key)) {
                parse(*node, s.qualified(key), target);
              }
            },
        .describe =
            [shape = std::move(shape), extra = std::move(extra)](const T&, const std::string& path, Descriptions& out) {
              KeyDescription entry = shape;
              entry.path = path;
              out.push_back(std::move(entry));
              for (KeyDescription child : extra) {
                child.path = std::format("{}.{}", path, child.path);
                out.push_back(std::move(child));
              }
            },
    };
  }

  // A nested table. `after` runs once the table is read, when the key is present, for checks across its keys.
  template <typename T, typename C>
  Field<T> table(
      std::string_view key, C T::* member, const Fields<C>& fields,
      std::type_identity_t<std::function<void(const toml::node&, C&)>> after = nullptr
  ) {
    return {
        .key = key,
        .read =
            [=, &fields](Section& s, T& target) {
              s.sub(key, [&](Section& child) { readFields(child, fields, target.*member); });
              if (after) {
                if (const toml::node* node = s.node(key)) {
                  after(*node, target.*member);
                }
              }
            },
        .describe =
            [=, &fields](const T& defaults, const std::string& path, Descriptions& out) {
              KeyDescription entry("table");
              entry.path = path;
              out.push_back(std::move(entry));
              describeFields(fields, defaults.*member, path, out);
            },
    };
  }

  // An array of tables, one entry per rule. `accept` sees each read entry with the parent it lands in, whose array
  // holds the entries kept so far, and decides whether it is kept; it reports its own reasons.
  template <typename T, typename C>
  Field<T> rules(
      std::string_view key, std::vector<C> T::* member, const Fields<C>& fields,
      std::type_identity_t<
          std::function<bool(const toml::node& entry, const std::string& context, C&, const T& parent)>>
          accept
  ) {
    return {
        .key = key,
        .read =
            [=, &fields](Section& s, T& target) {
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
                const std::string context = std::format("{}[{}]", path, index++);
                const toml::table* entryTable = entry.as_table();
                if (entryTable == nullptr) {
                  s.error(entry, std::format("{} must be a table", context));
                  continue;
                }
                C rule;
                {
                  Section keys(*entryTable, context, s.diagnostics());
                  readFields(keys, fields, rule);
                }
                if (accept(entry, context, rule, target)) {
                  (target.*member).push_back(std::move(rule));
                }
              }
            },
        .describe =
            [&fields](const T&, const std::string& path, Descriptions& out) {
              KeyDescription entry("array_of_tables");
              entry.path = path;
              out.push_back(std::move(entry));
              describeFields(fields, C{}, path + "[]", out);
            },
    };
  }

  // Every declared key, with the built-in values as defaults.
  [[nodiscard]] Descriptions describeConfig();

} // namespace umbriel::registry
