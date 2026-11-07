#ifndef SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_
#define SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_

// Included only within http_rest_v1_test.cpp's job_routes namespace.
// APP-001: registry field validation (A), owned path/uniqueness (B), and files/digests (C)
// are separate. These production-composition checks never inject launch inputs into core.
class RegistryFixture final {
 public:
  explicit RegistryFixture(std::string_view prefix = "issue134-registry-") {
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      root = std::filesystem::temp_directory_path() /
             (std::string(prefix) + std::to_string(next.fetch_add(1)));
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
    config.journal_path = (root / "journal").string();
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
  std::size_t startup_vectors = 0;
  std::size_t refusal_vectors = 0;

 private:
  static inline std::atomic_uint64_t next{0};
};

void SetRegistryPath(sitometron::daemon::StartupConfig& config, const std::filesystem::path& path) {
  config.applications_path = path.string();
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
  ++fixture.refusal_vectors;
  auto config = fixture.Config();
  int result = 0;
  SetRegistryPath(config, fixture.file);
  {
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
                                                 fixture.file.string()});
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

std::string RegistryBytes(std::string_view hex) {
  std::string result;
  for (std::size_t index = 0; index < hex.size(); index += 2)
    result.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return result;
}

int RegistryStarted(RegistryFixture& fixture, std::string_view name) {
  ++fixture.startup_vectors;
  auto config = fixture.Config();
  SetRegistryPath(config, fixture.file);
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
  sitometron::test::registry_vectors::Inventory vectors(
      std::string(SITOMETRON_TEST_SOURCE_DIR) +
          "/schemas/application-registry/v1/application-registry-vectors.json",
      sitometron::test::registry_vectors::Group::kLoader);
  std::size_t lexical_vectors = 0;
  for (const auto& vector : vectors.Rows()) {
    vectors.Consume(vector);
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
        const sitometron::registry::RegisteredApplication application{
            "native", "1", text, {}, std::string(RegistryFixture::k_abc)};
        const auto refusal =
            sitometron::registry::internal::ValidateRegistryLocations({application});
        result |=
            Check(refusal.has_value() != absolute, label, "owned stage B disagrees with host rule");
        ++lexical_vectors;
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
  const auto consumed = vectors.Finish();
  // Schema IDs belong only to registry_schema_vectors. These independent startup companions
  // prove field/entry diagnostics at the executable boundary without consuming those IDs twice.
  for (const std::size_t index : {0U, 1U}) {
    auto invalid = fixture.document;
    if (index == 1) invalid["applications"].push_back(invalid["applications"][0]);
    invalid["applications"][index]["version"] = 1;
    RegistryFixture::Write(fixture.file, invalid.dump());
    result |= RegistryRefused(fixture, name, "version", index);
  }
  RegistryFixture::Write(fixture.file, R"({"schema":"wrong","applications":[]})");
  result |= RegistryRefused(fixture, name, "schema");
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
  std::string unicode_path = "/";
  for (unsigned index = 0; index < 4095; ++index) unicode_path.append("😀");
  for (const auto& path : {std::string("/"), "/" + std::string(4095, 'a'), unicode_path}) {
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
  std::cout << name << ": loader-only IDs executed=" << consumed
            << " native lexical vectors=" << lexical_vectors
            << " startup vectors=" << fixture.startup_vectors
            << " refusal vectors=" << fixture.refusal_vectors << '\n';
  return result;
}

int RegistryBundleDigestVerified() {
  constexpr std::string_view name = "registry_bundle_digest_verified";
  RegistryFixture fixture;
  int result = RegistryStarted(fixture, name);
  // CLI paths share the Journal's native narrow encoding, not the JSON string encoding.
  const auto directory = fixture.root / std::filesystem::path(u8"registry-é");
  bool representable = false;
  try {
    representable = std::filesystem::path(directory.string()) == directory;
  } catch (const std::filesystem::filesystem_error& error) {
    std::cout << name << ": non-ASCII registry PATH conversion failed: " << error.code().value()
              << ": " << error.code().message() << '\n';
  }
  if (representable) {
    std::filesystem::create_directory(directory);
    const auto original = fixture.file;
    fixture.file = directory / "applications.json";
    fixture.Save();
    result |= RegistryStarted(fixture, name);
    fixture.file = original;
    std::cout << name << ": non-ASCII native registry PATH passed\n";
  } else {
    std::cout << name << ": native code page cannot express the non-ASCII registry PATH; "
              << "UTF-8 executable coverage still runs\n";
  }
  const auto unicode_path = fixture.root / std::filesystem::path(u8"bundle-あ-😀");
  std::filesystem::copy_file(fixture.bundle, unicode_path);
  fixture.document["applications"][0]["executable"] = RegistryFixture::Text(unicode_path);
  fixture.Save();
  result |= RegistryStarted(fixture, name);
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
  std::cout << name << ": startup vectors=" << fixture.startup_vectors
            << " refusal vectors=" << fixture.refusal_vectors << '\n';
  return result;
}

int RegistryRequestErrors(std::uint16_t port, std::string_view name, std::size_t& executed) {
  // ADR-0008 Section 5: route, method, media, size, JSON, validation precede registration/ingress.
  const std::array vectors = {
      std::pair{Request("POST", "/v1/jobs", "{"), std::pair{400, "malformed_json"}},
      std::pair{Request("POST", "/v1/jobs", "{}"), std::pair{422, "validation_failed"}},
      std::pair{Request("POST", "/v1/no-route", "{", "text/plain"),
                std::pair{404, "route_not_found"}},
      std::pair{Request("PUT", "/v1/jobs", "{", "text/plain"),
                std::pair{405, "method_not_allowed"}},
      std::pair{Request("POST", "/v1/jobs", "{", "text/plain"),
                std::pair{415, "unsupported_media_type"}},
      std::pair{Request("POST", "/v1/jobs", std::string(65537, 'x')),
                std::pair{413, "payload_too_large"}}};
  int result = 0;
  for (const auto& [request, expected] : vectors) {
    ++executed;
    result |=
        RestError(SendToPort(port, request), name, expected.first, "request", expected.second);
  }
  return result;
}

int RegistryAbsentKeepsEmptySet() {
  constexpr std::string_view name = "registry_absent_keeps_empty_set";
  int result = 0;
  std::size_t configurations = 0;
  std::size_t requests = 0;
  for (const bool named : {false, true}) {
    RegistryFixture fixture;
    fixture.document["applications"] = Json::array();
    fixture.Save();
    auto config = fixture.Config();
    if (named) SetRegistryPath(config, fixture.file);
    sitometron::daemon::Phase1Composition daemon(config);
    std::string error;
    if (!daemon.Start(error)) return Check(false, name, "empty registry refused: " + error);
    ++configurations;
    ++requests;
    // ADR-0015 Section 1 and ADR-0008 Section 5; matches rest_v1_sitometrond_empty_registry.
    result |= RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody())),
                        name, 422, "job", "unknown_application");
    result |= RegistryRequestErrors(daemon.listen_port(), name, requests);
    result |= Check(daemon.backend().ListJobs().empty() && daemon.identity_reads() == 0 &&
                        daemon.clock_reads() == 0 && daemon.writer_turn_count() == 0,
                    name, "empty-registry request reached identity/clock/ingress");
    if (!named && result == 0)
      std::cout << name << ": no-option startup and rejection controls passed\n";
    daemon.Stop();
  }
  std::cout << name << ": configurations=" << configurations << " request vectors=" << requests
            << '\n';
  return result;
}

