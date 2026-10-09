#include "cg_shader.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace aurora::gfx::cg {
namespace {
[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("Aurora GLSL to Cg: " + message);
}

struct Token {
  size_t begin, end;
  std::string text;
};

bool identifier(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");

  if (first == value.npos)
    return {};

  return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}

std::vector<Token> lex(std::string_view source) {
  std::vector<Token> tokens;

  for (size_t at = 0; at < source.size();) {
    if (std::isspace(static_cast<unsigned char>(source[at]))) {
      ++at;
      continue;
    }

    if (source[at] == '#') {
      const auto end = source.find('\n', at);
      at = end == source.npos ? source.size() : end + 1;
      continue;
    }

    if (source.substr(at, 2) == "//") {
      const auto end = source.find('\n', at);
      at = end == source.npos ? source.size() : end + 1;
      continue;
    }

    if (source.substr(at, 2) == "/*") {
      const auto end = source.find("*/", at + 2);

      if (end == source.npos)
        fail("unterminated comment");

      at = end + 2;
      continue;
    }

    const auto begin = at++;

    if (std::isdigit(static_cast<unsigned char>(source[begin]))) {
      while (at < source.size() && (identifier(source[at]) || source[at] == '.' || ((source[at] == '+' || source[at] == '-') && (source[at - 1] == 'e' || source[at - 1] == 'E'))))
        ++at;
    } else if (identifier(source[begin]))
      while (at < source.size() && identifier(source[at]))
        ++at;
    tokens.push_back({begin, at, std::string(source.substr(begin, at - begin))});
  }

  return tokens;
}

size_t close(const std::vector<Token>& t, size_t first) {
  const auto opening = t.at(first).text;
  const auto ending = opening == "(" ? ")" : opening == "[" ? "]" : opening == "{" ? "}" : "";

  if (!*ending)
    fail("expected opening delimiter");

  std::vector<std::string> stack;

  for (size_t i = first; i < t.size(); ++i) {
    const auto& word = t[i].text;

    if (word == "(" || word == "[" || word == "{")
      stack.push_back(word);
    else if (word == ")" || word == "]" || word == "}") {
      if (stack.empty() || (word == ")" ? stack.back() != "(" : word == "]" ? stack.back() != "[" : stack.back() != "{"))
        fail("unbalanced delimiters");

      stack.pop_back();

      if (stack.empty())
        return i;
    }
  }

  fail("unterminated delimiter");
}

using Transform = std::function<std::string(const std::vector<std::string>&)>;
void calls(std::string& source, std::string_view name, const Transform& transform) {
  // Rewrite innermost calls first. Token boundaries exclude comments and names
  // such as textureSize from a texture replacement; brackets protect commas.
  for (;;) {
    const auto t = lex(source);
    size_t first = t.size();

    for (size_t i = 0; i + 1 < t.size(); ++i)
      if (t[i].text == name && t[i + 1].text == "(")
        first = i;
    if (first == t.size())
      return;

    const auto last = close(t, first + 1);
    size_t begin = t[first + 1].end;
    std::vector<std::string> args;

    for (size_t i = first + 2; i < last; ++i) {
      if (t[i].text == "(" || t[i].text == "[" || t[i].text == "{") {
        i = close(t, i);
        continue;
      }

      if (t[i].text == ",") {
        args.push_back(trim(std::string_view(source).substr(begin, t[i].begin - begin)));
        begin = t[i].end;
      }
    }

    if (begin < t[last].begin)
      args.push_back(trim(std::string_view(source).substr(begin, t[last].begin - begin)));

    const auto replacement = transform(args);
    source.replace(t[first].begin, t[last].end - t[first].begin, replacement);
  }
}

void arity(const std::vector<std::string>& args, size_t count, const char* name) {
  if (args.size() != count)
    fail(std::string(name) + " has unsupported argument count");
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
  for (size_t at = 0; (at = s.find(from, at)) != s.npos; at += to.size())
    s.replace(at, from.size(), to);
}

void identifiers(std::string& s, const std::map<std::string, std::string>& mapping) {
  // Include preprocessor aliases, which lex deliberately excludes.
  for (const auto& [from, to] : mapping)
    s = std::regex_replace(s, std::regex("\\b" + from + "\\b"), to);
}

const std::map<std::string, std::string> types{{"vec2", "float2"},   {"vec3", "float3"},     {"vec4", "float4"},   {"ivec2", "int2"},    {"ivec3", "int3"},     {"ivec4", "int4"},
                                               {"uvec2", "uint2"},   {"uvec3", "uint3"},     {"uvec4", "uint4"},   {"bvec2", "bool2"},   {"bvec3", "bool3"},    {"bvec4", "bool4"},
                                               {"mat4", "float4x4"}, {"mat3x4", "float4x3"}, {"mat2", "float2x2"}, {"mat3", "float3x3"}, {"usampler2D", "int"}, {"isampler2D", "sampler2D"}};
std::string type(const std::string& name) {
  const auto it = types.find(name);
  return it == types.end() ? name : it->second;
}

struct Field {
  std::string type, name;
  size_t count = 0;
};

struct Layout {
  size_t alignment, size;
  std::string body;
};

std::vector<Field> fields(std::string_view source) {
  const auto t = lex(source);
  std::vector<Field> result;

  for (size_t i = 0; i < t.size();) {
    if (i + 2 >= t.size())
      fail("truncated uniform field");

    Field f{t[i].text, t[i + 1].text};
    i += 2;

    if (t[i].text == "[") {
      if (i + 3 >= t.size() || t[i + 2].text != "]" || !std::all_of(t[i + 1].text.begin(), t[i + 1].text.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        fail("uniform array must have a literal positive extent");

      const auto count = std::stoull(t[i + 1].text);

      if (!count || count > 65536)
        fail("uniform array extent outside supported range");

      f.count = size_t(count);
      i += 3;
    }

    if (i >= t.size() || t[i++].text != ";")
      fail("unsupported uniform field declaration");

    result.push_back(std::move(f));
  }

  return result;
}

size_t align(size_t at, size_t by) {
  return (at + by - 1) & ~(by - 1);
}

Layout layout(const std::vector<Field>& members, const std::map<std::string, Layout>& structs) {
  size_t at = 0, alignment = 16, pads = 0;
  std::string body;
  auto pad = [&](size_t bytes) {
    while (bytes) {
      body += "    float aur_padding_" + std::to_string(pads++) + ";\n";
      bytes -= 4;
    }
  };

  for (const auto& f : members) {
    Layout l{};

    if (f.type == "float" || f.type == "int" || f.type == "uint" || f.type == "bool")
      l = {4, 4, {}};
    else if (std::regex_match(f.type, std::regex("[iub]?vec[234]"))) {
      const size_t components = size_t(f.type.back() - '0');
      l = {components == 2 ? 8u : 16u, components * 4, {}};
    } else if (f.type == "mat4" || f.type == "mat3x4")
      l = {16, f.type == "mat4" ? 64u : 48u, {}};
    else if (const auto it = structs.find(f.type); it != structs.end())
      l = it->second;
    else
      fail("unsupported std140 type " + f.type);

    if (f.name.starts_with("aur_padding_"))
      fail("reserved uniform padding name");

    if (f.count) {
      // Packed Cg arrays have no independent GLSL array-stride qualifier.
      // Full vec4/matrix/explicitly padded struct elements preserve the ABI.
      if (l.size != align(l.size, 16))
        fail("unsupported std140 array stride for " + f.type);

      l.alignment = std::max(l.alignment, size_t(16));
      l.size *= f.count;
    }

    const size_t next = align(at, l.alignment);
    pad(next - at);
    at = next;
    body += "    " + f.type + " " + f.name + (f.count ? "[" + std::to_string(f.count) + "]" : "") + ";\n";
    at += l.size;
    alignment = std::max(alignment, l.alignment);
  }

  const auto size = align(at, alignment);
  pad(size - at);
  return {alignment, size, body};
}

void uniforms(std::string& source, std::set<std::string>& matrices) {
  const auto t = lex(source);
  std::map<std::string, Layout> layouts;
  std::vector<std::tuple<size_t, size_t, std::string>> edits;

  for (size_t i = 0; i + 2 < t.size(); ++i) {
    if (t[i].text != "struct" || t[i + 2].text != "{")
      continue;

    const auto end = close(t, i + 2);

    if (end + 1 >= t.size() || t[end + 1].text != ";")
      fail("unsupported struct declaration");

    const auto body = std::string_view(source).substr(t[i + 2].end, t[end].begin - t[i + 2].end);
    const auto fs = fields(body);
    const auto l = layout(fs, layouts);

    if (!layouts.emplace(t[i + 1].text, l).second)
      fail("duplicate struct");

    edits.emplace_back(t[i].begin, t[end + 1].end, "struct " + t[i + 1].text + " {\n" + l.body + "};");
    i = end + 1;
  }

  unsigned blocks = 0;

  for (size_t i = 0; i + 7 < t.size(); ++i) {
    if (t[i].text != "layout" || t[i + 1].text != "(" || t[i + 2].text != "std140")
      continue;

    if (++blocks > 1)
      fail("multiple uniform blocks need explicit buffer bindings");

    if (t[i + 3].text != ")" || t[i + 4].text != "uniform" || t[i + 6].text != "{")
      fail("unsupported std140 block qualifiers");

    const auto end = close(t, i + 6);

    if (end + 2 >= t.size() || t[end + 2].text != ";")
      fail("uniform block requires a named instance");

    const auto fs = fields(std::string_view(source).substr(t[i + 6].end, t[end].begin - t[i + 6].end));
    const auto l = layout(fs, layouts);
    const auto& name = t[i + 5].text;
    const auto& instance = t[end + 1].text;

    for (const auto& f : fs)
      if (f.type.starts_with("mat"))
        matrices.insert(instance + "." + f.name);
    edits.emplace_back(t[i].begin, t[end + 2].end, "struct " + name + " {\n" + l.body + "};\n" + name + " " + instance + " : BUFFER[0];");
    i = end + 2;
  }

  std::sort(edits.rbegin(), edits.rend());

  for (const auto& [first, last, text] : edits)
    source.replace(first, last - first, text);
}

struct Interface {
  std::string type, name;
  unsigned location;
};

struct Interfaces {
  std::vector<Interface> attributes, varyings;
  std::string output;
};

Interfaces interfaces(std::string& source, Stage stage) {
  const auto t = lex(source);
  Interfaces result;
  int braces = 0, parentheses = 0;
  std::vector<std::tuple<size_t, size_t, std::string>> edits;
  std::set<unsigned> locations;
  std::set<std::string> names;

  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i].text == "{")
      ++braces;

    if (t[i].text == "}")
      --braces;

    if (t[i].text == "(")
      ++parentheses;

    if (t[i].text == ")")
      --parentheses;

    if (braces || parentheses)
      continue;

    size_t declaration = i;
    unsigned location = UINT32_MAX;

    if (t[i].text == "layout") {
      if (i + 6 >= t.size() || t[i + 1].text != "(" || t[i + 2].text != "location" || t[i + 3].text != "=" || t[i + 5].text != ")" ||
          !std::all_of(t[i + 4].text.begin(), t[i + 4].text.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        fail("unsupported interface layout");
      const auto value = std::stoull(t[i + 4].text);

      if (value > 15)
        fail("interface location exceeds native range");

      location = unsigned(value);
      i += 6;
    }

    const auto direction = t[i].text;

    if (direction != "in" && direction != "out") {
      if (i != declaration || direction == "flat" || direction == "centroid" || direction == "noperspective")
        fail("unsupported interface qualifier");

      continue;
    }

    if (i + 3 >= t.size() || t[i + 3].text != ";")
      fail("unsupported interface declaration");

    const Interface item{t[i + 1].text, t[i + 2].text, location};

    if (item.type != "float" && !std::regex_match(item.type, std::regex("vec[234]")))
      fail("unsupported interpolant/attribute type");

    if (!names.insert(item.name).second)
      fail("duplicate interface name");

    if (stage == Stage::Vertex && direction == "in") {
      if (location == UINT32_MAX || !locations.insert(location).second)
        fail("vertex attributes require unique explicit locations");

      result.attributes.push_back(item);
    } else if (stage == Stage::Fragment && direction == "out") {
      if (!result.output.empty() || (location != UINT32_MAX && location != 0) || item.type != "vec4")
        fail("fragment requires one vec4 output at location zero");

      result.output = item.name;
    } else {
      if (location != UINT32_MAX)
        fail("explicit varying locations are not supported");

      result.varyings.push_back(item);
    }

    edits.emplace_back(t[declaration].begin, t[i + 3].end, "static " + item.type + " " + item.name + ";");
    i += 3;
  }

  if (braces || parentheses)
    fail("unbalanced shader source");

  if (stage == Stage::Fragment && result.output.empty())
    fail("missing fragment color output");

  if (result.varyings.size() > 10)
    fail("too many varying semantics");

  std::sort(edits.rbegin(), edits.rend());

  for (const auto& [first, last, text] : edits)
    source.replace(first, last - first, text);

  return result;
}

std::string attribute_semantic(const Interface& a, const std::string& family) {
  static const std::vector<std::string> dynamicNames{"native_position", "native_view_position", "native_normal", "native_color0", "native_color1"};
  static const std::vector<std::string> dynamicSemantics{"POSITION", "TEXCOORD0", "NORMAL", "COLOR0", "COLOR1"};
  static const std::vector<std::string> meshNames{"mesh_position", "mesh_normal", "mesh_color0", "mesh_color1"};
  static const std::vector<std::string> meshSemantics{"POSITION", "NORMAL", "COLOR0", "COLOR1"};

  if (family == "direct") {
    static const std::vector<std::string> names{"position_in", "uv_in", "color_in"}, semantics{"POSITION", "TEXCOORD0", "COLOR0"};
    static const std::vector<std::string> expected{"vec4", "vec2", "vec4"};

    if (a.location >= names.size() || a.name != names[a.location] || a.type != expected[a.location])
      fail("unsupported direct native attribute " + a.name);

    return semantics[a.location];
  }

  const bool mesh = family == "mesh";
  const auto& names = mesh ? meshNames : dynamicNames;
  const auto& semantics = mesh ? meshSemantics : dynamicSemantics;
  const auto firstTex = unsigned(names.size());

  if (mesh && a.location >= 12 && a.location < 15) {
    if (a.name != "mesh_matrix" + std::to_string(a.location - 12) || a.type != "vec4")
      fail("native matrix selector attribute does not match layout: " + a.name);

    return "BLENDINDICES" + std::to_string(a.location - 12);
  }

  const auto expected = a.location < firstTex ? names[a.location] : (mesh ? "mesh_tex" : "native_tex") + std::to_string(a.location - firstTex);

  if (a.location >= firstTex + 8 || a.name != expected)
    fail("unsupported/mixed native attribute " + a.name);

  if ((!mesh && a.type != ((a.location == 1 || a.location == 2 || a.location >= 5) ? "vec3" : "vec4")) || (mesh && a.type != "vec4"))
    fail("native attribute type does not match layout: " + a.name);

  return a.location < firstTex ? semantics[a.location] : "TEXCOORD" + std::to_string(a.location - firstTex + (mesh ? 0 : 1));
}

size_t opening(const std::vector<Token>& t, size_t end) {
  int depth = 0;
  const auto& last = t[end].text;
  const auto* first = last == ")" ? "(" : "[";

  for (size_t i = end + 1; i-- > 0;) {
    if (t[i].text == last)
      ++depth;

    if (t[i].text == first && !--depth)
      return i;
  }

  fail("unbalanced matrix operand");
}

size_t operand(const std::vector<Token>& t, size_t end) {
  size_t begin = end;

  if (t[end].text == ")" || t[end].text == "]") {
    begin = opening(t, end);

    if (begin && (t[end].text == "]" || (identifier(t[begin - 1].text[0]) && t[begin - 1].text != "return")))
      begin = operand(t, begin - 1);
  } else if (!identifier(t[end].text[0]))
    fail("unsupported matrix operand");

  if (begin >= 2 && t[begin - 1].text == ".")
    begin = operand(t, begin - 2);
  // Include unary signs only; a binary subtraction is a lower-precedence term.
  if (begin && (t[begin - 1].text == "-" || t[begin - 1].text == "+" || t[begin - 1].text == "!")) {
    if (begin == 1 || std::string("=(:,;{+-*/").find(t[begin - 2].text) != std::string::npos)
      --begin;
  }

  return begin;
}

void matrix_products(std::string& source, const std::set<std::string>& matrices) {
  for (;;) {
    const auto t = lex(source);
    bool changed = false;

    for (size_t i = 1; i + 3 < t.size(); ++i) {
      if (t[i].text != "*" || t[i + 2].text != "." || !matrices.contains(t[i + 1].text + "." + t[i + 3].text))
        continue;

      size_t last = i + 3;

      if (last + 1 < t.size() && t[last + 1].text == "[")
        last = close(t, last + 1);

      size_t first = operand(t, i - 1);

      while (first >= 2 && (t[first - 1].text == "*" || t[first - 1].text == "/" || t[first - 1].text == "%"))
        first = operand(t, first - 2);

      const auto lhs = trim(std::string_view(source).substr(t[first].begin, t[i].begin - t[first].begin));
      const auto rhs = std::string_view(source).substr(t[i + 1].begin, t[last].end - t[i + 1].begin);
      source.replace(t[first].begin, t[last].end - t[first].begin, "mul(" + lhs + ", " + std::string(rhs) + ")");
      changed = true;
      break;
    }

    if (!changed)
      return;
  }
}

std::string prefix(bool wordBuffers) {
  std::string result =
      "#pragma pack_matrix(column_major)\n"
      "typedef unsigned int uint;\ntypedef unsigned int2 uint2;\n"
      "typedef unsigned int3 uint3;\ntypedef unsigned int4 uint4;\n";
  for (const auto* base : {"int", "uint"})
    for (unsigned count = 1; count <= 4; ++count) {
      const auto ty = std::string(base) + (count == 1 ? "" : std::to_string(count));

      for (const auto* name : {"min", "max"}) {
        std::string expression;
        const char* op = name[1] == 'i' ? " < " : " > ";

        for (unsigned k = 0; k < count; ++k) {
          const auto component = count == 1 ? "" : std::string(".") + "xyzw"[k];

          if (k)
            expression += ", ";

          expression += "a" + component + op + "b" + component + " ? a" + component + " : b" + component;
        }

        if (count > 1)
          expression = ty + "(" + expression + ")";

        result += ty + " " + name + "(" + ty + " a, " + ty + " b) { return " + expression + "; }\n";
      }

      result += ty + " clamp(" + ty + " v, " + ty + " a, " + ty + " b) { return min(max(v, a), b); }\n";
    }
  result +=
      "static uint aur_vertex_id;\nstatic uint aur_instance_id;\n"
      "static float4 aur_position_out;\nstatic float4 aur_fragcoord;\nstatic float aur_depth_out;\n";
  if (wordBuffers)
    result +=
        "uint vwords[786432] : BUFFER[1];\nuint awords[2097152] : BUFFER[2];\n"
        "uint aur_word(int buffer_id, uint i) { return buffer_id == 0 ? vwords[i] : awords[i]; }\n";
  return result +
         "bool2 aur_less(float2 a, float2 b) { return a < b; }\n"
         "bool3 aur_equal(float3 a, float3 b) { return a == b; }\n"
         "bool3 aur_greater(float3 a, float3 b) { return a > b; }\n";
}
} // namespace

std::string convert(std::string_view input, Stage stage) {
  if (input.size() > (1u << 20))
    fail("shader exceeds supported source size");

  std::string source(input);
  source = std::regex_replace(source, std::regex("^precision[^;]*;[ \\t]*$", std::regex::multiline), "");
  source = std::regex_replace(source, std::regex("^#(?:version|line)[^\\n]*", std::regex::multiline), "");
  source = std::regex_replace(source, std::regex("\\b(?:highp|mediump|lowp)\\s+"), "");

  // Remove the generated texture word-buffer helper; native Cg uses BUFFERs.
  {
    const auto t = lex(source);

    for (size_t i = 1; i + 1 < t.size(); ++i)
      if (t[i].text == "aur_word" && t[i - 1].text == "uint" && t[i + 1].text == "(") {
        const auto argsEnd = close(t, i + 1);

        if (argsEnd + 1 >= t.size() || t[argsEnd + 1].text != "{")
          fail("unsupported word-buffer helper");

        const auto end = close(t, argsEnd + 1);
        source.erase(t[i - 1].begin, t[end].end - t[i - 1].begin);
        break;
      }
  }

  replace_all(source, "p.x, -p.y, 2.0 * p.z - p.w, p.w", "p.x, p.y, 2.0 * p.z - p.w, p.w");
  source = std::regex_replace(source, std::regex("vec4\\(position_in\\.x,\\s*-position_in\\.y,\\s*position_in\\.zw\\)"), "vec4(position_in.x, position_in.y, position_in.zw)");
  std::set<std::string> matrices;
  uniforms(source, matrices);
  const auto interface = interfaces(source, stage);

  if (!interface.attributes.empty()) {
    // The native emitter retains a dead vidx initializer from its fetch route.
    // Native attribute streams supply no vertex/instance ID semantic. Remove
    // only a proven unused local; reject a real dependency rather than reading
    // an unassigned static builtin in the generated Cg wrapper.
    const auto t = lex(source);
    std::vector<std::pair<size_t, size_t>> dead;

    for (size_t i = 0; i + 7 < t.size(); ++i) {
      if (t[i].text != "uint" || t[i + 2].text != "=" || t[i + 3].text != "uint" || t[i + 4].text != "(" || (t[i + 5].text != "gl_VertexID" && t[i + 5].text != "gl_InstanceID") ||
          t[i + 6].text != ")" || t[i + 7].text != ";")
        continue;
      if (std::count_if(t.begin(), t.end(), [&](const Token& token) { return token.text == t[i + 1].text; }) == 1)
        dead.emplace_back(t[i].begin, t[i + 7].end);
    }

    std::sort(dead.rbegin(), dead.rend());

    for (const auto& [first, last] : dead)
      source.erase(first, last - first);

    for (const auto& token : lex(source))
      if (token.text == "gl_VertexID" || token.text == "gl_InstanceID")
        fail("native attribute layout cannot consume builtin vertex/instance IDs");
  }

  std::set<std::string> integerSamplers, samplers;
  std::set<unsigned> units;
  const std::regex uniform("\\buniform\\s+(\\w+)\\s+(\\w+)\\s*;");
  std::string converted;
  size_t begin = 0;

  for (std::sregex_iterator it(source.begin(), source.end(), uniform), end; it != end; ++it) {
    const auto& m = *it;
    const auto ty = m[1].str(), name = m[2].str();
    std::string declaration;

    if (ty == "usampler2D" && (name == "vbuf" || name == "abuf"))
      declaration = "static const int " + name + " = " + (name == "vbuf" ? "0" : "1") + ";";
    else if (ty == "sampler2D" || ty == "isampler2D") {
      unsigned unit = 0;

      if (name == "tlut")
        unit = 1;
      else if (std::regex_match(name, std::regex("tex[0-7]")))
        unit = unsigned(name.back() - '0');
      else if (name != "src" && name != "native_texture")
        fail("unsupported sampler binding " + name);

      if (!units.insert(unit).second)
        fail("duplicate sampler unit");

      samplers.insert(name);

      if (ty == "isampler2D")
        integerSamplers.insert(name);

      declaration = "sampler2D " + name + " : TEXUNIT" + std::to_string(unit) + ";\nfloat2 aur_size_" + name + ";";
    } else if (ty == "vec4")
      declaration = "float4 " + name + ";";
    else
      fail("unsupported standalone uniform " + ty);

    converted.append(source, begin, size_t(m.position()) - begin);
    converted += declaration;
    begin = size_t(m.position() + m.length());
  }

  converted.append(source, begin, source.size() - begin);
  source = std::move(converted);
  calls(source, "textureSize", [&](const auto& a) {
    arity(a, 2, "textureSize");

    if (!samplers.contains(a[0]) || a[1] != "0")
      fail("textureSize requires a known sampler and level zero");

    return "int2(aur_size_" + a[0] + ")";
  });
  calls(source, "texelFetch", [&](const auto& a) {
    arity(a, 3, "texelFetch");

    if (!samplers.contains(a[0]) || a[2] != "0")
      fail("texelFetch requires a known sampler and level zero");

    auto value = "tex2Dlod<float4>(" + a[0] + ", float4((float2(" + a[1] + ") + 0.5) / aur_size_" + a[0] + ", 0.0, " + a[2] + "))";
    return integerSamplers.contains(a[0]) ? "int4(" + value + ")" : value;
  });
  source = std::regex_replace(source, std::regex("(^|\\n)const (\\w+ \\w+\\[\\d+\\]\\s*=\\s*)\\w+\\[\\d+\\]\\(([^;]*)\\);"), "$1static const $2{$3};");
  replace_all(source, "uintBitsToFloat(", "bit_cast<float>(");
  replace_all(source, "floatBitsToUint(", "bit_cast<uint>(");
  identifiers(source,
              {{"gl_VertexID", "aur_vertex_id"}, {"gl_InstanceID", "aur_instance_id"}, {"gl_Position", "aur_position_out"}, {"gl_FragCoord", "aur_fragcoord"}, {"gl_FragDepth", "aur_depth_out"}});
  const std::regex mainPattern("\\bvoid\\s+main\\s*\\(\\s*\\)");

  if (std::distance(std::sregex_iterator(source.begin(), source.end(), mainPattern), std::sregex_iterator()) != 1)
    fail("expected one void main() entry point");

  source = std::regex_replace(source, mainPattern, "void aur_main()");
  source = std::regex_replace(source, std::regex("(raw_fetch_u8_1\\([^;\\n]*?\\))\\s*/\\s*3u"), "uint(float($1) / 3.0)");
  source = std::regex_replace(source, std::regex("(in_texmtxidx\\d+)\\s*/\\s*3u"), "uint(float($1) / 3.0)");
  source = std::regex_replace(source, std::regex("\\blerp\\b(?!\\s*\\()"), "aur_lerp_value");
  calls(source, "textureProj", [&](const auto& a) {
    arity(a, 2, "textureProj");

    if (!samplers.contains(a[0]))
      fail("unknown projected sampler");

    return "tex2Dproj(" + a[0] + ", " + a[1] + ")";
  });
  calls(source, "texture", [&](const auto& a) {
    if ((a.size() != 2 && a.size() != 3) || !samplers.contains(a[0]))
      fail("unsupported texture call");

    return a.size() == 3 ? "tex2Dbias(" + a[0] + ", float4(" + a[1] + ", 0.0, " + a[2] + "))" : "tex2D(" + a[0] + ", " + a[1] + ")";
  });

  for (const auto& [from, to] : std::map<std::string, std::string>{{"mix", "lerp"},
                                                                   {"fract", "frac"},
                                                                   {"mod", "fmod"},
                                                                   {"inversesqrt", "rsqrt"},
                                                                   {"dFdx", "ddx"},
                                                                   {"dFdy", "ddy"},
                                                                   {"lessThan", "aur_less"},
                                                                   {"equal", "aur_equal"},
                                                                   {"greaterThan", "aur_greater"}})
    source = std::regex_replace(source, std::regex("\\b" + from + "\\s*\\("), to + "(");
  identifiers(source, types);
  matrix_products(source, matrices);

  for (const auto& token : lex(source))
    if (token.text == "layout" || token.text == "uniform" || token.text.starts_with("gl_"))
      fail("unsupported GLSL construct " + token.text);
  std::vector<std::string> params, body;

  if (stage == Stage::Vertex) {
    if (interface.attributes.empty()) {
      params = {"float vertex_id : TEXCOORD14", "float instance_id : TEXCOORD15"};
      body = {"aur_vertex_id = uint(vertex_id);", "aur_instance_id = uint(instance_id);"};
    } else {
      const auto& first = interface.attributes.front().name;
      const auto family = first.starts_with("mesh_") ? "mesh" : first.starts_with("native_") ? "dynamic" : "direct";

      for (const auto& a : interface.attributes) {
        params.push_back(type(a.type) + " a_" + a.name + " : " + attribute_semantic(a, family));
        body.push_back(a.name + " = a_" + a.name + ";");
      }
    }

    params.push_back("out float4 position : POSITION");
    body.push_back("aur_main();");
    body.push_back("position = aur_position_out;");

    for (size_t i = 0; i < interface.varyings.size(); ++i) {
      const auto& v = interface.varyings[i];
      params.push_back("out " + type(v.type) + " o_" + v.name + " : TEXCOORD" + std::to_string(i));
      body.push_back("o_" + v.name + " = " + v.name + ";");
    }
  } else {
    for (size_t i = 0; i < interface.varyings.size(); ++i) {
      const auto& v = interface.varyings[i];
      params.push_back(type(v.type) + " i_" + v.name + " : TEXCOORD" + std::to_string(i));
      body.push_back(v.name + " = i_" + v.name + ";");
    }

    params.push_back("float4 position : WPOS");
    body.push_back("aur_fragcoord = position;");
    body.push_back("aur_main();");

    if (std::regex_search(source, std::regex("\\baur_depth_out\\s*="))) {
      params.push_back("out float depth : DEPTH");
      body.push_back("depth = aur_depth_out;");
    }

    body.push_back("return " + interface.output + ";");
  }

  const auto sourceTokens = lex(source);
  const bool wordBuffers = std::any_of(sourceTokens.begin(), sourceTokens.end(), [](const Token& token) { return token.text == "aur_word"; });
  auto result = prefix(wordBuffers) + source + "\n" + (stage == Stage::Vertex ? "void" : "float4") + " main(";

  for (size_t i = 0; i < params.size(); ++i)
    result += (i ? ", " : "") + params[i];

  result += stage == Stage::Vertex ? ")\n{\n" : ") : COLOR0\n{\n";

  for (const auto& statement : body)
    result += "    " + statement + "\n";

  return result + "}\n";
}

std::string scalar_fragment(std::string_view input) {
  std::string source(input);
  const auto t = lex(source);
  size_t main = t.size();

  for (size_t i = 1; i + 1 < t.size(); ++i)
    if (t[i].text == "main" && t[i + 1].text == "(") {
      if (main != t.size() || t[i - 1].text != "float4")
        fail("expected one float4 fragment entry point");

      main = i;
    }
  if (main == t.size())
    fail("missing float4 fragment entry point");

  const auto argsEnd = close(t, main + 1);
  size_t body = argsEnd + 1;

  while (body < t.size() && t[body].text != "{")
    ++body;

  if (body == t.size())
    fail("missing fragment entry body");

  const auto end = close(t, body);
  std::vector<std::tuple<size_t, size_t, std::string>> edits;
  edits.emplace_back(t[main - 1].begin, t[main - 1].end, "__regformat float");
  unsigned returns = 0;

  for (size_t i = body + 1; i < end; ++i)
    if (t[i].text == "return") {
      const auto first = ++i;

      while (i < end && t[i].text != ";") {
        if (t[i].text == "(" || t[i].text == "[" || t[i].text == "{")
          i = close(t, i);

        ++i;
      }

      if (i == end || first == i)
        fail("unsupported scalar fragment return");

      const auto expression = trim(std::string_view(source).substr(t[first].begin, t[i].begin - t[first].begin));
      edits.emplace_back(t[first].begin, t[i].begin, "(" + expression + ").r");
      ++returns;
    }
  if (!returns)
    fail("fragment entry has no return");

  std::sort(edits.rbegin(), edits.rend());

  for (const auto& [first, last, text] : edits)
    source.replace(first, last - first, text);

  return source;
}
} // namespace aurora::gfx::cg
