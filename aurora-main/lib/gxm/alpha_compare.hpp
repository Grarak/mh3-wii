#pragma once
#include <array>
#include <stdexcept>
#include <string>

namespace aurora::gxm {
// GX currently emits literal 8-bit alpha references in its GLSL. Preserve its
// constant folding and comparison operators, but put surviving references in
// GXM's mapped default uniform ring so fades need no new offline programs.
// `parameterized` says whether the source had references; false (with the
// reason in `error`) rejects a source whose references cannot be mapped.
inline bool try_parameterize_alpha_references(std::string& source, std::array<float, 4>& values, bool& parameterized, std::string& error) {
  size_t position = 0;
  unsigned count = 0;

  while ((position = source.find("alphaCompare ", position)) != std::string::npos) {
    size_t number = position + 13;
    const auto op = source.find(' ', number);

    if (op == std::string::npos)
      break;

    const auto operation = source.substr(number, op - number);

    if (operation != "<" && operation != "<=" && operation != ">" && operation != ">=" && operation != "==" && operation != "!=") {
      position = number;
      continue;
    }

    number = op + 1;
    size_t end = number;
    unsigned value = 0;

    while (end < source.size() && source[end] >= '0' && source[end] <= '9') {
      value = value * 10 + unsigned(source[end++] - '0');

      if (value > 255) {
        error = "GX alpha reference exceeds one byte";
        return false;
      }
    }

    const bool floating = source.compare(end, 2, ".0") == 0;

    if (end == number || end == source.size() || (!floating && source[end] != 'u')) {
      position = number;
      continue;
    }

    if (count == 2) {
      error = "GX alpha compare has more than two references";
      return false;
    }

    values[count] = static_cast<float>(value);
    std::string replacement = count++ ? "aur_alpha_refs.y" : "aur_alpha_refs.x";

    if (!floating)
      replacement = "u32(" + replacement + ")";

    source.replace(number, end + (floating ? 2 : 1) - number, replacement);
    position = number + replacement.size();
  }

  parameterized = count != 0;

  if (!count)
    return true;

  const auto versionEnd = source.find('\n');

  if (!source.starts_with("#version ") || versionEnd == std::string::npos) {
    error = "GX fragment source has no GLSL version line";
    return false;
  }

  source.insert(versionEnd + 1, "uniform highp vec4 aur_alpha_refs;\n");
  return true;
}

// Whether the source had references; a source that cannot be mapped stops.
inline bool parameterize_alpha_references(std::string& source, std::array<float, 4>& values) {
  bool parameterized = false;
  std::string error;

  if (!try_parameterize_alpha_references(source, values, parameterized, error))
    throw std::runtime_error(error);

  return parameterized;
}
} // namespace aurora::gxm
