#include "coro/http/curl_http.h"

#include <curl/curl.h>
#include <event2/event.h>
#include <event2/event_struct.h>

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

#include "coro/http/http_body_generator.h"
#include "coro/interrupted_exception.h"

#ifdef USE_BUNDLED_CACERT
#include "coro/http/assets.h"
#endif

namespace coro::http {

namespace {

#ifdef WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

struct SocketData {
  event socket_event = {};
};

struct CurlHandleDeleter {
  void operator()(CURL* handle) const {
    if (handle) {
      curl_easy_cleanup(handle);
    }
  }
};

struct CurlListDeleter {
  void operator()(curl_slist* list) const {
    if (list) {
      curl_slist_free_all(list);
    }
  }
};

template <typename T>
T* CheckNotNull(T* p) {
  if (!p) {
    throw std::runtime_error("Unexpected null pointer.");
  }
  return p;
}

void Check(CURLMcode code) {
  if (code != CURLM_OK) {
    throw HttpException(code, curl_multi_strerror(code));
  }
}

void Check(CURLcode code) {
  if (code != CURLE_OK) {
    throw HttpException(code, curl_easy_strerror(code));
  }
}

void Check(int code) {
  if (code != 0) {
    throw HttpException(code, "Unknown error.");
  }
}

class CurlHttpImpl;
class CurlHttpOperation;
class CurlHttpBodyGenerator;

class CurlHandle {
 public:
  using Owner = std::variant<CurlHttpOperation*, CurlHttpBodyGenerator*>;

  CurlHandle(CURLM* http, event_base* event_loop, Request<>,
             const std::string* cache_path, stdx::stop_token, Owner);

  ~CurlHandle() noexcept;

 private:
  friend class CurlHttpImpl;
  friend class CurlHttpOperation;
  friend class CurlHttpBodyGenerator;

  struct OnCancel {
    void operator()() const;
    CurlHandle* data;
  };

  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb,
                              void* userdata);
  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems,
                               void* userdata);
  static int ProgressCallback(void* clientp, curl_off_t dltotal,
                              curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow);
  static size_t ReadCallback(char* buffer, size_t size, size_t nitems,
                             void* userdata);
  static void OnNextRequestBodyChunkRequested(evutil_socket_t, short,
                                              void* userdata);

  void Cleanup();

  void HandleException(std::exception_ptr exception);

  CURLM* http_;
  event_base* event_loop_;
  std::unique_ptr<CURL, CurlHandleDeleter> handle_;
  std::unique_ptr<curl_slist, CurlListDeleter> header_list_;
  std::optional<Generator<std::string>> request_body_;
  std::optional<Generator<std::string>::iterator> request_body_it_;
  std::optional<size_t> request_body_chunk_index_;
  stdx::stop_token stop_token_;
  Owner owner_;
  event next_request_body_chunk_;
  stdx::stop_callback<OnCancel> stop_callback_;
};

class CurlHttpBodyGenerator : public HttpBodyGenerator<CurlHttpBodyGenerator> {
 public:
  CurlHttpBodyGenerator(std::unique_ptr<CurlHandle> handle,
                        std::string_view initial_chunk);

  CurlHttpBodyGenerator(const CurlHttpBodyGenerator&) = delete;
  CurlHttpBodyGenerator(CurlHttpBodyGenerator&&) = delete;
  ~CurlHttpBodyGenerator() noexcept;

  CurlHttpBodyGenerator& operator=(const CurlHttpBodyGenerator&) = delete;
  CurlHttpBodyGenerator& operator=(CurlHttpBodyGenerator&&) = delete;

  void Resume();

 private:
  static void OnChunkReady(evutil_socket_t, short, void* handle);
  static void OnBodyReady(evutil_socket_t, short, void* handle);

  friend class CurlHttpOperation;
  friend class CurlHandle;
  friend class CurlHttpImpl;

  event chunk_ready_;
  event body_ready_;
  bool body_ready_fired_ = false;
  int status_ = -1;
  std::exception_ptr exception_ptr_;
  std::string data_;
  std::unique_ptr<CurlHandle> handle_;
};

class CurlHttpOperation {
 public:
  CurlHttpOperation(CURLM* http, event_base* event_loop, Request<>,
                    const std::string* cache_path, stdx::stop_token);
  CurlHttpOperation(const CurlHttpOperation&) = delete;
  CurlHttpOperation(CurlHttpOperation&&) = delete;
  ~CurlHttpOperation();

