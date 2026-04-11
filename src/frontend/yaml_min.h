/*
TGSpeechBox — Minimal YAML parser interface.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_YAML_MIN_H
#define TGSB_FRONTEND_YAML_MIN_H

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nvsp_frontend::yaml_min {

struct Node {
  enum class Type {
    Null,
    Scalar,
    Map,
    Seq,
  };

  Type type = Type::Null;
  // For scalars, we keep the raw text without quotes.
  std::string scalar;

  // std::map instead of std::unordered_map: std::map allows incomplete
  // value types (recursive Node) under GCC 11's libstdc++, which is the
  // default on Ubuntu 22.04 LTS.  Tiny perf diff vs unordered_map is
  // irrelevant — parsing happens once at startup.  Insertion-order
  // round-trip fidelity is handled separately by keyOrder below.
  std::map<std::string, Node> map;
  // Preserves insertion order of map keys for round-trip fidelity.
  std::vector<std::string> keyOrder;
  std::vector<Node> seq;

  // True if this node was parsed from inline flow syntax ({...} or [...]).
  // Used by the editor serializer to emit compact output.
  bool flowStyle = false;

  bool isScalar() const { return type == Type::Scalar; }
  bool isMap() const { return type == Type::Map; }
  bool isSeq() const { return type == Type::Seq; }

  // Typed scalar helpers. Return true on success.
  bool asBool(bool& out) const;
  bool asNumber(double& out) const;
  std::string asString(const std::string& fallback = "") const;

  const Node* get(std::string_view key) const;
};

// Parse a YAML file using a small, indentation-based subset.
// Supported:
// - maps (key: value)
// - sequences (- item)
// - nested blocks by indentation
// - scalar strings, bools, numbers
// - comments (# ...) on their own line or after a scalar

// Returns true on success. On failure, outError contains a message with a 1-based line number.
bool loadFile(const std::string& path, Node& outRoot, std::string& outError);

// Same as loadFile but parses from a string instead of a file path.
bool loadString(const std::string& yaml, Node& outRoot, std::string& outError);

} // namespace nvsp_frontend::yaml_min

#endif
