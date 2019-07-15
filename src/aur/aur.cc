#include "aur.hh"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace aur {

namespace {

std::string_view GetEnv(const char* name) {
  auto value = getenv(name);
  return std::string_view(value ? value : "");
}

template <typename TimeRes, typename ClockType>
auto TimepointTo(std::chrono::time_point<ClockType> tp) {
  return std::chrono::time_point_cast<TimeRes>(tp).time_since_epoch().count();
}

bool ConsumePrefix(std::string_view* view, std::string_view prefix) {
  if (view->find(prefix) != 0) {
    return false;
  }

  view->remove_prefix(prefix.size());
  return true;
}

class ResponseHandler {
 public:
  explicit ResponseHandler(Aur* aur) : aur_(aur) {}
  virtual ~ResponseHandler() = default;

  ResponseHandler(const ResponseHandler&) = delete;
  ResponseHandler& operator=(const ResponseHandler&) = delete;

  ResponseHandler(ResponseHandler&&) = default;
  ResponseHandler& operator=(ResponseHandler&&) = default;

  static size_t BodyCallback(char* ptr, size_t size, size_t nmemb,
                             void* userdata) {
    auto handler = static_cast<ResponseHandler*>(userdata);

    handler->body.append(ptr, size * nmemb);
    return size * nmemb;
  }

  static int DebugCallback(CURL*, curl_infotype type, char* data, size_t size,
                           void* userdata) {
    auto stream = static_cast<std::ofstream*>(userdata);

    if (type != CURLINFO_HEADER_OUT) {
      return 0;
    }

    stream->write(data, size);
    return 0;
  }

  int RunCallback(long status, const std::string& error) {
    int r = Run(status, error);
    delete this;
    return r;
  }

  Aur* aur() const { return aur_; }

  std::string body;
  std::array<char, CURL_ERROR_SIZE> error_buffer = {};

 private:
  virtual int Run(long status, const std::string& error) = 0;

  Aur* aur_;
};

template <typename ResponseT, typename CallbackT>
class TypedResponseHandler : public ResponseHandler {
 public:
  using CallbackType = CallbackT;

  constexpr TypedResponseHandler(Aur* aur, CallbackT callback)
      : ResponseHandler(aur), callback_(std::move(callback)) {}

 protected:
  virtual ResponseT MakeResponse() { return ResponseT(std::move(body)); }

 private:
  int Run(long status, const std::string& error) override {
    return callback_(ResponseWrapper(MakeResponse(), status, error));
  }

  const CallbackT callback_;
};

using RpcResponseHandler =
    TypedResponseHandler<RpcResponse, Aur::RpcResponseCallback>;
using RawResponseHandler =
    TypedResponseHandler<RawResponse, Aur::RawResponseCallback>;

class CloneResponseHandler
    : public TypedResponseHandler<CloneResponse, Aur::CloneResponseCallback> {
 public:
  CloneResponseHandler(Aur* aur, Aur::CloneResponseCallback callback,
                       std::string operation)
      : TypedResponseHandler(aur, std::move(callback)),
        operation_(std::move(operation)) {}

 protected:
  CloneResponse MakeResponse() override {
    return CloneResponse(std::move(operation_));
  }

 private:
  std::string operation_;
};

}  // namespace

