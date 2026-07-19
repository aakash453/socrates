// include/socrates/transport/tensor_codec.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::transport {

class TensorCodec {
 public:
  /**
   * Encodes a tensor and its spec into a compact wire format.
   * Format: [spec-header][data]. SHA-256 computed over data portion only.
   * Preconditions: tensor.data is non-empty and spec.maximum_encoded_bytes is
   * respected.
   * Postconditions: output contains the complete wire-format frame.
   * Throws: no operational exceptions; Result reports size mismatch.
   * Thread safety: const and reentrant.
   * Side effects: none.
   */
  Result<std::vector<std::byte>> encode(const Tensor& tensor) const;

  /**
   * Decodes a wire-format frame back into a Tensor with validation.
   * Preconditions: wire_bytes is a complete frame from encode().
   * Postconditions: success returns a validated tensor; checksum mismatch,
   * truncated data, or size violations return explicit errors.
   * Throws: no operational exceptions.
   * Thread safety: const and reentrant.
   * Side effects: none.
   */
  Result<Tensor> decode(const std::vector<std::byte>& wire_bytes) const;

 private:
  static constexpr std::uint32_t kHeaderMagic = 0x45414954;  // "E A I T"
  static constexpr std::size_t kHeaderSize = 64;
};

}  // namespace socrates::transport
