#ifndef SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_
#define SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_

// Included only within http_rest_v1_test.cpp's job_routes namespace.
// APP-001: registry field validation (A), owned path/uniqueness (B), and files/digests (C)
// are separate. These production-composition checks never inject launch inputs into core.
class RegistryFixture final {
 public:
  RegistryFixture() {
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      root = std::filesystem::temp_directory_path() /
             ("issue134-registry-" + std::to_string(next.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(root, error)) break;
      if (attempt == 999) throw std::runtime_error("registry fixture directory unavailable");
    }
    bundle = root / "bundle with spaces";
    file = root / "applications.json";
    Write(bundle, "abc");
    document = Json{{"schema", "application-registry.v1"},
                    {"applications", Json::array({Json{{"application_id", "demo-worker"},
                                                       {"version", "1"},
                                                       {"executable", Text(bundle)},
                                                       {"bundle_sha256", k_abc}}})}};
    Save();
  }
  ~RegistryFixture() {
    std::error_code ignored;
    std::filesystem::permissions(file, std::filesystem::perms::owner_all, ignored);
    std::filesystem::permissions(bundle, std::filesystem::perms::owner_all, ignored);
    std::filesystem::remove_all(root, ignored);
  }
  RegistryFixture(const RegistryFixture&) = delete;
  RegistryFixture& operator=(const RegistryFixture&) = delete;
  static std::string Text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
  }
  static void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("registry fixture write failed");
  }
  void Save() const { Write(file, document.dump()); }
  sitometron::daemon::StartupConfig Config() const {
    sitometron::daemon::StartupConfig config;
    config.journal_path = Text(root / "journal");
    config.http.listen_address = "127.0.0.1";
    config.http.listen_port = 0;
    return config;
  }
  static constexpr std::string_view k_abc =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  std::filesystem::path root;
  std::filesystem::path bundle;
  std::filesystem::path file;
  Json document;

 private:
  static inline std::atomic_uint64_t next{0};
};

template <class Config>
bool SetRegistryPath(Config& config, const std::filesystem::path& path) {
  // Intentional RED compatibility: no production success stub and no missing-member compile error.
  if constexpr (requires { config.applications_path = RegistryFixture::Text(path); }) {
    config.applications_path = RegistryFixture::Text(path);
    return true;
  }
  return false;
}

class RegistryOutput final {
 public:
  RegistryOutput() : old_out(std::cout.rdbuf(out.rdbuf())), old_err(std::cerr.rdbuf(err.rdbuf())) {}
  ~RegistryOutput() {
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
  }
  RegistryOutput(const RegistryOutput&) = delete;
  RegistryOutput& operator=(const RegistryOutput&) = delete;
  std::ostringstream out;
  std::ostringstream err;

 private:
  std::streambuf* old_out;
  std::streambuf* old_err;
};

int RegistryRefused(RegistryFixture& fixture, std::string_view name, std::string_view reason,
                    std::optional<std::size_t> entry = std::nullopt) {
  auto config = fixture.Config();
  int result = 0;
  if (SetRegistryPath(config, fixture.file)) {
    sitometron::daemon::Phase1Composition daemon(config);
    std::string error;
    const bool started = daemon.Start(error);
    result |= Check(!started && daemon.listen_port() == 0, name,
                    "invalid registry did not refuse before listening");
    if (started) {
      daemon.Stop();
      return result;  // Do not enter the executable's serve loop after an unexpected acceptance.
    }
    result |= Check(error.find(reason) != std::string::npos, name,
                    "composition refusal omitted reason: " + std::string(reason) + ": " + error);
    if (entry)
      result |=
          Check(error.find("applications[" + std::to_string(*entry) + "]") != std::string::npos,
                name, "composition refusal omitted entry index");
    daemon.Stop();
  }
  int status = 0;
  std::string output;
  std::string error;
  {
    RegistryOutput capture;
    status = sitometron::daemon::RunSitometrond({"--journal", config.journal_path, "--listen",
                                                 "127.0.0.1:0", "--applications",
                                                 RegistryFixture::Text(fixture.file)});
    output = capture.out.str();
    error = capture.err.str();
  }
  result |=
      Check(status == 1, name, "expected startup-refusal status 1, got " + std::to_string(status));
  result |=
      Check(output.find("listening") == std::string::npos, name, "refusal announced a listener");
  result |= Check(error.find(reason) != std::string::npos, name,
                  "standard error omitted reason: " + std::string(reason) + ": " + error);
  if (entry)
    result |= Check(error.find("applications[" + std::to_string(*entry) + "]") != std::string::npos,
                    name, "standard error omitted entry index");
  return result;
}

