// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_HTTP_HTTP_STREAM_POOL_JOB_H_
#define NET_HTTP_HTTP_STREAM_POOL_JOB_H_

#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "base/containers/unique_ptr_adapters.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/base/completion_once_callback.h"
#include "net/base/load_states.h"
#include "net/base/load_timing_info.h"
#include "net/base/net_error_details.h"
#include "net/base/priority_queue.h"
#include "net/base/request_priority.h"
#include "net/dns/host_resolver.h"
#include "net/dns/public/resolve_error_info.h"
#include "net/http/http_stream_pool.h"
#include "net/http/http_stream_request.h"
#include "net/log/net_log_with_source.h"
#include "net/socket/connection_attempts.h"
#include "net/socket/stream_attempt.h"
#include "net/socket/stream_socket_handle.h"
#include "net/socket/tls_stream_attempt.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_versions.h"
#include "url/gurl.h"

namespace net {

class HttpNetworkSession;
class NetLog;
class HttpStreamKey;

// Maintains in-flight HTTP stream requests. Peforms DNS resolution.
class HttpStreamPool::Job
    : public HostResolver::ServiceEndpointRequest::Delegate,
      public TlsStreamAttempt::SSLConfigProvider {
 public:
  // Time to delay connection attempts more than one when the destination is
  // known to support HTTP/2, to avoid unnecessary socket connection
  // establishments. See https://crbug.com/718576
  static constexpr base::TimeDelta kSpdyThrottleDelay = base::Milliseconds(300);

  // `group` must outlive `this`.
  Job(Group* group, NetLog* net_log);

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  ~Job() override;

  Group* group() { return group_; }

  HostResolver::ServiceEndpointRequest* service_endpoint_request() {
    return service_endpoint_request_.get();
  }

  bool is_service_endpoint_request_finished() const {
    return service_endpoint_request_finished_;
  }

  base::TimeTicks dns_resolution_start_time() const {
    return dns_resolution_start_time_;
  }

  base::TimeTicks dns_resolution_end_time() const {
    return dns_resolution_end_time_;
  }

  const NetLogWithSource& net_log();

  // Creates an HttpStreamRequest. Will call delegate's methods. See the
  // comments of HttpStreamRequest::Delegate methods for details.
  std::unique_ptr<HttpStreamRequest> RequestStream(
      HttpStreamRequest::Delegate* delegate,
      RequestPriority priority,
      const std::vector<SSLConfig::CertAndStatus>& allowed_bad_certs,
      bool enable_ip_based_pooling,
      bool enable_alternative_services,
      quic::ParsedQuicVersion quic_version,
      const NetLogWithSource& net_log);

  // Creates idle streams or sessions for `num_streams` be opened.
  // Note that this method finishes synchronously, or `callback` is called, once
  // `this` has enough streams/sessions for `num_streams` be opened. This means
  // that when there are two preconnect requests with `num_streams = 1`, all
  // callbacks are invoked when one stream/session is established (not two).
  int Preconnect(size_t num_streams,
                 quic::ParsedQuicVersion quic_version,
                 CompletionOnceCallback callback);

  // HostResolver::ServiceEndpointRequest::Delegate implementation:
  void OnServiceEndpointsUpdated() override;
  void OnServiceEndpointRequestFinished(int rv) override;

  // TlsStreamAttempt::SSLConfigProvider implementation:
  int WaitForSSLConfigReady(CompletionOnceCallback callback) override;
  SSLConfig GetSSLConfig() override;

  // Tries to process a pending request.
  void ProcessPendingRequest();

  // Returns the number of total requests in this job.
  size_t RequestCount() const { return requests_.size(); }

  // Returns the number of in-flight attempts.
  size_t InFlightAttemptCount() const { return in_flight_attempts_.size(); }

  // Cancels all in-flight attempts.
  void CancelInFlightAttempts();

  // Cancels all requests.
  void CancelRequests(int error);

  // Returns the number of pending requests/preconnects. The number is
  // calculated by subtracting the number of in-flight attempts (excluding slow
  // attempts) from the number of total requests.
  size_t PendingRequestCount() const;
  size_t PendingPreconnectCount() const;

  // Returns the highest priority in `requests_`.
  RequestPriority GetPriority() const;

  // Returns true when `this` is blocked by the pool's stream limit.
  bool IsStalledByPoolLimit();

  // Called when the server required HTTP/1.1. Clears the current SPDY session
  // if exists. Subsequent requests will fail while `this` is alive.
  void OnRequiredHttp11();

  // Called when the QuicTask owned by `this` is completed.
  void OnQuicTaskComplete(int rv);

  std::optional<int> GetQuicTaskResultForTesting() { return quic_task_result_; }

 private:
  // Represents failure of connection attempts. Used to call request's delegate
  // methods.
  enum class FailureKind {
    kStreamFailed,
    kCertifcateError,
    kNeedsClientAuth,
  };

  // Represents reasons if future connection attempts could be blocked or not.
  enum class CanAttemptResult {
    kAttempt,
    kNoPendingRequest,
    kBlockedStreamAttempt,
    kThrottledForSpdy,
    kReachedGroupLimit,
    kReachedPoolLimit,
  };

  // A peer of an HttpStreamRequest. Holds the HttpStreamRequest's delegate
  // pointer and implements HttpStreamRequest::Helper.
  class RequestEntry : public HttpStreamRequest::Helper {
   public:
    explicit RequestEntry(Job* job);

    RequestEntry(RequestEntry&) = delete;
    RequestEntry& operator=(const RequestEntry&) = delete;

    ~RequestEntry() override;

    std::unique_ptr<HttpStreamRequest> CreateRequest(
        HttpStreamRequest::Delegate* delegate,
        const NetLogWithSource& net_log);

    HttpStreamRequest* request() const { return request_; }

    HttpStreamRequest::Delegate* delegate() const { return delegate_; }

    // HttpStreamRequest::Helper methods:
    LoadState GetLoadState() const override;
    void OnRequestComplete() override;
    int RestartTunnelWithProxyAuth() override;
    void SetPriority(RequestPriority priority) override;

   private:
    const raw_ptr<Job> job_;
    raw_ptr<HttpStreamRequest> request_;
    raw_ptr<HttpStreamRequest::Delegate> delegate_;
  };

  using RequestQueue = PriorityQueue<std::unique_ptr<RequestEntry>>;

  struct InFlightAttempt;
  struct PreconnectEntry;

  const HttpStreamKey& stream_key() const;

  const SpdySessionKey& spdy_session_key() const;

  const QuicSessionKey& quic_session_key() const;

  HttpNetworkSession* http_network_session();
  SpdySessionPool* spdy_session_pool();
  QuicSessionPool* quic_session_pool();

  HttpStreamPool* pool();
  const HttpStreamPool* pool() const;

  bool UsingTls() const;

  bool RequiresHTTP11();

  // Returns the current load state.
  LoadState GetLoadState() const;

  void StartInternal(RequestPriority priority);

  void ResolveServiceEndpoint(RequestPriority initial_priority);

  void MaybeChangeServiceEndpointRequestPriority();

  // Called when service endpoint results have changed or finished.
  void ProcessServiceEndpointChanges();

  // Returns true when there is an active SPDY session that can be used for
  // on-going requests after service endpoint results has changed. May notify
  // requests of stream ready.
  bool CanUseExistingSessionAfterEndpointChanges();

  // Runs the stream attempt delay timer if stream attempts are blocked and the
  // timer is not running.
  void MaybeRunStreamAttemptDelayTimer();

  // Calculate SSLConfig if it's not calculated yet and `this` has received
  // enough information to calculate it.
  void MaybeCalculateSSLConfig();

  // Attempts QUIC sessions if QUIC can be used and `this` is ready to start
  // cryptographic connection handshakes.
  void MaybeAttemptQuic();

  // Attempts connections if there are pending requests and IPEndPoints that
  // haven't failed. If `max_attempts` is given, attempts connections up to
  // `max_attempts`.
  void MaybeAttemptConnection(
      std::optional<size_t> max_attempts = std::nullopt);

  // Returns true if there are pending requests and the pool and the group
  // haven't reached stream limits. If the pool reached the stream limit, may
  // close idle sockets in other groups. Also may cancel preconnects or trigger
  // `spdy_throttle_timer_`.
  bool IsConnectionAttemptReady();

  // Actual implementation of IsConnectionAttemptReady(), without having side
  // effects.
  CanAttemptResult CanAttemptConnection();

  // Returns true when connection attempts should be throttled because there is
  // an in-flight attempt and the destination is known to support HTTP/2.
  bool ShouldThrottleAttemptForSpdy();

  // Helper method to calculate pending requests/preconnects.
  size_t PendingCountInternal(size_t pending_count) const;

  std::optional<IPEndPoint> GetIPEndPointToAttempt();
  std::optional<IPEndPoint> FindPreferredIPEndpoint(
      const std::vector<IPEndPoint>& ip_endpoints);

  // Calculate the failure kind to notify requests of failure. Used to call
  // one of the delegate's methods.
  FailureKind DetermineFailureKind();

  // Notifies a failure to all requests.
  void NotifyFailure();

  // Notifies a failure to a single request. Used by NotifyFailure().
  void NotifyStreamRequestOfFailure();

  // Notifies all preconnects of completion.
  void NotifyPreconnectsComplete(int rv);

  // Called after completion of a connection attempt to decriment stream
  // counts in preconnect entries. Invokes the callback of an entry when the
  // entry's stream counts becomes zero (i.e., `this` has enough streams).
  void ProcessPreconnectsAfterAttemptComplete(int rv);

  // Creates a text based stream and notifies the highest priority request.
  void CreateTextBasedStreamAndNotify(
      std::unique_ptr<StreamSocket> stream_socket,
      StreamSocketHandle::SocketReuseType reuse_type,
      LoadTimingInfo::ConnectTiming connect_timing);

  void CreateSpdyStreamAndNotify();

  void CreateQuicStreamAndNotify();

  void NotifyStreamReady(std::unique_ptr<HttpStream> stream,
                         NextProto negotiated_protocol);

  // Extracts an entry from `requests_` of which priority is highest. The
  // ownership of the entry is moved to `notified_requests_`.
  RequestEntry* ExtractFirstRequestToNotify();

  // Called when the priority of `request` is set.
  void SetRequestPriority(HttpStreamRequest* request, RequestPriority priority);

  // Called when an HttpStreamRequest associated with `entry` is going to
  // be destroyed.
  void OnRequestComplete(RequestEntry* entry);

  void OnInFlightAttemptComplete(InFlightAttempt* raw_attempt, int rv);
  void OnInFlightAttemptTcpHandshakeComplete(InFlightAttempt* raw_attempt,
                                             int rv);
  void OnInFlightAttemptSlow(InFlightAttempt* raw_attempt);

  void HandleAttemptFailure(std::unique_ptr<InFlightAttempt> in_flight_attempt,
                            int rv);

  void OnSpdyThrottleDelayPassed();

  // Returns the delay for TCP-based stream attempts in favor of QUIC.
  base::TimeDelta GetStreamAttemptDelay();

  // Updates whether stream attempts should be blocked or not. May cancel
  // `stream_attempt_delay_timer_`.
  void UpdateStreamAttemptState();

  // Called when `stream_attempt_delay_timer_` is fired.
  void OnStreamAttemptDelayPassed();

  bool CanUseQuic();

  bool CanUseExistingQuicSession();

  void MaybeComplete();

  const raw_ptr<Group> group_;

  const NetLogWithSource net_log_;

  ProxyInfo proxy_info_;

  bool enable_ip_based_pooling_ = true;

  bool enable_alternative_services_ = true;

  // Holds requests that are waiting for notifications (a delegate method call
  // to indicate success or failure).
  RequestQueue requests_;
  // Holds requests that are already notified results. We need to keep them
  // to avoid dangling pointers.
  std::set<std::unique_ptr<RequestEntry>, base::UniquePtrComparator>
      notified_requests_;

  // Holds preconnect requests.
  std::set<std::unique_ptr<PreconnectEntry>, base::UniquePtrComparator>
      preconnects_;

  std::unique_ptr<HostResolver::ServiceEndpointRequest>
      service_endpoint_request_;
  bool service_endpoint_request_finished_ = false;
  base::TimeTicks dns_resolution_start_time_;
  base::TimeTicks dns_resolution_end_time_;

  // Set to true when `this` cannot handle further requests. Used to ensure that
  // `this` doesn't accept further requests while notifying the failure to the
  // existing requests.
  bool is_failing_ = false;

  // Set to true when `CancelRequests()` is called.
  bool is_canceling_requests_ = false;

  NetErrorDetails net_error_details_;
  ResolveErrorInfo resolve_error_info_;
  ConnectionAttempts connection_attempts_;

  // Set to an error from the latest stream attempt failure or network change
  // events. Used to notify delegates when all attempts failed.
  int error_to_notify_ = ERR_FAILED;

  // Set to a SSLInfo when an attempt has failed with a certificate error. Used
  // to notify requests.
  std::optional<SSLInfo> cert_error_ssl_info_;

  // Set to a SSLCertRequestInfo when an attempt has requested a client cert.
  // Used to notify requests.
  scoped_refptr<SSLCertRequestInfo> client_auth_cert_info_;

  // Allowed bad certificates from the newest request.
  std::vector<SSLConfig::CertAndStatus> allowed_bad_certs_;
  // SSLConfig for all TLS connection attempts. Calculated after the service
  // endpoint request is ready to proceed cryptographic handshakes.
  // TODO(crbug.com/40812426): We need to have separate SSLConfigs when we
  // support multiple HTTPS RR that have different service endpoints.
  std::optional<SSLConfig> ssl_config_;
  std::vector<CompletionOnceCallback> ssl_config_waiting_callbacks_;

  std::set<std::unique_ptr<InFlightAttempt>, base::UniquePtrComparator>
      in_flight_attempts_;
  // The number of in-flight attempts that are treated as slow.
  size_t slow_attempt_count_ = 0;

  base::OneShotTimer spdy_throttle_timer_;
  bool spdy_throttle_delay_passed_ = false;

  // When true, try to use IPv6 for the next attempt first.
  bool prefer_ipv6_ = true;
  // Updated when a stream attempt failed. Used to calculate next IPEndPoint to
  // attempt.
  std::set<IPEndPoint> failed_ip_endpoints_;
  // Updated when a stream attempt is considered slow. Used to calculate next
  // IPEndPoint to attempt.
  std::set<IPEndPoint> slow_ip_endpoints_;

  // Initialized when one of an attempt is negotiated to use HTTP/2.
  base::WeakPtr<SpdySession> spdy_session_;

  // QUIC version that is known to be used for the destination, usually coming
  // from Alt-Svc.
  quic::ParsedQuicVersion quic_version_ =
      quic::ParsedQuicVersion::Unsupported();
  // Created when attempting QUIC sessions.
  std::unique_ptr<QuicTask> quic_task_;
  // Set when `quic_task_` is completed.
  std::optional<int> quic_task_result_;

  // The delay for TCP-based stream attempts in favor of QUIC.
  base::TimeDelta stream_attempt_delay_;
  // Set to true when stream attempts should be blocked.
  bool should_block_stream_attempt_ = false;
  base::OneShotTimer stream_attempt_delay_timer_;

  base::WeakPtrFactory<Job> weak_ptr_factory_{this};
};

}  // namespace net

#endif  // NET_HTTP_HTTP_STREAM_POOL_JOB_H_