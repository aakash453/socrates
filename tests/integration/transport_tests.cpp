#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "socrates/transport/transport.h"
#include "socrates/transport/tensor_codec.h"

namespace socrates {
namespace {

using namespace transport;

TEST(Transport, GrpcStartStop) {
  auto t = make_grpc_transport();
  TransportConfig cfg;
  cfg.listen_address = "127.0.0.1";
  cfg.listen_port = 0;
  cfg.tls_certificate_pem = "fake-cert";
  cfg.tls_private_key_pem = "fake-key";

  EXPECT_NO_THROW(t->start(cfg));
  EXPECT_NO_THROW(t->stop());
}

TEST(Transport, DoubleStartThrows) {
  auto t = make_grpc_transport();
  TransportConfig cfg;
  cfg.listen_address = "127.0.0.1";
  cfg.listen_port = 0;
  cfg.tls_certificate_pem = "fake";

  t->start(cfg);
  EXPECT_THROW(t->start(cfg), RuntimeError);
  t->stop();
}

TEST(Transport, RegisterUnaryHandler) {
  auto t = make_grpc_transport();
  t->register_unary_handler("Echo", [](const UnaryRequest& req) -> Result<UnaryResponse> {
    UnaryResponse resp;
    resp.payload = req.payload;
    resp.elapsed = std::chrono::microseconds(100);
    return resp;
  });
}

TEST(Transport, RegisterDuplicateHandlerThrows) {
  auto t = make_grpc_transport();
  t->register_unary_handler("Dup", [](const auto&) -> Result<UnaryResponse> {
    return UnaryResponse{};
  });
  EXPECT_THROW(
      t->register_unary_handler("Dup", [](const auto&) -> Result<UnaryResponse> {
        return UnaryResponse{};
      }),
      RuntimeError);
}

TEST(Transport, StreamDeduplicationDropsOutOfOrder) {
  auto t = make_grpc_transport();
  TransportConfig cfg;
  cfg.listen_address = "127.0.0.1";
  cfg.listen_port = 0;
  cfg.tls_certificate_pem = "fake";

  t->start(cfg);

  std::vector<std::uint64_t> received_seqs;
  auto handler = [&](const StreamFrame& f) {
    received_seqs.push_back(f.sequence_number);
  };

  auto writer_result = t->open_stream("node-a:5000", "hidden-states", handler);
  ASSERT_TRUE(writer_result.is_ok());

  // Inject frames in order
  auto* grpc = dynamic_cast<GrpcTransport*>(t.get());
  ASSERT_NE(grpc, nullptr);

  StreamFrame f1{"hidden-states", 1, {}, false};
  StreamFrame f2{"hidden-states", 2, {}, false};
  StreamFrame f3{"hidden-states", 1, {}, false};  // duplicate — should be dropped
  StreamFrame f4{"hidden-states", 3, {}, true};

  grpc->inject_incoming_frame("node-a:5000", "hidden-states", f1);
  grpc->inject_incoming_frame("node-a:5000", "hidden-states", f2);
  grpc->inject_incoming_frame("node-a:5000", "hidden-states", f3);
  grpc->inject_incoming_frame("node-a:5000", "hidden-states", f4);

  ASSERT_EQ(received_seqs.size(), 3u);
  EXPECT_EQ(received_seqs[0], 1u);
  EXPECT_EQ(received_seqs[1], 2u);
  EXPECT_EQ(received_seqs[2], 3u);

  t->stop();
}

TEST(TensorCodec, RoundtripIntact) {
  TensorCodec codec;
  Tensor orig;
  orig.spec.element_type = ElementType::kFloat32;
  orig.spec.shape.dims = {2, 4, 8};
  orig.spec.maximum_encoded_bytes = 0;
  orig.data.assign(sizeof(float) * 64, std::byte{0xAB});

  auto encoded = codec.encode(orig);
  ASSERT_TRUE(encoded.is_ok());

  auto decoded = codec.decode(encoded.value());
  ASSERT_TRUE(decoded.is_ok());

  EXPECT_EQ(decoded.value().spec.element_type, orig.spec.element_type);
  EXPECT_EQ(decoded.value().spec.shape.dims, orig.spec.shape.dims);
  EXPECT_EQ(decoded.value().data, orig.data);
}

TEST(TensorCodec, RejectsTruncatedFrame) {
  TensorCodec codec;
  std::vector<std::byte> truncated(4, std::byte{0});
  auto result = codec.decode(truncated);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.error_code(), ErrorCode::kDataLoss);
}

TEST(TensorCodec, RejectsTamperedData) {
  TensorCodec codec;
  Tensor orig;
  orig.spec.element_type = ElementType::kFloat16;
  orig.spec.shape.dims = {1};
  orig.data.assign(2, std::byte{0xCD});

  auto encoded = codec.encode(orig);
  ASSERT_TRUE(encoded.is_ok());

  // Tamper with body byte
  encoded.value().back() = std::byte{0xFF};

  auto decoded = codec.decode(encoded.value());
  ASSERT_TRUE(decoded.is_err());
  EXPECT_EQ(decoded.error_code(), ErrorCode::kDataLoss);
}

}  // namespace
}  // namespace socrates