std::pair<std::string, std::optional<std::size_t>> RegistrySchemaFault(std::string_view id) {
  if (id.starts_with("root-not-object")) return {"object", std::nullopt};
  if (id.starts_with("entry-not-object")) return {"object", 0};
  if (id.starts_with("unknown-root")) return {"unknown", std::nullopt};
  if (id.starts_with("unknown-entry")) return {"unknown", 0};
  if (id.starts_with("schema-") || id == "missing-schema") return {"schema", std::nullopt};
  if (id.starts_with("applications-") || id == "missing-applications")
    return {"applications", std::nullopt};
  if (id.starts_with("id-") || id.starts_with("application_id-") || id == "missing-application_id")
    return {"application_id", 0};
  if (id.starts_with("digest-") || id.starts_with("bundle_sha256-") ||
      id == "missing-bundle_sha256")
    return {"bundle_sha256", 0};
  if (id.starts_with("version-") || id == "missing-version") return {"version", 0};
  if (id.starts_with("executable-") || id == "missing-executable") return {"executable", 0};
  if (id.starts_with("argument")) return {"argument", 0};
  throw std::runtime_error("schema refusal vector has no field/entry expectation");
}

std::string RegistryBytes(std::string_view hex) {
  std::string result;
  for (std::size_t index = 0; index < hex.size(); index += 2)
    result.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return result;
}

int RegistryStarted(RegistryFixture& fixture, std::string_view name) {
  auto config = fixture.Config();
  if (!SetRegistryPath(config, fixture.file))
    return Check(false, name, "production StartupConfig has no applications path");
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  int result = Check(daemon.Start(error), name, "valid registry refused: " + error);
  if (daemon.listen_port() != 0)
    result |=
        ExpectStatus(SendToPort(daemon.listen_port(), Request("GET", "/v1/health")), name, 200);
  else
    result |= Check(false, name, "valid registry opened no ephemeral listener");
  daemon.Stop();
  return result;
}