  CurlHttpOperation& operator=(const CurlHttpOperation&) = delete;
  CurlHttpOperation& operator=(CurlHttpOperation&&) = delete;

  bool await_ready();
  void await_suspend(stdx::coroutine_handle<void> awaiting_coroutine);
  std::unique_ptr<Response<CurlHttpBodyGenerator>> await_resume();

 private:
  static void OnHeadersReady(evutil_socket_t fd, short event, void* handle);

  friend class CurlHttpImpl;
  friend class CurlHandle;

  stdx::coroutine_handle<void> awaiting_coroutine_;
  std::exception_ptr exception_ptr_;
  event headers_ready_;
  bool headers_ready_event_posted_;
  int status_ = -1;
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string body_;
  bool no_body_ = false;
  std::unique_ptr<CurlHandle> handle_;
};

class CurlHttpImpl {
 public:
  CurlHttpImpl(event_base* event_loop, std::optional<std::string> cache_path);

  CurlHttpImpl(CurlHttpImpl&&) = delete;
  CurlHttpImpl& operator=(CurlHttpImpl&&) = delete;

  ~CurlHttpImpl();

  CurlHttpOperation Fetch(Request<> request,
                          stdx::stop_token = stdx::stop_token()) const;

 private:
  static int SocketCallback(CURL* handle, curl_socket_t socket, int what,
                            void* userp, void* socketp);
  static int TimerCallback(CURLM* handle, long timeout_ms, void* userp);
  static void SocketEvent(evutil_socket_t fd, short event, void* multi_handle);
  static void TimeoutEvent(evutil_socket_t fd, short event, void* handle);
  static void ProcessEvents(CURLM* handle);

  friend class CurlHttpOperation;
  friend class CurlHttpBodyGenerator;
  friend class CurlHandle;

  struct CurlMultiDeleter {
    void operator()(CURLM* handle) const;
  };

  std::unique_ptr<CURLM, CurlMultiDeleter> curl_handle_;
  event_base* event_loop_;
  event timeout_event_;
  std::optional<std::string> cache_path_;
};

CurlHandle::~CurlHandle() noexcept { Cleanup(); }

void CurlHandle::Cleanup() {
  if (http_) {
    Check(curl_multi_remove_handle(http_, handle_.get()));
    http_ = nullptr;
  }
  if (next_request_body_chunk_.ev_base) {
    event_del(&next_request_body_chunk_);
  }
}

void CurlHandle::HandleException(std::exception_ptr exception) {
  if (auto* operation = std::get_if<CurlHttpOperation*>(&owner_)) {
    Cleanup();
    (*operation)->exception_ptr_ = std::move(exception);
    if ((*operation)->awaiting_coroutine_) {
      std::exchange((*operation)->awaiting_coroutine_, nullptr).resume();
    }
  } else if (auto* generator = std::get_if<CurlHttpBodyGenerator*>(&owner_)) {
    Cleanup();
    (*generator)->exception_ptr_ = std::move(exception);
    (*generator)->Close((*generator)->exception_ptr_);
  }
}

size_t CurlHandle::HeaderCallback(char* buffer, size_t size, size_t nitems,
                                  void* userdata) {
  auto* data = reinterpret_cast<CurlHandle*>(userdata);
  if (!std::holds_alternative<CurlHttpOperation*>(data->owner_)) {
    return 0;
  }
  auto* http_operation = std::get<CurlHttpOperation*>(data->owner_);
  std::string_view view(buffer, size * nitems);
  auto index = view.find_first_of(':');
  if (index != std::string::npos) {
    http_operation->headers_.emplace_back(
        ToLowerCase(std::string(view.begin(), view.begin() + index)),
        TrimWhitespace(std::string(view.begin() + index + 1, view.end())));
  } else if (view.starts_with("HTTP")) {
    std::istringstream stream{std::string(view)};
    std::string http_version;
    int code;
    stream >> http_version >> code;
    http_operation->headers_.clear();
    http_operation->status_ = code;
  }
  return size * nitems;
}

