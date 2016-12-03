#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

#include "errors.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

POLYBAR_NS

namespace io_util {
  string read(int read_fd, int bytes_to_read, int& bytes_read_loc, int& status_loc) {
    char buffer[BUFSIZ - 1];

    if (bytes_to_read == -1) {
      bytes_to_read = sizeof(buffer);
    }

    status_loc = 0;

    if ((bytes_read_loc = ::read(read_fd, &buffer, bytes_to_read)) == -1) {
      throw system_error("Error trying to read from fd");
    } else if (bytes_read_loc == 0) {
      status_loc = -1;
    } else {
      buffer[bytes_read_loc] = '\0';
    }

    return {buffer};
  }

  string read(int read_fd, int bytes_to_read) {
    int bytes_read = 0;
    int status = 0;
    return read(read_fd, bytes_to_read, bytes_read, status);
  }

  string readline(int read_fd, int& bytes_read) {
    stringstream buffer;
    char char_;
    int bytes = 0;
    bytes_read = 0;

    while ((bytes = ::read(read_fd, &char_, 1)) > 0) {
      if (bytes <= 0) {
        break;
      }
      if (char_ == '\n' || char_ == '\x00') {
        break;
      }
      bytes_read += bytes;
      buffer << char_;
    }

    if (bytes_read <= 0) {
      return "";
    }
    return string_util::strip_trailing_newline(buffer.str());
  }

  string readline(int read_fd) {
    int bytes_read;
    return readline(read_fd, bytes_read);
  }

  size_t write(int write_fd, const string& data) {
    return ::write(write_fd, data.c_str(), strlen(data.c_str()));
  }

  size_t writeline(int write_fd, const string& data) {
    if (data.length() == 0) {
      return -1;
    }
    if (data.substr(data.length() - 1, 1) != "\n") {
      return io_util::write(write_fd, data + "\n");
    } else {
      return io_util::write(write_fd, data);
    }
  }

  void tail(int read_fd, const function<void(string)>& callback) {
    int bytes_read;
    while (true) {
      auto line = io_util::readline(read_fd, bytes_read);
      if (bytes_read <= 0) {
        break;
      }
      callback(line);
    }
  }

  void tail(int read_fd, int writeback_fd) {
    tail(read_fd, [&](string data) { io_util::writeline(writeback_fd, data); });
  }

  bool poll(int fd, short int events, int timeout_ms) {
    struct pollfd fds[1];
    fds[0].fd = fd;
    fds[0].events = events;

    ::poll(fds, 1, timeout_ms);

    return fds[0].revents & events;
  }

  bool poll_read(int fd, int timeout_ms) {
    return poll(fd, POLLIN, timeout_ms);
  }

  bool poll_write(int fd, int timeout_ms) {
    return poll(fd, POLLOUT, timeout_ms);
  }

  bool interrupt_read(int write_fd) {
    char end[1] = {'\n'};
    size_t bytes = ::write(write_fd, end, 1);
    return bytes > 0;
  }
}

POLYBAR_NS_END
