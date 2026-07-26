#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt::builder {

struct Profile {
  std::string_view name;
  std::string_view target_arch;
  std::string_view compiler_family;
  std::string_view buildtype;
  bool cross;
  bool tests;
  bool nvapi;
  bool nvngx;
  bool airconv_windows;
};

const std::vector<Profile> &Profiles();
const Profile *FindProfile(std::string_view name);
std::string JsonEscape(std::string_view value);
bool IsPathWithin(const std::filesystem::path &path,
                  const std::filesystem::path &root);

namespace testing {
std::map<std::string, std::string> ParseProperties(std::string_view contents);
std::map<std::string, std::string> ParseJsonStringObject(
    std::string_view contents);
std::optional<std::filesystem::path> DiscoverConfigPath(
    const std::filesystem::path &repo_root,
    const std::optional<std::filesystem::path> &requested = std::nullopt);
std::filesystem::path CcacheRoot(
    const std::filesystem::path &managed_root,
    std::string_view profile_namespace);
std::filesystem::path StagedInstallRoot(
    const std::filesystem::path &stage_dir,
    const std::filesystem::path &install_prefix);
void WriteFileAtomic(const std::filesystem::path &path, std::string_view contents);
void WithFileLock(const std::filesystem::path &path,
                  const std::function<void()> &operation);
std::vector<std::string>
AuditDx12Metal4Policy(const std::filesystem::path &repo_root);
bool AuditDiagnosticMayBeBaselined(std::string_view path,
                                   std::string_view check);
std::string AuditCacheKey(const std::filesystem::path &translation_unit,
                          const std::filesystem::path &repo_root,
                          const std::vector<std::string> &command,
                          std::string_view tool_identity,
                          std::string_view config_contents);
// Parses raw clang-tidy output lines exactly as the audit does and collapses
// them to one entry per unique source location, formatted with the number of
// translation units that reported it as a trailing "xN".
std::vector<std::string> DeduplicateAuditDiagnosticLines(
    const std::filesystem::path &repo_root,
    const std::vector<std::string> &lines);
} // namespace testing

class Application {
public:
  explicit Application(std::filesystem::path repo_root);
  int Run(std::span<const std::string> arguments);

private:
  std::filesystem::path repo_root_;
};

} // namespace dxmt::builder
