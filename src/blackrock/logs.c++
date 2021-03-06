// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "logs.h"
#include "cluster-rpc.h"
#include <sandstorm/util.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <stdio.h>
#include <errno.h>

namespace blackrock {

class LogSink::ClientHandler {
public:
  ClientHandler(LogSink& sink, kj::Own<kj::AsyncIoStream> stream, kj::String addr)
      : sink(sink), stream(kj::mv(stream)), addr(kj::mv(addr)) {}

  kj::Promise<void> run() {
    return stream->tryRead(buffer + leftover, 1, sizeof(buffer) - leftover)
        .then([this](size_t amount) -> kj::Promise<void> {
      if (amount == 0) {
        if (prefix == nullptr) {
          // Never got any data on this stream. Probably just a probe. Don't print anything.
        } else {
          if (leftover > 0) {
            buffer[leftover] = '\n';
            writeLine(kj::arrayPtr(buffer, leftover + 1));
          }
          writeLine(kj::StringPtr("DISCONNECTED\n"));
        }
        return kj::READY_NOW;
      }

      amount += leftover;

      // Split into lines.
      uint lineStart = 0;
      for (uint i = 0; i < amount; i++) {
        if (buffer[i] == '\n') {
          writeLine(kj::arrayPtr(buffer + lineStart, i + 1 - lineStart));
          lineStart = i + 1;
        } else if (i - lineStart >= 8192) {
          // Force a line split at 8k to avoid excessive buffering.
          char c = buffer[i];
          buffer[i] = '\n';
          writeLine(kj::arrayPtr(buffer + lineStart, i + 1 - lineStart));
          buffer[i] = c;

          // Insert a "..." prefix on the continuation.
          buffer[i-1] = '.';
          buffer[i-2] = '.';
          buffer[i-3] = '.';
          lineStart = i - 3;
        }
      }

      // Move trailing text to the start of the buffer.
      leftover = amount - lineStart;
      memmove(buffer, buffer + lineStart, leftover);

      return run();
    });
  }

private:
  LogSink& sink;
  kj::Own<kj::AsyncIoStream> stream;
  kj::String addr;
  kj::String prefix;
  uint leftover = 0;
  char buffer[16384];

  void writeLine(kj::ArrayPtr<const char> chars) {
    if (chars.size() == 0) return;

    if (prefix == nullptr) {
      // This is the first line received. Treat it as the name, if it's valid.

      kj::ArrayPtr<const char> name = chars.slice(0, chars.size() - 1);

      // We expect the name to be a 16-or-fewer character hostname.
      bool valid = true;
      if (name.size() > 16 || name.size() == 0) {
        valid = false;
      } else {
        for (char c: name) {
          if ((c < 'a' || 'z' < c) &&
              (c < 'A' || 'Z' < c) &&
              (c < '0' || '9' < c) &&
              c != '-' && c != '_') {
            valid = false;
            break;
          }
        }
      }

      if (!valid) {
        name = addr;
      }

      if (!sink.namesSeen.insert(kj::str(name)).second) {
        // Dup name.
        for (uint counter = 1; ; ++counter) {
          kj::String altName = kj::str(name, '.', counter);
          kj::StringPtr ptr = altName;
          if (sink.namesSeen.insert(kj::mv(altName)).second) {
            name = ptr;
            break;
          }
        }
      }

      if (valid) {
        sink.write(kj::str(" * ", name, " (", addr, ") CONNECTED\n"));
      } else {
        sink.write(kj::str(" * ??? (", addr, ") CONNECTED\n"));
      }

      prefix = kj::str(" [", name, kj::repeat(' ', 16 - kj::min(name.size(), 16)), "] ");

      if (valid) {
        // This line became the name, so don't write it.
        return;
      }
    }

    sink.write(prefix, chars);
  }
};

LogSink::LogSink(): tasks(*this) {}

kj::Promise<void> LogSink::acceptLoop(kj::Own<kj::ConnectionReceiver> receiver) {
  auto promise = receiver->accept();
  return promise.then([this,KJ_MVCAP(receiver)](kj::Own<kj::AsyncIoStream> stream) mutable {
    auto addr = kj::str(SimpleAddress::getPeer(*stream));
    auto client = kj::heap<ClientHandler>(*this, kj::mv(stream), kj::mv(addr));
    auto promise = client->run();
    tasks.add(promise.attach(kj::mv(client)));
    return acceptLoop(kj::mv(receiver));
  });
}

void LogSink::write(kj::ArrayPtr<const char> part1, kj::ArrayPtr<const char> part2) {
  char timestampBuf[128];
  time_t now = time(nullptr);
  struct tm local;
  KJ_ASSERT(gmtime_r(&now, &local) != nullptr);
  size_t n = strftime(timestampBuf, sizeof(timestampBuf), "%Y-%m-%d_%H-%M-%S", &local);

  kj::StringPtr timestamp(timestampBuf, n);

  kj::ArrayPtr<const byte> pieces[3] = { timestamp.asBytes(), part1.asBytes(), part2.asBytes() };
  kj::FdOutputStream(STDOUT_FILENO).write(pieces);
}

void LogSink::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, "exception in log sink read loop", exception);
}

