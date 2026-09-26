// ASQS - utility helpers: file IO, time, fs, string.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace asqs {

// ---- time ----
int64_t now_ms();          // epoch ms (UTC)
int64_t now_sec();
std::string iso8601_utc(int64_t ms);
// ASQS file timestamp per project spec: {sec}_{min}_{hour}_{day}_{month}_{year}, zero padded.
// ss_mm_HH_dd_MM_yyyy. Timezone: UTC unless local=true.
std::string asqs_timestamp(bool local, int64_t ms);

// ---- fs ----
bool ensure_dir(const std::string& path);              // mkdir -p style, recursive
bool file_exists(const std::string& path);
std::vector<std::string> list_dir(const std::string& path); // names, non-recursive
std::optional<std::string> read_file(const std::string& path);
bool write_file_atomic(const std::string& path, const std::string& data); // tmp+fsync+rename
bool rename_file(const std::string& from, const std::string& to);
bool delete_file(const std::string& path);
bool fsync_dir(const std::string& path);

// ---- string ----
std::string to_lower(const std::string& s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

} // namespace asqs
