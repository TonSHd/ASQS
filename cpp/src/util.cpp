#include "util.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace asqs {

int64_t now_ms() {
    // Real millisecond resolution (CLOCK_REALTIME). The previous
    // implementation was seconds * 1000, so the millisecond part was
    // always 000: log timestamps showed ".000Z", and any two events in
    // the same second shared the same now_ms() value (which made batch
    // filenames collide in the same second).
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
int64_t now_sec() { return ::time(nullptr); }

std::string iso8601_utc(int64_t ms) {
    time_t t = time_t(ms / 1000);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    // 128 bytes comfortably exceeds GCC's worst-case bound (78) for this
    // format, so -Wformat-truncation can prove the write always fits.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, int(ms % 1000));
    return buf;
}

std::string asqs_timestamp(bool local, int64_t ms) {
    time_t t = time_t(ms / 1000);
    struct tm tmv;
    if (local) localtime_r(&t, &tmv);
    else gmtime_r(&t, &tmv);
    // Same worst-case safety margin as iso8601_utc (see above).
    char buf[128];
    // Spec order: seconds, minutes, hours, day, month, year (zero-padded).
    std::snprintf(buf, sizeof(buf), "%02d_%02d_%02d_%02d_%02d_%04d",
                  tmv.tm_sec, tmv.tm_min, tmv.tm_hour, tmv.tm_mday,
                  tmv.tm_mon + 1, tmv.tm_year + 1900);
    return buf;
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && i > 0) ::mkdir(cur.c_str(), 0755);
    }
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name(e->d_name);
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
    return out;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += size_t(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

bool rename_file(const std::string& from, const std::string& to) {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool delete_file(const std::string& path) { return ::unlink(path.c_str()) == 0; }

bool fsync_dir(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    ::fsync(fd);
    ::close(fd);
    return true;
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

} // namespace asqs