int RegisteredApplicationCreates() {
  constexpr std::string_view name = "rest_v1_registered_application_creates";
  RegistryFixture fixture;
  auto config = fixture.Config();
  SetRegistryPath(config, fixture.file);
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error)) return Check(false, name, "verified registry refused: " + error);
  std::size_t requests = 0;
  int result = RegistryRequestErrors(daemon.listen_port(), name, requests);
  // ADR-0009 Section 9: valid-length unregistered IDs are unknown regardless of their alphabet.
  // Escaped U+0000 is the existing rest_v1_json_rejects_raw_nul/escaped-control answer.
  for (const auto& id : {std::string("x"), std::string(128, 'z'), std::string("UPPER"),
                         std::string("a/b"), std::string("é"), std::string(1, '\0')}) {
    ++requests;
    result |=
        RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody(id))),
                  name, 422, "job", "unknown_application");
  }
  for (const auto& id : {std::string{}, std::string(129, 'a')}) {
    ++requests;
    result |=
        RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody(id))),
                  name, 422, "request", "validation_failed");
  }
  ++requests;
  const auto created =
      SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("demo-worker")));
  result |= ExpectStatus(created, name, 202);  // ADR-0008 Section 4, create commits before 202.
  if (created.status == 202) {
    const auto job = Json::parse(created.body);
    const auto id = job.at("job_id").get<std::string>();
    result |= Check(created.headers.at("location") == JobPath(id) &&
                        (job.at("state") == "admitted" || job.at("state") == "preparing") &&
                        !job.at("terminal").get<bool>() && job.at("outcome").is_null(),
                    name, "create did not return its committed nonterminal resource and Location");
    result |= Check(daemon.WaitForSupervisorIdleForTest(), name,
                    "production supervisor did not reach the runner placeholder wait");
    ++requests;
    result |=
        ExpectStatus(SendToPort(daemon.listen_port(), Request("GET", JobPath(id))), name, 200);
    result |= Check(
        daemon.backend().ListJobs().size() == 1 && daemon.identity_reads() == 3 &&
            daemon.clock_reads() == 3 && daemon.writer_turn_count() == 3 &&
            daemon.backend().GetJob(id)->state == "preparing",
        name, "production did not commit creation, resources and launch intent before waiting");
    // ADR-0008 Section 4: cancel commits before 202. With intent recorded, presence is
    // unknown and the unchanged reducer enters stopping, not pre-intent finalizing.
    ++requests;
    result |= ExpectStatus(SendToPort(daemon.listen_port(), Request("POST", JobPath(id, true))),
                           name, 202);
    ++requests;
    const auto cancelled = SendToPort(daemon.listen_port(), Request("GET", JobPath(id)));
    result |= ExpectStatus(cancelled, name, 200);  // ADR-0008 Section 4 read endpoint.
    if (cancelled.status == 200) {
      const auto after = Json::parse(cancelled.body);
      result |=
          Check(after.at("state") == "stopping" && !after.at("terminal").get<bool>() &&
                    after.at("outcome").is_null(),
                name, "post-intent cancel did not leave production waiting for process facts");
    }
  }
  daemon.Stop();
  std::cout << name << ": request vectors=" << requests << '\n';
  return result;
}

