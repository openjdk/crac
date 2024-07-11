#ifndef SHARE_UTILITIES_METHODKIND_HPP
#define SHARE_UTILITIES_METHODKIND_HPP

#include "memory/allStatic.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

// Kinds of methods.
//
// According to InstanceKlass::find_local_method(), a class can have separate
// methods with the same name and signature for each of these kinds.
struct MethodKind : public AllStatic {
  enum class Enum: u1 {
    STATIC   = 0,
    INSTANCE = 1, // Non-static, non-overpass
    OVERPASS = 2,
  };

  static Enum of_method(const Method &m) {
    assert(!(m.is_static() && m.is_overpass()), "overpass cannot be static");
    return m.is_static() ? Enum::STATIC : (m.is_overpass() ? Enum::OVERPASS : Enum::INSTANCE);
  }

  static constexpr bool is_method_kind(u1 val) { return val <= static_cast<u1>(Enum::OVERPASS); }

  static constexpr Klass::StaticLookupMode as_static_lookup_mode(MethodKind::Enum kind) {
    return kind == Enum::STATIC ? Klass::StaticLookupMode::find : Klass::StaticLookupMode::skip;
  }
  static constexpr Klass::OverpassLookupMode as_overpass_lookup_mode(MethodKind::Enum kind) {
    return kind == Enum::OVERPASS ? Klass::OverpassLookupMode::find : Klass::OverpassLookupMode::skip;
  }

  static constexpr const char *name(MethodKind::Enum kind) {
    switch (kind) {
      case Enum::STATIC:   return "static";
      case Enum::OVERPASS: return "overpass";
      case Enum::INSTANCE: return "non-static non-overpass";
      default: ShouldNotReachHere(); return nullptr;
    }
  }
};

#endif // SHARE_UTILITIES_METHODKIND_HPP
