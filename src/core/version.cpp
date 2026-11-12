#include "core/version.hpp"

namespace rksec::version {

std::string full()
{
    std::string s = "rk3588-security ";
    s += semver();
    s += " (";
    s += git_describe();
    s += ", ";
    s += build_type();
    s += ", ";
    s += compiler();
    s += ")";
    return s;
}

}  // namespace rksec::version
