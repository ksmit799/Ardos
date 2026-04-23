#ifndef ARDOS_PARENTING_RULE_H
#define ARDOS_PARENTING_RULE_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

class DCClass;

namespace Ardos {

// Parent classes can declare a `setParentingRules(string type, string rule)`
// field that the CA interprets to automatically manage interests on behalf of
// a client when their avatar is parented underneath, or when the client opens
// an interest whose parent carries the rule.
//
// Stated    — default. No automatic behaviour; the caller must open interests
//             themselves. Chosen when the field is absent or unrecognised.
// Follow    — a single interest tracks the avatar's current zone under this
//             parent. Opened on entry, re-targeted on zone change, closed on
//             exit.
// Cartesian — grid-world parent. Avatar zone identifies a cell; an NxN block
//             of cells centred on the avatar is kept open. Zone updates
//             diff-and-update the block.
// Auto      — when any interest opens under this parent in a specific origin
//             zone, a configured set of additional zones is merged into that
//             interest.
enum class ParentingRuleKind {
  Stated,
  Follow,
  Cartesian,
  Auto,
};

struct ParentingRule {
  ParentingRuleKind kind = ParentingRuleKind::Stated;

  // Cartesian rule parameters, parsed from "startZone:gridSize:radius".
  //   startZone: zoneId of cell (0, 0) in the grid.
  //   gridSize:  number of cells per row. Total cells = gridSize * gridSize.
  //   radius:    number of cells around the avatar cell to keep open.
  //              radius=0 is "just the current cell", 1 is 3x3, 2 is 5x5, etc.
  uint32_t cartStartZone = 0;
  uint32_t cartGridSize = 0;
  uint32_t cartRadius = 0;

  // Auto rule parameters, parsed from "originZone:z1|z2|...".
  uint32_t autoOriginZone = 0;
  std::vector<uint32_t> autoExtraZones;
};

// Read the `setParentingRules` field off a parent class and parse it. Returns
// true on successful parse into `out`. Unknown types, missing field, or bad
// parameters fall back to Stated and return false -- which is fine; the CA
// then just treats the parent as a normal non-managed parent.
bool TryParseParentingRule(DCClass* klass, ParentingRule& out);

// Compute the set of zones a Cartesian parent should have interest in, given
// the avatar's current zone under that parent. Off-grid avatar zones produce
// an empty set (nothing to open).
std::unordered_set<uint32_t> CartesianGridZones(const ParentingRule& rule,
                                                uint32_t avatarZone);

}  // namespace Ardos

#endif  // ARDOS_PARENTING_RULE_H
