#include "parenting_rule.h"

#include <dcAtomicField.h>
#include <dcClass.h>
#include <dcField.h>
#include <dcPacker.h>
#include <dcParameter.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>

namespace Ardos {

namespace {

// Pull the default string value of an atomic field's Nth element. The DC
// compiler stores defaults in the parameter's packed form so we bounce
// through DCPacker to recover the original string. Returns false if the
// element either has no default or isn't a string.
bool ExtractElementDefaultString(DCAtomicField* atomicField, int elementIndex,
                                 std::string& out) {
  if (elementIndex < 0 || elementIndex >= atomicField->get_num_elements()) {
    return false;
  }
  if (!atomicField->has_element_default(elementIndex)) {
    return false;
  }

  DCParameter* param = atomicField->get_element(elementIndex);
  if (!param) {
    return false;
  }

  DCPacker packer;
  packer.set_unpack_data(atomicField->get_element_default(elementIndex));
  packer.begin_unpack(param);
  out = packer.unpack_string();
  bool ok = packer.end_unpack();
  return ok;
}

bool ParseUint32(std::string_view sv, uint32_t& out) {
  auto result = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return result.ec == std::errc{} && result.ptr == sv.data() + sv.size();
}

// Split on a single-char delimiter; trims nothing. Empty input → empty list.
std::vector<std::string_view> Split(std::string_view s, char delim) {
  std::vector<std::string_view> out;
  if (s.empty()) {
    return out;
  }
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delim) {
      out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

bool ParseCartesianRule(std::string_view ruleStr, ParentingRule& out) {
  // "<startZone>:<gridSize>:<radius>"
  auto parts = Split(ruleStr, ':');
  if (parts.size() != 3) {
    return false;
  }
  if (!ParseUint32(parts[0], out.cartStartZone) ||
      !ParseUint32(parts[1], out.cartGridSize) ||
      !ParseUint32(parts[2], out.cartRadius)) {
    return false;
  }
  if (out.cartGridSize == 0) {
    return false;
  }
  return true;
}

bool ParseAutoRule(std::string_view ruleStr, ParentingRule& out) {
  // "<originZone>:<z1>|<z2>|..."
  auto colon = ruleStr.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  if (!ParseUint32(ruleStr.substr(0, colon), out.autoOriginZone)) {
    return false;
  }

  auto zones = Split(ruleStr.substr(colon + 1), '|');
  for (auto z : zones) {
    if (z.empty()) {
      continue;
    }
    uint32_t parsed;
    if (!ParseUint32(z, parsed)) {
      return false;
    }
    out.autoExtraZones.push_back(parsed);
  }
  if (out.autoExtraZones.empty()) {
    return false;
  }
  return true;
}

}  // namespace

bool TryParseParentingRule(DCClass* klass, ParentingRule& out) {
  out = {};  // defaults to Stated/zeros.

  if (!klass) {
    return false;
  }

  DCField* field = klass->get_field_by_name("setParentingRules");
  if (!field) {
    // Not annotated -- entirely normal, most classes won't have this. Treat
    // as Stated and let the caller no-op.
    return false;
  }

  DCAtomicField* atomicField = field->as_atomic_field();
  if (!atomicField || atomicField->get_num_elements() < 2) {
    spdlog::get("ca")->warn(
        "Class '{}' has a setParentingRules field but it isn't a two-element "
        "atomic (expected `setParentingRules(string type, string Rule)`).",
        klass->get_name());
    return false;
  }

  std::string typeStr;
  std::string ruleStr;
  if (!ExtractElementDefaultString(atomicField, 0, typeStr) ||
      !ExtractElementDefaultString(atomicField, 1, ruleStr)) {
    spdlog::get("ca")->warn(
        "Class '{}' setParentingRules has no default values; both type and "
        "Rule must be string defaults on the field.",
        klass->get_name());
    return false;
  }

  if (typeStr == "Stated") {
    out.kind = ParentingRuleKind::Stated;
    return true;
  }
  if (typeStr == "Follow") {
    out.kind = ParentingRuleKind::Follow;
    return true;
  }
  if (typeStr == "Cartesian") {
    if (!ParseCartesianRule(ruleStr, out)) {
      spdlog::get("ca")->warn(
          "Class '{}' has Cartesian parenting rule with malformed Rule "
          "string: '{}' (expected '<startZone>:<gridSize>:<radius>')",
          klass->get_name(), ruleStr);
      return false;
    }
    out.kind = ParentingRuleKind::Cartesian;
    return true;
  }
  if (typeStr == "Auto") {
    if (!ParseAutoRule(ruleStr, out)) {
      spdlog::get("ca")->warn(
          "Class '{}' has Auto parenting rule with malformed Rule string: "
          "'{}' (expected '<originZone>:<z1>|<z2>|...')",
          klass->get_name(), ruleStr);
      return false;
    }
    out.kind = ParentingRuleKind::Auto;
    return true;
  }

  spdlog::get("ca")->warn("Class '{}' has unknown parenting rule type: '{}'",
                          klass->get_name(), typeStr);
  return false;
}

std::unordered_set<uint32_t> CartesianGridZones(const ParentingRule& rule,
                                                uint32_t avatarZone) {
  std::unordered_set<uint32_t> zones;
  if (rule.kind != ParentingRuleKind::Cartesian || rule.cartGridSize == 0) {
    return zones;
  }

  // Avatar zone is off-grid: refuse to open anything. Caller will close any
  // previously-open zones via the diff.
  if (avatarZone < rule.cartStartZone) {
    return zones;
  }

  uint64_t linear = static_cast<uint64_t>(avatarZone) - rule.cartStartZone;
  uint64_t totalCells =
      static_cast<uint64_t>(rule.cartGridSize) * rule.cartGridSize;
  if (linear >= totalCells) {
    return zones;
  }

  uint32_t row = static_cast<uint32_t>(linear / rule.cartGridSize);
  uint32_t col = static_cast<uint32_t>(linear % rule.cartGridSize);

  // radius=0 -> just the centre cell; radius=1 -> 3x3; radius=2 -> 5x5.
  const uint32_t reach = rule.cartRadius;

  uint32_t rowLo = (row >= reach) ? (row - reach) : 0;
  uint32_t rowHi = std::min<uint32_t>(rule.cartGridSize - 1, row + reach);
  uint32_t colLo = (col >= reach) ? (col - reach) : 0;
  uint32_t colHi = std::min<uint32_t>(rule.cartGridSize - 1, col + reach);

  for (uint32_t r = rowLo; r <= rowHi; ++r) {
    for (uint32_t c = colLo; c <= colHi; ++c) {
      zones.insert(rule.cartStartZone + r * rule.cartGridSize + c);
    }
  }
  return zones;
}

}  // namespace Ardos
