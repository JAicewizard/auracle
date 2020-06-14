#include "pacman.hh"

#include <fnmatch.h>
#include <glob.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/types/span.h"

namespace std {

template <>
struct default_delete<glob_t> {
  void operator()(glob_t* globbuf) {
    globfree(globbuf);
    delete globbuf;
  }
};

}  // namespace std

namespace {

bool IsSection(std::string_view s) {
  return s.size() > 2 && s.front() == '[' && s.back() == ']';
}

}  // namespace

namespace auracle {

Pacman::Pacman(alpm_handle_t* alpm)
    : alpm_(alpm), local_db_(alpm_get_localdb(alpm_)) {}

Pacman::~Pacman() { alpm_release(alpm_); }

struct ParseState {
  alpm_handle_t* alpm;
  std::string dbpath = "/var/lib/pacman";
  std::string rootdir = "/";

  std::string section;
  std::vector<std::string> repos;
};

bool ParseOneFile(const std::string& path, ParseState* state) {
  std::ifstream file(path);

  std::string buffer;
  while (std::getline(file, buffer)) {
    std::string_view line = absl::StripAsciiWhitespace(buffer);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    if (IsSection(line)) {
      state->section = line.substr(1, line.size() - 2);
      continue;
    }

    auto equals = line.find('=');
    if (equals == std::string::npos) {
      // There aren't any directives we care about which are valueless.
      continue;
    }

    auto key = absl::StripTrailingAsciiWhitespace(line.substr(0, equals));
    auto value = absl::StripLeadingAsciiWhitespace(line.substr(equals + 1));

    if (state->section == "options") {
      if (key == "DBPath") {
        state->dbpath = value;
      } else if (key == "RootDir") {
        state->rootdir = value;
      }
    } else {
      state->repos.emplace_back(state->section);
    }

    if (key == "Include") {
      auto globbuf = std::make_unique<glob_t>();

      if (glob(std::string(value).c_str(), GLOB_NOCHECK, nullptr,
               globbuf.get()) != 0) {
        return false;
      }

      for (const auto* p : absl::Span(globbuf->gl_pathv, globbuf->gl_pathc)) {
        if (!ParseOneFile(p, state)) {
          return false;
        }
      }
    }
  }

  file.close();

  return true;
}

// static
std::unique_ptr<Pacman> Pacman::NewFromConfig(const std::string& config_file) {
  ParseState state;

  if (!ParseOneFile(config_file, &state)) {
    return nullptr;
  }

  alpm_errno_t err;
  state.alpm = alpm_initialize("/", state.dbpath.c_str(), &err);
  if (state.alpm == nullptr) {
    return nullptr;
  }

  for (const auto& repo : state.repos) {
    alpm_register_syncdb(state.alpm, repo.c_str(),
                         static_cast<alpm_siglevel_t>(0));
  }

  return std::unique_ptr<Pacman>(new Pacman(state.alpm));
}

std::string Pacman::RepoForPackage(const std::string& package) const {
  for (auto i = alpm_get_syncdbs(alpm_); i != nullptr; i = i->next) {
    auto db = static_cast<alpm_db_t*>(i->data);
    auto pkgcache = alpm_db_get_pkgcache(db);

    if (alpm_find_satisfier(pkgcache, package.c_str()) != nullptr) {
      return alpm_db_get_name(db);
    }
  }

  return std::string();
}

bool Pacman::DependencyIsSatisfied(const std::string& package) const {
  auto* cache = alpm_db_get_pkgcache(local_db_);
  return alpm_find_satisfier(cache, package.c_str()) != nullptr;
}

std::optional<Pacman::Package> Pacman::GetLocalPackage(
    const std::string& name) const {
  auto* pkg = alpm_db_get_pkg(local_db_, name.c_str());
  if (pkg == nullptr) {
    return std::nullopt;
  }

  return Package{alpm_pkg_get_name(pkg), alpm_pkg_get_version(pkg)};
}

std::vector<Pacman::Package> Pacman::LocalPackages() const {
  std::vector<Package> packages;

  for (auto i = alpm_db_get_pkgcache(local_db_); i != nullptr; i = i->next) {
    const auto pkg = static_cast<alpm_pkg_t*>(i->data);

    packages.emplace_back(alpm_pkg_get_name(pkg), alpm_pkg_get_version(pkg));
  }

  return packages;
}

// static
int Pacman::Vercmp(const std::string& a, const std::string& b) {
  return alpm_pkg_vercmp(a.c_str(), b.c_str());
}

}  // namespace auracle
