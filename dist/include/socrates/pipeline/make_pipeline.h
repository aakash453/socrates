#include "socrates/pipeline/inference_pipeline.h"
#include <memory>

namespace socrates::pipeline {
std::unique_ptr<InferencePipeline> make_inference_pipeline();
}  // namespace socrates::pipeline