size_t CurlHandle::WriteCallback(char* ptr, size_t size, size_t nmemb,
                                 void* userdata) {
  auto* data = reinterpret_cast<CurlHandle*>(userdata);
  if (std::holds_alternative<CurlHttpOperation*>(data->owner_)) {
    auto* http_operation = std::get<CurlHttpOperation*>(data->owner_);
    if (!http_operation->headers_ready_event_posted_) {
      http_operation->headers_ready_event_posted_ = true;
      evuser_trigger(&http_operation->headers_ready_);
    }
    http_operation->body_ += std::string(ptr, ptr + size * nmemb);
  } else if (std::holds_alternative<CurlHttpBodyGenerator*>(data->owner_)) {
    auto* http_body_generator = std::get<CurlHttpBodyGenerator*>(data->owner_);
    if (!http_body_generator->data_.empty() ||
        http_body_generator->GetBufferedByteCount() > 0) {
      return CURL_WRITEFUNC_PAUSE;
    }
    http_body_generator->data_ += std::string(ptr, ptr + size * nmemb);
    evuser_trigger(&http_body_generator->chunk_ready_);
  }
  return size * nmemb;
}

int CurlHandle::ProgressCallback(void* clientp, curl_off_t /*dltotal*/,
                                 curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                 curl_off_t /*ulnow*/) {
  auto* data = reinterpret_cast<CurlHandle*>(clientp);
  return data->stop_token_.stop_requested() ? -1 : 0;
}

size_t CurlHandle::ReadCallback(char* buffer, size_t size, size_t nitems,
                                void* userdata) {
  auto* data = reinterpret_cast<CurlHandle*>(userdata);
  if (!data->request_body_it_ || !data->request_body_chunk_index_) {
    return CURL_READFUNC_PAUSE;
  }
  if (data->request_body_it_ == std::end(*data->request_body_)) {
    return 0;
  }
  std::string& current_chunk = **data->request_body_it_;
  size_t offset = 0;
  for (; offset < size * nitems &&
         data->request_body_chunk_index_ < current_chunk.size();
       offset++) {
    buffer[offset] = current_chunk[(*data->request_body_chunk_index_)++];
  }
  if (data->request_body_chunk_index_ == current_chunk.size()) {
    data->request_body_chunk_index_ = std::nullopt;
    evuser_trigger(&data->next_request_body_chunk_);
  }
  return offset > 0 ? offset : CURL_READFUNC_PAUSE;
}

void CurlHandle::OnNextRequestBodyChunkRequested(evutil_socket_t, short,
                                                 void* userdata) {
  RunTask([data = reinterpret_cast<CurlHandle*>(userdata)]() -> Task<> {
    try {
      data->request_body_it_ = co_await ++*data->request_body_it_;
      data->request_body_chunk_index_ = 0;
      curl_easy_pause(data->handle_.get(), CURLPAUSE_SEND_CONT);
    } catch (...) {
      data->HandleException(std::current_exception());
    }
  });
}

void CurlHandle::OnCancel::operator()() const {
  data->HandleException(std::make_exception_ptr(InterruptedException()));
}

CurlHandle::CurlHandle(CURLM* http, event_base* event_loop, Request<> request,
                       const std::string* cache_path,
                       stdx::stop_token stop_token, Owner owner)
    : http_(http),
      event_loop_(event_loop),
      handle_(curl_easy_init()),
      header_list_(),
      request_body_(std::move(request.body)),
      stop_token_(std::move(stop_token)),
      owner_(owner),
      next_request_body_chunk_(),
      stop_callback_(stop_token_, OnCancel{this}) {
  Check(curl_easy_setopt(handle_.get(), CURLOPT_URL, request.url.data()));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_PRIVATE, this));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_WRITEFUNCTION, WriteCallback));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_WRITEDATA, this));
  Check(
      curl_easy_setopt(handle_.get(), CURLOPT_HEADERFUNCTION, HeaderCallback));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_HEADERDATA, this));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_XFERINFOFUNCTION,
                         ProgressCallback));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_XFERINFODATA, this));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_READFUNCTION, ReadCallback));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_READDATA, this));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_NOPROGRESS, 0L));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_SSL_VERIFYPEER, 1L));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_CUSTOMREQUEST,
                         std::string(MethodToString(request.method)).c_str()));
  Check(curl_easy_setopt(handle_.get(), CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_NONE));
  if (cache_path) {
    Check(
        curl_easy_setopt(handle_.get(), CURLOPT_ALTSVC,
                         (*cache_path + PATH_SEPARATOR "alt-svc.txt").c_str()));
  }
