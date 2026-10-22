#pragma once

// A mapped file (read only) and a file sink for the Builder. POSIX.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "pyre/container.hpp"
#include "source.hpp"

namespace pyre::tool {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw SourceError("cannot open " + path + ": " + std::strerror(errno));
        struct stat st;
        if (::fstat(fd_, &st) != 0) throw SourceError("cannot stat " + path);
        size_ = std::size_t(st.st_size);
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) throw SourceError("cannot map " + path + ": " + std::strerror(errno));
            data_ = static_cast<const std::uint8_t*>(p);
        }
    }
    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const std::uint8_t> bytes() const { return {data_, size_}; }

private:
    int fd_ = -1;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

/// Writes to a file through a buffer; writeAt goes round it.
class FileSink : public pyre::Sink {
public:
    explicit FileSink(const std::string& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) throw SourceError("cannot create " + path + ": " + std::strerror(errno));
        buf_.reserve(kBuffer);
    }
    ~FileSink() override {
        if (fd_ >= 0) ::close(fd_);
    }
    bool append(const void* data, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (buf_.size() + n > kBuffer && !flush()) return false;
        if (n >= kBuffer) {
            if (!writeAll(p, n, pos_)) return false;
            pos_ += n;
            return true;
        }
        buf_.insert(buf_.end(), p, p + n);
        return true;
    }
    bool writeAt(std::uint64_t pos, const void* data, std::size_t n) override {
        if (!flush()) return false;
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::size_t done = 0;
        while (done < n) {
            const ssize_t w = ::pwrite(fd_, p + done, n - done, off_t(pos + done));
            if (w <= 0) return false;
            done += std::size_t(w);
        }
        return true;
    }
    /// Writes what is buffered and closes the file.
    bool close() {
        const bool ok = flush() && ::close(fd_) == 0;
        fd_ = -1;
        return ok;
    }

private:
    static constexpr std::size_t kBuffer = 8u << 20;
    bool flush() {
        if (buf_.empty()) return true;
        const bool ok = writeAll(buf_.data(), buf_.size(), pos_);
        pos_ += buf_.size();
        buf_.clear();
        return ok;
    }
    bool writeAll(const std::uint8_t* p, std::size_t n, std::uint64_t at) {
        std::size_t done = 0;
        while (done < n) {
            const ssize_t w = ::pwrite(fd_, p + done, n - done, off_t(at + done));
            if (w <= 0) return false;
            done += std::size_t(w);
        }
        return true;
    }
    std::string path_;
    int fd_ = -1;
    std::uint64_t pos_ = 0;
    std::vector<std::uint8_t> buf_;
};

}  // namespace pyre::tool
