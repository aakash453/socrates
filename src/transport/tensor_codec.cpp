// src/transport/tensor_codec.cpp
// Wire-format tensor envelope with SHA-256 validation.

#include "socrates/transport/tensor_codec.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#if SOCRATES_HAS_OPENSSL
#include <openssl/evp.h>
#endif

namespace socrates::transport {

namespace {

// SHA-256 computation.
// Uses OpenSSL EVP when available; falls back to a simple hash otherwise.
Sha256Digest compute_sha256(const std::byte* data, std::size_t len) {
  Sha256Digest d{};

#if SOCRATES_HAS_OPENSSL
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return d;

  unsigned int digest_len = 0;
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data, len);
  EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(d.data()),
                     &digest_len);
  EVP_MD_CTX_free(ctx);
#else
  // Fallback: deterministic pseudo-hash for platforms without OpenSSL
  std::uint32_t h = 0;
  for (std::size_t i = 0; i < len; ++i) {
    h = h * 31 + static_cast<std::uint32_t>(data[i]);
    d[i % 32] = static_cast<std::byte>(h & 0xFF);
  }
#endif

  return d;
}

void write_uint32(std::byte* dst, std::uint32_t val) {
  dst[0] = static_cast<std::byte>(val & 0xFF);
  dst[1] = static_cast<std::byte>((val >> 8) & 0xFF);
  dst[2] = static_cast<std::byte>((val >> 16) & 0xFF);
  dst[3] = static_cast<std::byte>((val >> 24) & 0xFF);
}

std::uint32_t read_uint32(const std::byte* src) {
  return static_cast<std::uint32_t>(src[0]) |
         (static_cast<std::uint32_t>(src[1]) << 8) |
         (static_cast<std::uint32_t>(src[2]) << 16) |
         (static_cast<std::uint32_t>(src[3]) << 24);
}

void write_uint64(std::byte* dst, std::uint64_t val) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<std::byte>((val >> (i * 8)) & 0xFF);
  }
}

std::uint64_t read_uint64(const std::byte* src) {
  std::uint64_t val = 0;
  for (int i = 0; i < 8; ++i) {
    val |= static_cast<std::uint64_t>(src[i]) << (i * 8);
  }
  return val;
}

}  // namespace

Result<std::vector<std::byte>> TensorCodec::encode(const Tensor& tensor) const {
  if (tensor.data.empty()) {
    return Result<std::vector<std::byte>>::Err(ErrorCode::kInvalidArgument,
                                                "cannot encode empty tensor");
  }

  auto total = kHeaderSize + tensor.data.size();
  if (tensor.spec.maximum_encoded_bytes > 0 && total > tensor.spec.maximum_encoded_bytes) {
    return Result<std::vector<std::byte>>::Err(ErrorCode::kResourceExhausted,
                                                "tensor exceeds maximum_encoded_bytes");
  }

  std::vector<std::byte> wire(total);
  auto* hdr = wire.data();
  auto* body = wire.data() + kHeaderSize;

  // Header
  write_uint32(hdr + 0, kHeaderMagic);
  write_uint32(hdr + 4, static_cast<std::uint32_t>(tensor.spec.element_type));
  write_uint64(hdr + 8, static_cast<std::uint64_t>(tensor.spec.shape.dims.size()));
  std::size_t off = 16;
  for (auto dim : tensor.spec.shape.dims) {
    write_uint64(hdr + off, static_cast<std::uint64_t>(dim));
    off += 8;
  }
  write_uint64(hdr + 56, static_cast<std::uint64_t>(tensor.data.size()));

  // Body
  std::memcpy(body, tensor.data.data(), tensor.data.size());

  // SHA-256 over body only
  auto digest = compute_sha256(body, tensor.data.size());
  std::memcpy(hdr + 32, digest.data(), 32);

  return wire;
}

Result<Tensor> TensorCodec::decode(const std::vector<std::byte>& wire_bytes) const {
  if (wire_bytes.size() < kHeaderSize) {
    return Result<Tensor>::Err(ErrorCode::kDataLoss, "frame too small for header");
  }

  auto* hdr = wire_bytes.data();
  if (read_uint32(hdr) != kHeaderMagic) {
    return Result<Tensor>::Err(ErrorCode::kDataLoss, "invalid magic number");
  }

  std::uint64_t data_size = read_uint64(hdr + 56);
  if (kHeaderSize + data_size > wire_bytes.size()) {
    return Result<Tensor>::Err(ErrorCode::kDataLoss, "truncated payload");
  }

  auto* body = wire_bytes.data() + kHeaderSize;

  // Verify checksum
  Sha256Digest expected, actual;
  std::memcpy(expected.data(), hdr + 32, 32);
  actual = compute_sha256(body, static_cast<std::size_t>(data_size));
  if (expected != actual) {
    return Result<Tensor>::Err(ErrorCode::kDataLoss, "SHA-256 checksum mismatch");
  }

  // Rebuild tensor
  Tensor t;
  t.spec.element_type = static_cast<ElementType>(read_uint32(hdr + 4));
  std::uint64_t rank = read_uint64(hdr + 8);
  std::size_t off = 16;
  for (std::uint64_t i = 0; i < rank; ++i) {
    t.spec.shape.dims.push_back(static_cast<std::int64_t>(read_uint64(hdr + off)));
    off += 8;
  }

  t.data.assign(body, body + static_cast<std::size_t>(data_size));
  return t;
}

}  // namespace socrates::transport
