#include "entrypoint.hpp"

#include <stdexcept>

namespace sitometron::daemon {

// Intentional RED scaffold. The production main is not redirected until the GREEN implementation.
int RunSitometrond(const std::vector<std::string>& args, IdentityFactory factory) {
  (void)args;
  (void)factory;
  throw std::runtime_error("entrypoint RED scaffold");
}

}  // namespace sitometron::daemon