// =======================================================================================

static time_t currentDay() {
  // Note that POSIX.1-2008 Rationale A.4.15 states that every days is exactly 86400 seconds in
  // sections-since-Epoch time. Leap seconds don't exist.
  return time(nullptr) / 86400;
}

void rotateLogs(int input, int logDirFd) {
  char buffer [8192];

  time_t day = currentDay();
  kj::AutoCloseFd output;
  for (;;) {
    ssize_t n;
    KJ_SYSCALL(n = read(input, buffer, sizeof(buffer)));
    if (n == 0) break;

    if (output == nullptr) {
      char timestamp[128];
      time_t now = day * 86400;
      struct tm local;
      KJ_ASSERT(gmtime_r(&now, &local) != nullptr);
      strftime(timestamp, sizeof(timestamp), "blackrock.%Y-%m-%d", &local);

      output = sandstorm::raiiOpenAt(logDirFd, timestamp, O_WRONLY | O_CREAT | O_APPEND);

      while (unlinkat(logDirFd, "blackrock.current", 0) < 0) {
        int error = errno;
        if (error == ENOENT) {
          // fine
          break;
        } else if (error != EINTR) {
          KJ_FAIL_SYSCALL("unlinkat(logdir, 'blackrock.current')", error);
        }
      }

      KJ_SYSCALL(symlinkat(timestamp, logDirFd, "blackrock.current"));
    }

    kj::FdOutputStream(output.get()).write(buffer, n);

    time_t newDay = currentDay();
    if (newDay > day && buffer[n-1] == '\n') {
      // A new day just started and we just saw a line break. Start a new file.
      output = nullptr;
      day = newDay;
    }
  }
}

// =======================================================================================

class LogClient {
public:
  LogClient(kj::Network& network, kj::Timer& timer, kj::StringPtr name,
            kj::StringPtr backlogDir, kj::StringPtr logAddressFile,
            kj::Own<kj::AsyncInputStream> input)
      : network(network),
        timer(timer),
        nameLine(kj::str(name, '\n')),
        logAddressFile(logAddressFile),
        input(kj::mv(input)),
        backlogName(kj::str(backlogDir, "/blackrock-backlog.", time(nullptr), '.', getpid())),
        backlog(sandstorm::raiiOpen(backlogName, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)),
        reconnectTask(reconnect()) {}

  void redirectToBacklog(int fd) {
    KJ_SYSCALL(dup2(backlog, fd));
  }

  kj::Promise<void> run() {
    return input->tryRead(buffer, 1, sizeof(buffer)).then([this](size_t size) {
      if (size == 0) {
        // EOF -- the main process exited. Finish up writing.
        return writeQueue.then([this]() {
          // In case we're not currently connected, we'll keep trying to reconnect and upload logs
          // for 30 seconds. If we don't manage to do so, we'll leave our log file on local disk.
          return reconnectTask.then([this]() {
            // Successfully uploaded the logs, so let's delete the file.
            KJ_SYSCALL(unlink(backlogName.cStr()));
          }).exclusiveJoin(timer.afterDelay(30 * kj::SECONDS));
        });
      } else {
        auto data = kj::heapArray<byte>(buffer, size);
        kj::ArrayPtr<const byte> dataPtr = data;
        writeQueue = writeQueue.then([this,size,dataPtr]() -> kj::Promise<void> {
          KJ_IF_MAYBE(c, connection) {
            if (receivedEof) {
              // It appears that we've received an EOF from the other end, therefore anything we
              // write() now may be silently lost.
              connection = nullptr;
              writeBacklog(dataPtr);
              reconnectTask = reconnect();
              return kj::READY_NOW;
            } else {
              return kj::evalNow([&]() {
                return c->get()->write(dataPtr.begin(), dataPtr.size());
              }).catch_([this,dataPtr](kj::Exception&& exception) {
                // Cancel existing reconnect task (which may still be looping in awaitEof(), which
                // uses `connection`, which we're about to destroy).
                reconnectTask = nullptr;

                // Discard the connection.
                connection = nullptr;
                if (expectDisconnected(exception)) {
                  KJ_LOG(ERROR, "log sink disconnected (write error); trying to reconnect");
                }
                writeBacklog(dataPtr);
                reconnectTask = reconnect();
              });
            }
          } else {
            writeBacklog(dataPtr);
            return kj::READY_NOW;
          }
        }).attach(kj::mv(data));

        return run();
      }
    });
  }

private:
  kj::Network& network;
  kj::Timer& timer;
  kj::String nameLine;
  kj::StringPtr logAddressFile;
  kj::Own<kj::AsyncInputStream> input;
  kj::String backlogName;
  kj::AutoCloseFd backlog;