#ifdef USE_BUNDLED_CACERT
  curl_blob ca_cert{
      .data = const_cast<void*>(reinterpret_cast<const void*>(kCaCert.data())),
      .len = kCaCert.size()};
  Check(curl_easy_setopt(handle_.get(), CURLOPT_CAINFO_BLOB, &ca_cert));
#endif
  std::optional<curl_off_t> content_length;
  for (const auto& [header_name, header_value] : request.headers) {
    std::string header_line = header_name;
    header_line += ": ";
    header_line += header_value;
    header_list_.reset(
        curl_slist_append(header_list_.release(), header_line.c_str()));
    if (!header_list_) {
      throw HttpException(CURLE_OUT_OF_MEMORY, "curl_slist_append failed");
    }
    if (ToLowerCase(header_name) == "content-length") {
      content_length = std::stoll(header_value);
    }
  }
  Check(
      curl_easy_setopt(handle_.get(), CURLOPT_HTTPHEADER, header_list_.get()));

  if (request_body_) {
    if (request.method == Method::kPost) {
      Check(curl_easy_setopt(handle_.get(), CURLOPT_POST, 1L));
      if (content_length) {
        Check(curl_easy_setopt(handle_.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                               *content_length));
      }
    } else {
      curl_easy_setopt(handle_.get(), CURLOPT_UPLOAD, 1L);
      if (content_length) {
        Check(curl_easy_setopt(handle_.get(), CURLOPT_INFILESIZE_LARGE,
                               *content_length));
      }
    }
    RunTask([d = this]() -> Task<> {
      try {
        d->request_body_it_ = co_await d->request_body_->begin();
        d->request_body_chunk_index_ = 0;
        curl_easy_pause(d->handle_.get(), CURLPAUSE_SEND_CONT);
      } catch (...) {
        d->HandleException(std::current_exception());
      }
    });
  }

  Check(event_assign(&next_request_body_chunk_, event_loop, -1, 0,
                     OnNextRequestBodyChunkRequested, this));

  Check(curl_multi_add_handle(http, handle_.get()));
}

CurlHttpBodyGenerator::CurlHttpBodyGenerator(std::unique_ptr<CurlHandle> handle,
                                             std::string_view initial_chunk)
    : handle_(std::move(handle)) {
  handle_->owner_ = this;
  Check(event_assign(&chunk_ready_, handle_->event_loop_, -1, 0, OnChunkReady,
                     this));
  Check(event_assign(&body_ready_, handle_->event_loop_, -1, 0, OnBodyReady,
                     this));
  ReceivedData(initial_chunk);
}

void CurlHttpBodyGenerator::OnChunkReady(evutil_socket_t, short, void* handle) {
  auto* curl_http_body_generator =
      reinterpret_cast<CurlHttpBodyGenerator*>(handle);
  std::string data = std::move(curl_http_body_generator->data_);
  curl_http_body_generator->data_.clear();
  if (curl_http_body_generator->status_ != -1 &&
      !curl_http_body_generator->body_ready_fired_) {
    curl_http_body_generator->body_ready_fired_ = true;
    evuser_trigger(&curl_http_body_generator->body_ready_);
  }
  curl_http_body_generator->ReceivedData(std::move(data));
}

void CurlHttpBodyGenerator::OnBodyReady(evutil_socket_t, short, void* handle) {
  auto* curl_http_body_generator =
      reinterpret_cast<CurlHttpBodyGenerator*>(handle);
  if (curl_http_body_generator->exception_ptr_) {
    curl_http_body_generator->Close(curl_http_body_generator->exception_ptr_);
  } else {
    curl_http_body_generator->Close(curl_http_body_generator->status_);
  }
}

CurlHttpBodyGenerator::~CurlHttpBodyGenerator() noexcept {
  if (chunk_ready_.ev_base) {
    Check(event_del(&chunk_ready_));
    Check(event_del(&body_ready_));
  }
}

void CurlHttpBodyGenerator::Resume() {
  if (status_ == -1 && !exception_ptr_) {
    curl_easy_pause(handle_->handle_.get(), CURLPAUSE_RECV_CONT);
  }
}

