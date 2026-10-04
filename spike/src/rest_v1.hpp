#ifndef SITOMETRON_SPIKE_REST_V1_HPP_
#define SITOMETRON_SPIKE_REST_V1_HPP_

#include <map>
#include <string>

#include "http_server.hpp"
#include "job_driver.hpp"

namespace sitometron::spike {

// The deployment-registered Applications of this run: identifier to the shell command the skeleton
// launches for it. ADR-0008 Section 4: until the Application Registry exists, the registered set
// comes from the startup configuration.
using Applications = std::map<std::string, std::string>;

// A non-normative prototype of the External REST v1 Job surface (Accepted ADR-0008): create, read,
// list, health, and readiness. Cancel is not served yet. Handles every target under /v1.
[[nodiscard]] HttpResponse RouteV1(JobDriver& driver, const Applications& applications,
                                   const HttpRequest& request);

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_REST_V1_HPP_
