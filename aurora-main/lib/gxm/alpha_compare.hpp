#pragma once
#include <array>
#include <stdexcept>
#include <string>

namespace aurora::gxm {
// GX currently emits literal 8-bit alpha references in its GLSL. Preserve its
// constant folding and comparison operators, but put surviving references in
// GXM's mapped default uniform ring so fades need no new offline programs.
inline bool parameterize_alpha_references(std::string& source, std::array<float, 4>& values) {
  size_t position = 0;
  unsigned count = 0;
  while ((position = source.find("alphaCompare ", position)) != std::string::npos) {
    size_t number = position + 13;
    const auto op = source.find(' ', number);
    if (op == std::string::npos) break;
    const auto operation = source.substr(number, op - number);
    if (operation != "<" && operation != "<=" && operation != ">" && operation != ">=" &&
        operation != "==" && operation != "!=") { position = number; continue; }
    number = op + 1;
    size_t end = number;
    unsigned value = 0;
    while (end < source.size() && source[end] >= '0' && source[end] <= '9') {
      value = value * 10 + unsigned(source[end++] - '0');
      if (value > 255) throw std::runtime_error("GX alpha reference exceeds one byte");
    }
    const bool floating=source.compare(end,2,".0")==0;
    if (end == number || end == source.size() || (!floating && source[end] != 'u')) { position = number; continue; }
    if (count == 2) throw std::runtime_error("GX alpha compare has more than two references");
    values[count] = static_cast<float>(value);
    std::string replacement = count++ ? "aur_alpha_refs.y" : "aur_alpha_refs.x";
    if(!floating) replacement="u32("+replacement+")";
    source.replace(number, end + (floating?2:1) - number, replacement);
    position = number + replacement.size();
  }
  if (!count) return false;
  const auto versionEnd = source.find('\n');
  if (!source.starts_with("#version ") || versionEnd == std::string::npos)
    throw std::runtime_error("GX fragment source has no GLSL version line");
  source.insert(versionEnd + 1, "uniform highp vec4 aur_alpha_refs;\n");
  return true;
}
}