CurlHttpOperation::CurlHttpOperation(CURLM* http, event_base* event_loop,
                                     Request<> request,
                                     const std::string* cache_directory,
                                     stdx::stop_token stop_token)
    : headers_ready_(),
      headers_ready_event_posted_(),
      handle_(std::make_unique<CurlHandle>(http, event_loop, std::move(request),
                                           cache_directory,
                                           std::move(stop_token), this)) {
  Check(event_assign(&headers_ready_, handle_->event_loop_, -1, 0,
                     OnHeadersReady, this));
}

CurlHttpOperation::~CurlHttpOperation() {
  if (headers_ready_.ev_base) {
    event_del(&headers_ready_);
  }
}

void CurlHttpOperation::OnHeadersReady(evutil_socket_t, short, void* handle) {
  auto* http_operation = reinterpret_cast<CurlHttpOperation*>(handle);
  if (http_operation->awaiting_coroutine_) {
    std::exchange(http_operation->awaiting_coroutine_, nullptr).resume();
  }
}

bool CurlHttpOperation::await_ready() {
  return exception_ptr_ || status_ != -1;
}

void CurlHttpOperation::await_suspend(
    stdx::coroutine_handle<void> awaiting_coroutine) {
  awaiting_coroutine_ = awaiting_coroutine;
}

std::unique_ptr<Response<CurlHttpBodyGenerator>>
CurlHttpOperation::await_resume() {
  if (exception_ptr_) {
    std::rethrow_exception(exception_ptr_);
  }
  std::unique_ptr<Response<CurlHttpBodyGenerator>> response(
      new Response<CurlHttpBodyGenerator>{
          .status = status_,
          .headers = std::move(headers_),
          .body = {std::move(handle_), std::move(body_)}});
  if (no_body_) {
    response->body.Close(status_);
  }
  return response;
}

CurlHttpImpl::CurlHttpImpl(event_base* event_loop,
                           std::optional<std::string> cache_path)
    : curl_handle_(curl_multi_init()),
      event_loop_(event_loop),
      timeout_event_(),
      cache_path_(std::move(cache_path)) {
  event_assign(&timeout_event_, event_loop, -1, 0, TimeoutEvent,
               curl_handle_.get());
  Check(curl_multi_setopt(curl_handle_.get(), CURLMOPT_SOCKETFUNCTION,
                          SocketCallback));
  Check(curl_multi_setopt(curl_handle_.get(), CURLMOPT_TIMERFUNCTION,
                          TimerCallback));
  Check(curl_multi_setopt(curl_handle_.get(), CURLMOPT_SOCKETDATA, this));
  Check(curl_multi_setopt(curl_handle_.get(), CURLMOPT_TIMERDATA, this));
}

CurlHttpImpl::~CurlHttpImpl() { event_del(&timeout_event_); }

void CurlHttpImpl::TimeoutEvent(evutil_socket_t, short, void* handle) {
  int running_handles;
  Check(curl_multi_socket_action(handle, CURL_SOCKET_TIMEOUT, 0,
                                 &running_handles));
  ProcessEvents(handle);
}

void CurlHttpImpl::ProcessEvents(CURLM* multi_handle) {
  CURLMsg* message;
  do {
    int message_count;
    message = curl_multi_info_read(multi_handle, &message_count);
    if (message && message->msg == CURLMSG_DONE) {
      CURL* handle = message->easy_handle;
      CurlHandle* data;
      Check(curl_easy_getinfo(handle, CURLINFO_PRIVATE, &data));
      if (std::holds_alternative<CurlHttpOperation*>(data->owner_)) {
        auto* operation = std::get<CurlHttpOperation*>(data->owner_);
        if (message->data.result == CURLE_OK) {
          long response_code;
          Check(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE,
                                  &response_code));
          operation->status_ = static_cast<int>(response_code);
        } else {
          operation->exception_ptr_ = std::make_exception_ptr(HttpException(
              message->data.result, curl_easy_strerror(message->data.result)));
        }
        operation->no_body_ = true;
        Check(event_base_once(
            operation->handle_->event_loop_, -1, EV_TIMEOUT,
            [](evutil_socket_t, short, void* handle) {
              stdx::coroutine_handle<void>::from_address(handle).resume();
            },
            std::exchange(operation->awaiting_coroutine_, nullptr).address(),
            nullptr));
      } else if (std::holds_alternative<CurlHttpBodyGenerator*>(data->owner_)) {
        auto* curl_http_body_generator =
            std::get<CurlHttpBodyGenerator*>(data->owner_);
        curl_http_body_generator->status_ =
            static_cast<int>(message->data.result);
        if (message->data.result != CURLE_OK) {
          curl_http_body_generator->exception_ptr_ = std::make_exception_ptr(
              HttpException(message->data.result,
                            curl_easy_strerror(message->data.result)));
        }
        if (!(curl_http_body_generator->chunk_ready_.ev_evcallback.evcb_flags &
              EVLIST_ACTIVE)) {
          curl_http_body_generator->body_ready_fired_ = true;
          evuser_trigger(&curl_http_body_generator->body_ready_);
        }
      }
    }
  } while (message != nullptr);
}

