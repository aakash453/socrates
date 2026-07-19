// include/socrates/runtime/backend_dispatch.h
// Platform-optimized backend selection with automatic fallback chains.
#pragma once

#include <memory>
#include <vector>

#include "socrates/inference/inference_backend.h"
#include "socrates/model/model_manager.h"

namespace socrates::runtime {

// Builds a priority-ordered backend chain for the current platform.
// The last entry is always a CPU backend (guaranteed to work).
// Registers all backends with the provided registry.
std::vector<std::shared_ptr<inference::InferenceBackend>>
build_backend_fallback_chain(inference::BackendRegistry& registry);

// Attempts to load a model shard, trying each backend in the chain.
// Falls through on failure until a backend succeeds or the chain is exhausted.
Result<std::unique_ptr<inference::InferenceSession>>
load_model_with_fallback(
    const std::vector<std::shared_ptr<inference::InferenceBackend>>& chain,
    const model::ShardLease& shard,
    const inference::LoadOptions& options,
    CancellationToken cancellation);

}  // namespace socrates::runtime
