// include/socrates/transport/transport.h
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::transport {

enum class TransportMethod { kGrpcMtls, kUsb, kQuic, kWebRtc };

struct TransportConfig {
  std::string listen_address;
  std::uint32_t listen_port{0};
  std::string tls_certificate_pem;
  std::string tls_private_key_pem;
  std::string tls_trust_bundle_pem;
  std::size_t max_message_bytes{64 * 1024 * 1024};
  std::chrono::milliseconds default_deadline{30000};
  std::chrono::milliseconds keepalive_interval{10000};
  std::chrono::milliseconds keepalive_timeout{5000};
  bool keepalive_permit_without_calls{true};
  std::uint32_t max_concurrent_streams{100};
  std::uint32_t max_receive_queue_size{256};
};

struct UnaryRequest {
  std::string method;
  Bytes payload;
  std::string target_node_id;
  Deadline deadline;
  std::uint64_t sequence_number{0};
};

struct UnaryResponse {
  Bytes payload;
  std::chrono::microseconds elapsed;
};

struct StreamFrame {
  std::string stream_id;
  std::uint64_t sequence_number{0};
  Bytes payload;
  bool is_last{false};
};

using UnaryHandler = std::function<Result<UnaryResponse>(const UnaryRequest&)>;
using StreamHandler = std::function<void(const StreamFrame&)>;
using StreamWriter = std::function<Result<bool>(const StreamFrame&)>;

class Transport {
 public:
  virtual ~Transport() = default;

  /**
   * Starts the transport server, accepting authenticated connections.
   * Preconditions: config contains valid TLS material, a listen address, and a
   * non-zero port; service is stopped.
   * Postconditions: on success, registered handlers are invoked for incoming
   * requests; partial failure rolls back and leaves the server stopped.
   * Throws: RuntimeError(kInvalidArgument, kPermissionDenied, kInternal).
   * Thread safety: start/stop are serialized; handler invocations may be
   * concurrent across calls but serialized per stream.
   * Side effects: binds a socket, loads TLS credentials, starts I/O workers.
   */
  virtual void start(const TransportConfig& config) = 0;

  /**
   * Registers a handler for a named unary RPC method.
   * Preconditions: service is stopped.
   * Postconditions: registered handlers are invoked for matching incoming calls.
   * Throws: RuntimeError(kAlreadyExists) on duplicate method names.
   * Thread safety: not safe to call while serving.
   * Side effects: none.
   */
  virtual void register_unary_handler(std::string method, UnaryHandler handler) = 0;

  /**
   * Registers a handler for inbound streaming frames on a stream.
   * Preconditions: service is stopped.
   * Postconditions: handler receives ordered, deduplicated frames.
   * Throws: RuntimeError(kAlreadyExists) on duplicate stream names.
   * Thread safety: not safe to call while serving.
   * Side effects: none.
   */
  virtual void register_stream_handler(std::string stream_id,
                                        StreamHandler handler) = 0;

  /**
   * Sends a unary request to a remote node and returns the response.
   * Preconditions: service is started; target is reachable.
   * Postconditions: success returns the decoded response within deadline.
   * Throws: no operational exceptions; Result reports timeout, unavailable,
   * cancelled, and transport errors.
   * Thread safety: safe for concurrent calls to distinct targets.
   * Side effects: opens a client connection if none exists; sends network I/O.
   */
  virtual Result<UnaryResponse> send_unary(const std::string& target_address,
                                            const UnaryRequest& request) = 0;

  /**
   * Opens an ordered bidirectional stream to a remote node.
   * Preconditions: service is started; target is reachable.
   * Postconditions: writer frames are delivered in order; handler receives
   * incoming frames in sequence order with bounded queue backpressure.
   * Throws: no operational exceptions; Result reports connection failure.
   * Thread safety: the returned writer is safe for serialized use; handler
   * invocations are serialized for this stream.
   * Side effects: opens a long-lived bidirectional connection.
   */
  virtual Result<StreamWriter> open_stream(const std::string& target_address,
                                            const std::string& stream_id,
                                            StreamHandler handler) = 0;

  /**
   * Broadcasts a unary request to every live member in the membership.
   * Preconditions: service is started; membership snapshot is current.
   * Postconditions: each live node receives exactly one copy; duplicates are
   * rejected by the receiver based on (node_id, sequence_number) dedup.
   * Throws: no operational exceptions; partial failures do not fail the call;
   * per-node errors are logged and aggregated.
   * Thread safety: safe for concurrent calls.
   * Side effects: sends N unary requests to N live members.
   */
  virtual Result<std::vector<UnaryResponse>> broadcast(
      const std::vector<std::string>& member_addresses,
      const UnaryRequest& request) = 0;

  /**
   * Gracefully stops the transport, draining or cancelling active streams.
   * Preconditions: none.
   * Postconditions: no new calls are accepted and all I/O workers are stopped.
   * Throws: never.
   * Thread safety: idempotent; safe to call from any thread.
   * Side effects: closes all connections, releases ports and file descriptors.
   */
  virtual void stop() noexcept = 0;
};

std::unique_ptr<Transport> make_grpc_transport();

}  // namespace socrates::transport