int RegistryStartupRefusesInvalid() {
  constexpr std::string_view name = "registry_startup_refuses_invalid";
  RegistryFixture fixture;
  std::filesystem::remove(fixture.file);
  int result = RegistryRefused(fixture, name, "registry");
  auto control = fixture.Config();
  if (!SetRegistryPath(control, fixture.file)) return result;
  const auto vectors =
      ReadJsonFile("schemas/application-registry/v1/application-registry-vectors.json");
  for (const auto& vector : vectors.at("loader_only_vectors")) {
    const auto label = std::string(name) + " [" + vector.at("id").get<std::string>() + "]";
    if (vector.contains("bytes_hex")) {
      RegistryFixture::Write(fixture.file,
                             RegistryBytes(vector.at("bytes_hex").get<std::string>()));
    } else {
      auto document = fixture.document;
      if (vector.at("fixture") == "duplicate_identifier")
        document["applications"].push_back(document["applications"][0]);
      else if (vector.at("fixture") == "relative_executable")
        document["applications"][0]["executable"] = "relative-bundle";
      else if (vector.at("fixture") == "native_path") {
        const auto text = vector.at("path").get<std::string>();
        const std::u8string utf8(text.begin(), text.end());
#ifdef _WIN32
        const bool absolute = vector.at("absolute_windows").get<bool>();
#else
        const bool absolute = vector.at("absolute_linux").get<bool>();
#endif
        result |= Check(std::filesystem::path(utf8).is_absolute() == absolute, label,
                        "host absolute-path premise changed");
        // UNC/device forms are lexical controls only: never contact a server/open a device.
        if (absolute) continue;
        document["applications"][0]["executable"] = text;
      } else
        return Check(false, label, "unknown loader-only fixture");
      RegistryFixture::Write(fixture.file, document.dump());
    }
    std::optional<std::size_t> entry;
    if (vector.contains("entry")) entry = vector.at("entry").get<std::size_t>();
    result |= RegistryRefused(fixture, label, vector.at("reason").get<std::string>(), entry);
  }
  for (const auto& vector : vectors.at("schema_vectors")) {
    if (vector.at("valid").get<bool>()) continue;
    RegistryFixture::Write(fixture.file, vector.at("instance").dump());
    const auto [reason, entry] = RegistrySchemaFault(vector.at("id").get<std::string>());
    result |= RegistryRefused(fixture,
                              std::string(name) + " [" + vector.at("id").get<std::string>() + "]",
                              reason, entry);
  }
  // Byte caps are C++-only constructions, not committed megabyte fixtures.
  const auto empty =
      Json{{"schema", "application-registry.v1"}, {"applications", Json::array()}}.dump();
  for (const std::size_t size : {1'048'575U, 1'048'576U, 1'048'577U}) {
    RegistryFixture::Write(fixture.file, empty + std::string(size - empty.size(), ' '));
    result |= size <= 1'048'576 ? RegistryStarted(fixture, name)
                                : RegistryRefused(fixture, name, "1 MiB");
  }
  auto many = fixture.document;
  many["applications"] = Json::array();
  auto maximal = fixture.document["applications"][0];
  maximal["version"] = std::string(128, 'v');
  maximal["executable"] = "/" + std::string(4095, 'a');
  maximal["arguments"] = std::vector<std::string>(64, std::string(1024, 'a'));
  for (unsigned index = 0; index < 20; ++index) {
    auto id = "app-" + std::to_string(index);
    id.append(128 - id.size(), 'a');
    maximal["application_id"] = id;
    many["applications"].push_back(maximal);
  }
  result |= Check(many.dump().size() > 1'048'576, name, "maximal-entry fixture did not exceed cap");
  RegistryFixture::Write(fixture.file, many.dump());
  result |= RegistryRefused(fixture, name, "1 MiB");
  // Stage A permits these lengths. Full loading must still apply B/C, without shortening them.
  for (const auto& path : {std::string("/"), "/" + std::string(4095, 'a')}) {
    auto document = fixture.document;
    document["applications"][0]["executable"] = path;
    RegistryFixture::Write(fixture.file, document.dump());
#ifdef _WIN32
    result |= RegistryRefused(fixture, name, "absolute", 0);
#else
    result |= RegistryRefused(fixture, name, "regular", 0);
#endif
  }
  std::filesystem::remove(fixture.file);
  std::filesystem::create_directory(fixture.file);
  result |= RegistryRefused(fixture, name, "registry");
  std::filesystem::remove(fixture.file);
  fixture.Save();
#ifdef _WIN32
  const auto locked = ::CreateFileW(fixture.file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  result |=
      Check(locked != INVALID_HANDLE_VALUE, name, "could not create unreadable registry control");
  if (locked != INVALID_HANDLE_VALUE) {
    result |= RegistryRefused(fixture, name, "read");
    (void)::CloseHandle(locked);
  }
#else
  std::filesystem::permissions(fixture.file, std::filesystem::perms::none);
  result |= RegistryRefused(fixture, name, "read");
  std::filesystem::permissions(fixture.file, std::filesystem::perms::owner_all);
#endif
  return result;
}

int RegistryBundleDigestVerified() {
  constexpr std::string_view name = "registry_bundle_digest_verified";
  RegistryFixture fixture;
  int result = RegistryStarted(fixture, name);
  auto config = fixture.Config();
  if (!SetRegistryPath(config, fixture.file)) return result;
  const auto link = fixture.root / "symbolic-link";
  std::error_code linked;
  std::filesystem::create_symlink(fixture.bundle, link, linked);
  result |= Check(
      !linked && std::filesystem::is_symlink(link) && std::filesystem::is_regular_file(link), name,
      "required file symbolic link creation failed: " + std::to_string(linked.value()) + " (" +
          linked.category().name() + "): " + linked.message());
  if (linked) return result;  // Required failure, never a Windows skip or a substitute hard link.
  std::cout << name << ": real file symbolic link created and followed\n";
  fixture.document["applications"][0]["executable"] = RegistryFixture::Text(link);
  fixture.Save();
  result |= RegistryStarted(fixture, name);
  RegistryFixture::Write(fixture.bundle, "abd");
  result |= RegistryRefused(fixture, name, "digest", 0);
  std::filesystem::remove(fixture.bundle);
  result |= RegistryRefused(fixture, name, "regular", 0);
  std::filesystem::create_directory(fixture.bundle);
  result |= RegistryRefused(fixture, name, "regular", 0);
  std::filesystem::remove(fixture.bundle);
  RegistryFixture::Write(fixture.bundle, "abc");
  fixture.document["applications"][0]["executable"] = RegistryFixture::Text(fixture.bundle);
  fixture.Save();
#ifndef _WIN32
  std::filesystem::permissions(fixture.bundle, std::filesystem::perms::none);
  result |= RegistryRefused(fixture, name, "read", 0);
  std::filesystem::permissions(fixture.bundle, std::filesystem::perms::owner_all);
#else
  // ADR-0015 permits only this unreadable-executable fixture to be skipped on Windows.
  std::cout << name << ": Windows skips the unreadable-executable permission fixture (ADR-0015)\n";
#endif
  for (const auto& bytes : {std::string{}, std::string("a\0b", 3), std::string(131'073, 'x')}) {
    RegistryFixture::Write(fixture.bundle, bytes);
    boost::hash2::sha2_256 hasher;
    hasher.update(bytes.data(), bytes.size());
    const auto digest = boost::hash2::to_string(hasher.result());
    if (bytes.empty())
      result |= Check(digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      name, "empty-file SHA-256 known answer changed");
    fixture.document["applications"][0]["bundle_sha256"] = digest;
    fixture.Save();
    result |= RegistryStarted(fixture, name);
  }
  fixture.document["applications"].push_back(fixture.document["applications"][0]);
  fixture.document["applications"][1]["application_id"] = "bad-second";
  fixture.document["applications"][1]["bundle_sha256"] = RegistryFixture::k_abc;
  fixture.Save();
  result |= RegistryRefused(fixture, name, "digest", 1);
  return result;
}

int RegistryRequestErrors(std::uint16_t port, std::string_view name) {
  // ADR-0008 Section 5: route, method, media, size, JSON, validation precede registration/ingress.
  int result = RestError(SendToPort(port, Request("POST", "/v1/jobs", "{")), name, 400, "request",
                         "malformed_json");
  result |= RestError(SendToPort(port, Request("POST", "/v1/jobs", "{}")), name, 422, "request",
                      "validation_failed");
  result |= RestError(SendToPort(port, Request("POST", "/v1/no-route", "{", "text/plain")), name,
                      404, "request", "route_not_found");
  result |= RestError(SendToPort(port, Request("PUT", "/v1/jobs", "{", "text/plain")), name, 405,
                      "request", "method_not_allowed");
  result |= RestError(SendToPort(port, Request("POST", "/v1/jobs", "{", "text/plain")), name, 415,
                      "request", "unsupported_media_type");
  result |= RestError(SendToPort(port, Request("POST", "/v1/jobs", std::string(65537, 'x'))), name,
                      413, "request", "payload_too_large");
  return result;
}

int RegistryAbsentKeepsEmptySet() {
  constexpr std::string_view name = "registry_absent_keeps_empty_set";
  int result = 0;
  for (const bool named : {false, true}) {
    RegistryFixture fixture;
    fixture.document["applications"] = Json::array();
    fixture.Save();
    auto config = fixture.Config();
    if (named && !SetRegistryPath(config, fixture.file)) {
      result |= Check(false, name, "explicit valid empty registry is not supported");
      continue;
    }
    sitometron::daemon::Phase1Composition daemon(config);
    std::string error;
    if (!daemon.Start(error)) return Check(false, name, "empty registry refused: " + error);
    // ADR-0015 Section 1 and ADR-0008 Section 5; matches rest_v1_sitometrond_empty_registry.
    result |= RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody())),
                        name, 422, "job", "unknown_application");
    result |= RegistryRequestErrors(daemon.listen_port(), name);
    result |= Check(daemon.backend().ListJobs().empty() && daemon.identity_reads() == 0 &&
                        daemon.clock_reads() == 0 && daemon.writer_turn_count() == 0,
                    name, "empty-registry request reached identity/clock/ingress");
    if (!named && result == 0)
      std::cout << name << ": no-option control already passes before registry implementation\n";
    daemon.Stop();
  }
  return result;
}