void CurlHttpImpl::SocketEvent(evutil_socket_t fd, short event, void* handle) {
  int running_handles;
  Check(
      curl_multi_socket_action(handle, fd,
                               ((event & EV_READ) ? CURL_CSELECT_IN : 0) |
                                   ((event & EV_WRITE) ? CURL_CSELECT_OUT : 0),
                               &running_handles));
  ProcessEvents(handle);
}

int CurlHttpImpl::SocketCallback(CURL*, curl_socket_t socket, int what,
                                 void* userp, void* socketp) {
  auto* http = reinterpret_cast<CurlHttpImpl*>(userp);
  if (what == CURL_POLL_REMOVE) {
    auto* data = reinterpret_cast<SocketData*>(socketp);
    if (data) {
      Check(event_del(&data->socket_event));
      delete data;
    }
  } else {
    auto* data = reinterpret_cast<SocketData*>(socketp);
    if (!data) {
      data = new SocketData;
      Check(curl_multi_assign(http->curl_handle_.get(), socket, data));
    } else {
      Check(event_del(&data->socket_event));
    }
    Check(event_assign(
        &data->socket_event, http->event_loop_, socket,
        static_cast<short>(((what & CURL_POLL_IN) ? EV_READ : 0) |
                           ((what & CURL_POLL_OUT) ? EV_WRITE : 0) |
                           EV_PERSIST),
        SocketEvent, http->curl_handle_.get()));
    Check(event_add(&data->socket_event, nullptr));
  }
  return 0;
}

int CurlHttpImpl::TimerCallback(CURLM*, long timeout_ms, void* userp) {
  auto* http = reinterpret_cast<CurlHttpImpl*>(userp);
  if (timeout_ms == -1) {
    Check(event_del(&http->timeout_event_));
  } else {
    timeval tv = {
        .tv_sec = static_cast<decltype(tv.tv_sec)>(timeout_ms / 1000),
        .tv_usec = static_cast<decltype(tv.tv_usec)>(timeout_ms % 1000 * 1000)};
    Check(event_add(&http->timeout_event_, &tv));
  }
  return 0;
}

CurlHttpOperation CurlHttpImpl::Fetch(Request<> request,
                                      stdx::stop_token token) const {
  return {curl_handle_.get(), event_loop_, std::move(request),
          cache_path_ ? &*cache_path_ : nullptr, std::move(token)};
}

void CurlHttpImpl::CurlMultiDeleter::operator()(CURLM* handle) const {
  if (handle) {
    curl_multi_cleanup(handle);
  }
}

Generator<std::string> ToBody(
    std::unique_ptr<Response<CurlHttpBodyGenerator>> d) {
  FOR_CO_AWAIT(std::string & chunk, d->body) { co_yield std::move(chunk); }
}

}  // namespace

struct CurlHttpBase::Impl {
  CurlHttpImpl impl;
};

CurlHttpBase::CurlHttpBase(const coro::util::EventLoop* event_loop,
                           std::optional<std::string> cache_path)
    : d_(new Impl{
          {reinterpret_cast<struct event_base*>(GetEventLoop(*event_loop)),
           cache_path}}) {}

CurlHttpBase::~CurlHttpBase() = default;

Task<Response<>> CurlHttpBase::Fetch(Request<> request,
                                     stdx::stop_token stop_token) const {
  auto response =
      co_await d_->impl.Fetch(std::move(request), std::move(stop_token));
  auto status = response->status;
  auto headers = std::move(response->headers);
  co_return Response<>{.status = status,
                       .headers = std::move(headers),
                       .body = ToBody(std::move(response))};
}

}  // namespace coro::http