Aur::Aur(Options options) : options_(std::move(options)) {
  curl_global_init(CURL_GLOBAL_SSL);
  curl_multi_ = curl_multi_init();

  curl_multi_setopt(curl_multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
  curl_multi_setopt(curl_multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, 5L);

  curl_multi_setopt(curl_multi_, CURLMOPT_SOCKETFUNCTION, &Aur::SocketCallback);
  curl_multi_setopt(curl_multi_, CURLMOPT_SOCKETDATA, this);

  curl_multi_setopt(curl_multi_, CURLMOPT_TIMERFUNCTION, &Aur::TimerCallback);
  curl_multi_setopt(curl_multi_, CURLMOPT_TIMERDATA, this);

  sigset_t ss{};
  sigaddset(&ss, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ss, &saved_ss_);

  sd_event_default(&event_);

  std::string_view debug = GetEnv("AURACLE_DEBUG");
  if (ConsumePrefix(&debug, "requests:")) {
    debug_level_ = DebugLevel::REQUESTS;
    debug_stream_.open(std::string(debug), std::ofstream::trunc);
  } else if (!debug.empty()) {
    debug_level_ = DebugLevel::VERBOSE_STDERR;
  }
}

Aur::~Aur() {
  curl_multi_cleanup(curl_multi_);
  curl_global_cleanup();

  sd_event_source_unref(timer_);
  sd_event_unref(event_);

  sigprocmask(SIG_SETMASK, &saved_ss_, nullptr);

  if (debug_stream_.is_open()) {
    debug_stream_.close();
  }
}

void Aur::Cancel(const ActiveRequests::value_type& request) {
  struct Visitor {
    constexpr explicit Visitor(Aur* aur) : aur(aur) {}

    void operator()(CURL* curl) {
      aur->FinishRequest(curl, CURLE_ABORTED_BY_CALLBACK,
                         /*dispatch_callback=*/false);
    }

    void operator()(sd_event_source* source) { aur->FinishRequest(source); }

    Aur* aur;
  };

  std::visit(Visitor(this), request);
}

void Aur::CancelAll() {
  while (!active_requests_.empty()) {
    Cancel(*active_requests_.begin());
  }

  cancelled_ = true;
}

// static
int Aur::SocketCallback(CURLM*, curl_socket_t s, int action, void* userdata,
                        void* sockptr) {
  auto aur = static_cast<Aur*>(userdata);
  auto io = static_cast<sd_event_source*>(sockptr);
  return aur->DispatchSocketCallback(s, action, io);
}

int Aur::DispatchSocketCallback(curl_socket_t s, int action,
                                sd_event_source* io) {
  if (action == CURL_POLL_REMOVE) {
    return FinishRequest(io);
  }

  auto action_to_revents = [](int action) -> std::uint32_t {
    switch (action) {
      case CURL_POLL_IN:
        return EPOLLIN;
      case CURL_POLL_OUT:
        return EPOLLOUT;
      case CURL_POLL_INOUT:
        return EPOLLIN | EPOLLOUT;
      default:
        return 0;
    }
  };
  std::uint32_t events = action_to_revents(action);

  if (io != nullptr) {
    if (sd_event_source_set_io_events(io, events) < 0) {
      return -1;
    }

    if (sd_event_source_set_enabled(io, SD_EVENT_ON) < 0) {
      return -1;
    }
  } else {
    if (sd_event_add_io(event_, &io, s, events, &Aur::OnCurlIO, this) < 0) {
      return -1;
    }

    if (curl_multi_assign(curl_multi_, s, io) != CURLM_OK) {
      return -1;
    }
  }

  return 0;
}

// static
int Aur::OnCurlIO(sd_event_source*, int fd, uint32_t revents, void* userdata) {
  auto aur = static_cast<Aur*>(userdata);

  int action;
  if ((revents & (EPOLLIN | EPOLLOUT)) == (EPOLLIN | EPOLLOUT)) {
    action = CURL_POLL_INOUT;
  } else if (revents & EPOLLIN) {
    action = CURL_POLL_IN;
  } else if (revents & EPOLLOUT) {
    action = CURL_POLL_OUT;
  } else {
    action = 0;
  }

  int unused;
  if (curl_multi_socket_action(aur->curl_multi_, fd, action, &unused) !=
      CURLM_OK) {
    return -EINVAL;
  }

  return aur->CheckFinished();
}

// static
int Aur::OnCurlTimer(sd_event_source*, uint64_t, void* userdata) {
  auto aur = static_cast<Aur*>(userdata);

  int unused;
  if (curl_multi_socket_action(aur->curl_multi_, CURL_SOCKET_TIMEOUT, 0,
                               &unused) != CURLM_OK) {
    return -EINVAL;
  }

  return aur->CheckFinished();
}

// static
int Aur::TimerCallback(CURLM*, long timeout_ms, void* userdata) {
  auto aur = static_cast<Aur*>(userdata);
  return aur->DispatchTimerCallback(timeout_ms);
}

int Aur::DispatchTimerCallback(long timeout_ms) {
  if (timeout_ms < 0) {
    if (sd_event_source_set_enabled(timer_, SD_EVENT_OFF) < 0) {
      return -1;
    }

    return 0;
  }

  auto usec = TimepointTo<std::chrono::microseconds>(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));

  if (timer_ != nullptr) {
    if (sd_event_source_set_time(timer_, usec) < 0) {
      return -1;
    }

    if (sd_event_source_set_enabled(timer_, SD_EVENT_ONESHOT) < 0) {
      return -1;
    }
  } else {
    if (sd_event_add_time(event_, &timer_, CLOCK_MONOTONIC, usec, 0,
                          &Aur::OnCurlTimer, this) < 0) {
      return -1;
    }
  }

  return 0;
}

int Aur::FinishRequest(CURL* curl, CURLcode result, bool dispatch_callback) {
  ResponseHandler* handler;
  curl_easy_getinfo(curl, CURLINFO_PRIVATE, &handler);

  int r = 0;
  if (dispatch_callback) {
    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    std::string error;
    if (result != CURLE_OK) {
      error = handler->error_buffer.data();
      if (error.empty()) {
        error = curl_easy_strerror(result);
      }
    }

    r = handler->RunCallback(response_code, error);
  } else {
    delete handler;
  }

  active_requests_.erase(curl);
  curl_multi_remove_handle(curl_multi_, curl);
  curl_easy_cleanup(curl);

  return r;
}