int RegisteredApplicationCreates() {
  constexpr std::string_view name = "rest_v1_registered_application_creates";
  RegistryFixture fixture;
  auto config = fixture.Config();
  if (!SetRegistryPath(config, fixture.file))
    return Check(false, name, "verified production registry cannot be configured");
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error)) return Check(false, name, "verified registry refused: " + error);
  int result = RegistryRequestErrors(daemon.listen_port(), name);
  // ADR-0009 Section 9: valid-length unregistered IDs are unknown regardless of their alphabet.
  // Escaped U+0000 is the existing rest_v1_json_rejects_raw_nul/escaped-control answer.
  for (const auto& id : {std::string("x"), std::string(128, 'z'), std::string("UPPER"),
                         std::string("a/b"), std::string("é"), std::string(1, '\0')})
    result |=
        RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody(id))),
                  name, 422, "job", "unknown_application");
  for (const auto& id : {std::string{}, std::string(129, 'a')})
    result |=
        RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody(id))),
                  name, 422, "request", "validation_failed");
  const auto created =
      SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("demo-worker")));
  result |= ExpectStatus(created, name, 202);  // ADR-0008 Section 4, create commits before 202.
  if (created.status == 202) {
    const auto job = Json::parse(created.body);
    const auto id = job.at("job_id").get<std::string>();
    result |=
        Check(created.headers.at("location") == JobPath(id) && job.at("state") == "admitted" &&
                  !job.at("terminal").get<bool>() && job.at("outcome").is_null(),
              name, "create did not return its committed admitted resource and Location");
    result |=
        ExpectStatus(SendToPort(daemon.listen_port(), Request("GET", JobPath(id))), name, 200);
    result |= Check(daemon.backend().ListJobs().size() == 1 && daemon.identity_reads() == 3 &&
                        daemon.clock_reads() == 1 && daemon.writer_turn_count() == 1,
                    name, "production create did not commit exactly one creation with real ports");
  }
  daemon.Stop();
  return result;
}

#endif  // SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_