// Test observation only; no launch identity is injected into production composition.
bool RegistrySupervisorDrained(sitometron::daemon::Phase1Composition& daemon) {
  return daemon.WaitForSupervisorIdleForTest();
}

int RegistryLaunchIdentityFromEntry() {
  constexpr std::string_view name = "registry_launch_identity_from_entry";
  RegistryFixture fixture("issue133-registry-");
  const auto second_bundle = fixture.root / "second-bundle";
  RegistryFixture::Write(second_bundle, "");
  std::string maximum_version;
  for (unsigned i = 0; i < 128; ++i) maximum_version.append("\xf0\x9f\x98\x80");
  fixture.document["applications"][0]["version"] = "v\\\"\\\\-あ";
  fixture.document["applications"].push_back(
      Json{{"application_id", "second-worker"},
           {"version", maximum_version},
           {"executable", RegistryFixture::Text(second_bundle)},
           {"bundle_sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}});
  auto minimum = fixture.document["applications"][0];
  minimum["application_id"] = "minimum-worker";
  minimum["version"] = "1";
  fixture.document["applications"].push_back(minimum);
  fixture.Save();
  auto config = fixture.Config();
  SetRegistryPath(config, fixture.file);
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error)) return Check(false, name, "verified registry refused: " + error);
  std::map<std::string, Json> expected;
  int result = 0;
  // Interleave entries: reducing to an allowlist or remembering the last entry must fail.
  for (const std::size_t index : {0U, 1U, 2U, 0U}) {
    const auto& entry = fixture.document["applications"][index];
    const auto response = SendToPort(
        daemon.listen_port(),
        Request("POST", "/v1/jobs", CreateBody(entry.at("application_id").get<std::string>())));
    result |= ExpectStatus(response, name, 202);
    if (response.status == 202) {
      const auto id = Json::parse(response.body).at("job_id").get<std::string>();
      expected[id] = Json{{"application_id", entry.at("application_id")},
                          {"version", entry.at("version")},
                          {"bundle_sha256", entry.at("bundle_sha256")}};
    }
  }
  const bool drained = RegistrySupervisorDrained(daemon);
  // Exercise the real composition Session producer without inventing a Worker fact.
  // Hold before allocation, commit cancel, then let ONLY production continuations run.
  std::string retained_job;
  const bool held = daemon.ArmSupervisorBarrierForTest("after_decision", "resources_committed");
  result |= Check(held, name, "could not hold pre-allocation composition decision");
  if (held) {
    const auto response =
        SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("minimum-worker")));
    result |= ExpectStatus(response, name, 202);
    if (response.status == 202) {
      retained_job = Json::parse(response.body).at("job_id").get<std::string>();
      result |=
          Check(daemon.WaitForSupervisorBarrierForTest(), name, "resource decision was not held");
      result |= ExpectStatus(
          SendToPort(daemon.listen_port(), Request("POST", JobPath(retained_job, true))), name,
          202);
    }
    result |= Check(daemon.ReleaseSupervisorBarrierForTest() && RegistrySupervisorDrained(daemon),
                    name, "composition producers did not drain after early cancel");
    const auto cancelled = daemon.backend().GetJob(retained_job);
    result |= Check(cancelled && cancelled->terminal && cancelled->outcome == "cancelled", name,
                    "real Session placeholder did not reach cancelled outcome and cleanup");
  }
  daemon.Stop();  // Never read Journal files concurrently with the writer.
  // A changed verified entry must not supply launch inputs to an unresolved replayed Job.
  fixture.document["applications"][0]["version"] = "changed-on-restart";
  fixture.Save();
  {
    sitometron::daemon::Phase1Composition restarted(config);
    const bool started = restarted.Start(error);
    result |= Check(started, name, "restart refused: " + error);
    if (started)
      result |= Check(
          !restarted.backend().GetReadiness().ready && restarted.identity_reads() == 0 &&
              restarted.clock_reads() == 0 && restarted.writer_turn_count() == 0,
          name,
          "unresolved replay generated identities or drove a Job with changed registry inputs");
    restarted.Stop();
  }
  std::map<std::string, Json> observed;
  const std::array<std::string_view, 5> retention_events{
      "session_retain_requested", "session_retained", "finalization_completed",
      "terminal_outcome_committed", "cleanup_status_recorded"};
  std::array<std::uint64_t, 5> retention_sequences{};
  for (const auto& file : std::filesystem::directory_iterator(config.journal_path)) {
    if (file.path().extension() != ".ndjson") continue;
    std::ifstream input(file.path(), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
      const auto record = Json::parse(line);
      if (!retained_job.empty() && record.at("job_id") == retained_job) {
        for (std::size_t i = 0; i < retention_events.size(); ++i) {
          if (record.at("event_type").get_ref<const std::string&>() != retention_events[i])
            continue;
          result |= Check(retention_sequences[i] == 0, name, "duplicate retention continuation");
          retention_sequences[i] = record.at("sequence").get<std::uint64_t>();
          if (i <= 1)
            result |= Check(record.at("payload").at("session_id") == retained_job, name,
                            "composition retainer changed the bound Session identity");
          if (i == 4)
            result |= Check(record.at("payload").at("status") == "completed", name,
                            "composition did not record completed cleanup");
        }
      }
      if (record.at("event_type") != "worker_launch_intent") continue;
      const auto id = record.at("job_id").get<std::string>();
      result |= Check(observed.emplace(id, record.at("payload").at("application")).second, name,
                      "duplicate launch intent for one Job");
    }
  }
  result |= Check(expected.size() == 4 && observed.size() == expected.size(), name,
                  "expected 4 committed worker_launch_intent records from verified entries; got " +
                      std::to_string(observed.size()));
  for (const auto& [job, application] : observed)
    result |= Check(expected.contains(job) && expected.at(job) == application, name,
                    "launch identity differs from requested verified registry entry");
  result |= Check(
      !retained_job.empty() && retention_sequences[0] != 0 &&
          std::adjacent_find(retention_sequences.begin(), retention_sequences.end(),
                             [](auto before, auto after) { return before >= after; }) ==
              retention_sequences.end(),
      name, "real asynchronous retention/finalization/terminal/cleanup sequence is incomplete");
  result |= Check(drained, name, "production supervisor has no deterministic drain observation");
  std::cout << name << ": verified Jobs=" << expected.size()
            << " launch identities=" << observed.size() << '\n';
  return result;
}

#endif  // SITOMETRON_TEST_REGISTRY_COMPOSITION_CHECKS_HPP_