  kj::Maybe<kj::Own<kj::AsyncIoStream>> connection;
  kj::Promise<void> writeQueue = kj::READY_NOW;
  kj::Promise<void> reconnectTask;
  bool receivedEof = false;
  off_t backlogOffset = 0;
  byte buffer[4096];
  byte backlogBuffer[4096];

  void writeBacklog(kj::ArrayPtr<const byte> data) {
    kj::FdOutputStream(backlog.get()).write(data.begin(), data.size());
  }

  kj::Promise<void> reconnect() {
    return reconnectLoop().eagerlyEvaluate([](kj::Exception&& e) {
      // This is not supposed to happen because we carefully catch exceptions.
      KJ_LOG(ERROR, "failure in log client reconnect loop", e);
    });
  }

  kj::Promise<void> reconnectLoop() {
    return kj::evalNow([&]() {
      // Read the log address from the file.
      SimpleAddress address = nullptr;
      kj::FdInputStream(sandstorm::raiiOpen(logAddressFile, O_RDONLY | O_CLOEXEC))
          .read(&address, sizeof(address));

      // Connect to it.
      auto addressObj = address.onNetwork(network);
      auto promise = addressObj->connect();
      return promise.attach(kj::mv(addressObj));
    }).then([this](kj::Own<kj::AsyncIoStream>&& newConnection) -> kj::Promise<void> {
      // Connected, start writing the backlog to the new connection.

      auto promise = kj::evalNow([&]() {
        return newConnection->write(nameLine.begin(), nameLine.size());
      });

      return promise.then([this,KJ_MVCAP(newConnection)]() mutable {
        return writeBacklog(kj::mv(newConnection));
      }, [this](kj::Exception&& exception) {
        // Dang, connection failed right away. Keep trying.
        expectDisconnected(exception);
        return reconnectLoop();
      });
    }, [this](kj::Exception&& exception) {
      // Connection failed. Try again in 10 seconds.
      expectDisconnected(exception);
      return timer.afterDelay(10 * kj::SECONDS).then([this]() {
        return reconnectLoop();
      });
    });
  }

  kj::Promise<void> writeBacklog(kj::Own<kj::AsyncIoStream> newConnection) {
    ssize_t n;
    KJ_SYSCALL(n = pread(backlog, backlogBuffer, sizeof(backlogBuffer), backlogOffset));

    if (n == 0) {
      // We're all caught up!

      // Truncate the backlog; it's all saved.
      KJ_SYSCALL(lseek(backlog, 0, SEEK_SET));
      KJ_SYSCALL(ftruncate(backlog, 0));
      backlogOffset = 0;

      // Now we can continue on.
      receivedEof = false;
      auto promise = awaitEof(*newConnection);
      connection = kj::mv(newConnection);
      return promise;
    } else {
      backlogOffset += n;

      // Send backlog up to server.
      auto promise = kj::evalNow([&]() {
        return newConnection->write(backlogBuffer, n);
      });

      return promise.then([this,KJ_MVCAP(newConnection)]() mutable {
        return writeBacklog(kj::mv(newConnection));
      }, [this](kj::Exception&& exception) {
        // Dang, failed while trying to upload the backlog.
        expectDisconnected(exception);
        return reconnectLoop();
      });
    }
  }

  bool expectDisconnected(const kj::Exception& exception) {
    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      return true;
    } else {
      KJ_LOG(ERROR, "unexpected exception in log gatherer", exception);
      return false;
    }
  }

  kj::Promise<void> awaitEof(kj::AsyncIoStream& connection) {
    static byte dummy[1024];
    return connection.tryRead(&dummy, 1, sizeof(dummy))
        .then([this,&connection](size_t n) -> kj::Promise<void> {
      if (n > 0) {
        return awaitEof(connection);
      } else {
        KJ_LOG(ERROR, "log sink disconnected (EOF); will reconnect on next log");
        receivedEof = true;
        return kj::READY_NOW;
      }
    }, [this](kj::Exception&& exception) -> kj::Promise<void> {
      if (expectDisconnected(exception)) {
        KJ_LOG(ERROR, "log sink disconnected (read error); will reconnect on next log");
      }
      receivedEof = true;
      return kj::READY_NOW;
    });
  }
};

void runLogClient(kj::StringPtr name, kj::StringPtr logAddressFile, kj::StringPtr backlogDir) {
  auto ioContext = kj::setupAsyncIo();

  LogClient client(ioContext.provider->getNetwork(),
                   ioContext.lowLevelProvider->getTimer(),
                   name, backlogDir, logAddressFile,
                   ioContext.lowLevelProvider->wrapInputFd(STDIN_FILENO));
  client.redirectToBacklog(STDOUT_FILENO);
  client.redirectToBacklog(STDERR_FILENO);
  client.run().wait(ioContext.waitScope);
  KJ_UNREACHABLE;
}

} // namespace blackrock