int Aur::FinishRequest(sd_event_source* source) {
  active_requests_.erase(source);
  sd_event_source_unref(source);
  return 0;
}

int Aur::CheckFinished() {
  int unused;

  auto msg = curl_multi_info_read(curl_multi_, &unused);
  if (msg == nullptr || msg->msg != CURLMSG_DONE) {
    return 0;
  }

  auto r = FinishRequest(msg->easy_handle, msg->data.result,
                         /* dispatch_callback = */ true);
  if (r < 0) {
    CancelAll();
  }

  return r;
}

int Aur::Wait() {
  cancelled_ = false;

  while (!active_requests_.empty()) {
    if (sd_event_run(event_, 1) < 0) {
      return -EIO;
    }
  }

  return cancelled_ ? -ECANCELED : 0;
}

struct RpcRequestTraits {
  using ResponseHandlerType = RpcResponseHandler;

  static constexpr char const* kEncoding = "";
};

struct RawRequestTraits {
  using ResponseHandlerType = RawResponseHandler;

  static constexpr char const* kEncoding = "";
};

struct TarballRequestTraits {
  using ResponseHandlerType = RawResponseHandler;

  static constexpr char const* kEncoding = "identity";
};

template <typename RequestTraits>
void Aur::QueueHttpRequest(
    const HttpRequest& request,
    const typename RequestTraits::ResponseHandlerType::CallbackType& callback) {
  for (const auto& r : request.Build(options_.baseurl)) {
    auto curl = curl_easy_init();

    auto handler =
        new typename RequestTraits::ResponseHandlerType(this, callback);

    using RH = ResponseHandler;
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);
    curl_easy_setopt(curl, CURLOPT_URL, r.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &RH::BodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, handler);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, handler);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, handler->error_buffer.data());
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, RequestTraits::kEncoding);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, options_.useragent.c_str());

    switch (debug_level_) {
      case DebugLevel::NONE:
        break;
      case DebugLevel::REQUESTS:
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, &RH::DebugCallback);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &debug_stream_);
        [[fallthrough]];
      case DebugLevel::VERBOSE_STDERR:
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        break;
    }

    curl_multi_add_handle(curl_multi_, curl);
    active_requests_.emplace(curl);
  }
}

// static
int Aur::OnCloneExit(sd_event_source* source, const siginfo_t* si,
                     void* userdata) {
  auto handler = static_cast<CloneResponseHandler*>(userdata);

  handler->aur()->FinishRequest(source);

  std::string error;
  if (si->si_status != 0) {
    error.assign("git exited with unexpected exit status " +
                 std::to_string(si->si_status));
  }

  return handler->RunCallback(si->si_status, error);
}

void Aur::QueueCloneRequest(const CloneRequest& request,
                            const CloneResponseCallback& callback) {
  const bool update = fs::exists(fs::path(request.reponame()) / ".git");

  auto handler =
      new CloneResponseHandler(this, callback, update ? "update" : "clone");

  int pid = fork();
  if (pid < 0) {
    handler->RunCallback(-errno, "failed to fork new process for git: " +
                                     std::string(strerror(errno)));
    return;
  }

  if (pid == 0) {
    const auto url = request.Build(options_.baseurl)[0];

    std::vector<const char*> cmd;
    if (update) {
      // clang-format off
      cmd = {
        "git",
         "-C",
        request.reponame().c_str(),
        "pull",
         "--quiet",
         "--rebase",
         "--autostash",
         "--ff-only",
      };
      // clang-format on
    } else {
      // clang-format off
      cmd = {
        "git",
        "clone",
        "--quiet",
        url.c_str(),
      };
      // clang-format on
    }
    cmd.push_back(nullptr);

    execvp(cmd[0], const_cast<char* const*>(cmd.data()));
    _exit(127);
  }

  sd_event_source* child;
  sd_event_add_child(event_, &child, pid, WEXITED, &Aur::OnCloneExit, handler);

  active_requests_.emplace(child);
}

void Aur::QueueRawRequest(const HttpRequest& request,
                          const RawResponseCallback& callback) {
  QueueHttpRequest<RawRequestTraits>(request, callback);
}

void Aur::QueueRpcRequest(const RpcRequest& request,
                          const RpcResponseCallback& callback) {
  QueueHttpRequest<RpcRequestTraits>(request, callback);
}

void Aur::QueueTarballRequest(const RawRequest& request,
                              const RawResponseCallback& callback) {
  QueueHttpRequest<TarballRequestTraits>(request, callback);
}

}  // namespace aur

/* vim: set et ts=2 sw=2: */
