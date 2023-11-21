/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/gpu/gpu_command_buffer.h"

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/gpu_driver.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/gpu/gpu_kernel.h"
#include "xla/stream_executor/gpu/gpu_stream.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/kernel_spec.h"
#include "xla/stream_executor/launch_dim.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/status.h"

namespace stream_executor::gpu {

using Mode = CommandBuffer::Mode;
using State = CommandBuffer::State;

std::string_view to_string(State state) {
  switch (state) {
    case State::kCreate:
      return "create";
    case State::kUpdate:
      return "update";
    case State::kFinalized:
      return "finalized";
  }
}

tsl::Status UnsupportedStateError(State state) {
  return absl::InternalError(
      absl::StrCat("Unsupported command buffer state: ", to_string(state)));
}

//===----------------------------------------------------------------------===//
// GpuCommandBuffer resource usage tracking
//===----------------------------------------------------------------------===//

static std::atomic<int64_t> allocated_execs(0);
static std::atomic<int64_t> alive_execs(0);

static int64_t NotifyExecCreated() {
  alive_execs.fetch_add(1, std::memory_order_relaxed);
  return allocated_execs.fetch_add(1, std::memory_order_relaxed);
}

static int64_t NotifyExecDestroyed() {
  DCHECK_GE(alive_execs.load(std::memory_order_relaxed), 1);
  return alive_execs.fetch_sub(1, std::memory_order_relaxed) - 1;
}

/*static*/ int64_t GpuCommandBuffer::AllocatedExecs() {
  return allocated_execs.load(std::memory_order_relaxed);
}

/*static*/ int64_t GpuCommandBuffer::AliveExecs() {
  return alive_execs.load(std::memory_order_relaxed);
}

//===----------------------------------------------------------------------===//
// GpuCommandBuffer implementation
//===----------------------------------------------------------------------===//

GpuCommandBuffer::GpuCommandBuffer(Mode mode, GpuExecutor* parent,
                                   GpuGraphHandle graph, bool is_owned_graph)
    : mode_(mode),
      parent_(parent),
      graph_(graph),
      is_owned_graph_(is_owned_graph) {}

GpuCommandBuffer::~GpuCommandBuffer() {
  if (exec_ != nullptr) {
    VLOG(5) << "Destroy GPU command buffer executable graph " << exec_ << " "
            << "(remaining alive executable graphs: " << NotifyExecDestroyed()
            << ")";
    auto st = GpuDriver::DestroyGraphExec(exec_);
    CHECK(st.ok()) << "Failed to destroy GPU graph exec: " << st.message();
  }
  if (graph_ != nullptr && is_owned_graph_) {
    auto st = GpuDriver::DestroyGraph(graph_);
    CHECK(st.ok()) << "Failed to destroy GPU graph: " << st.message();
  }
}

static GpuDevicePtr AsDevicePtr(const DeviceMemoryBase& mem) {
  return reinterpret_cast<GpuDevicePtr>(const_cast<void*>(mem.opaque()));
}

tsl::Status GpuCommandBuffer::Trace(
    Stream* stream, absl::AnyInvocable<tsl::Status()> function) {
  // TODO(ezhulenev): Check that graph is empty, because we should not be mixing
  // graph tracing with explicit graph construction.
  TF_RETURN_IF_ERROR(CheckNotFinalized());

  VLOG(5) << "Trace into GPU command buffer graph " << graph_
          << " on a stream: " << stream->DebugStreamPointers();

  auto gpu_stream = AsGpuStreamValue(stream);

  // Switch stream into the capture mode.
  uint64_t start_nanos = tsl::Env::Default()->NowNanos();
  TF_RETURN_IF_ERROR(GpuDriver::StreamBeginCapture(
      gpu_stream, GpuDriver::StreamCaptureMode::kThreadLocal));

  auto traced = function();

  // Always stop capturing the stream before checking `traced` result.
  TF_RETURN_IF_ERROR(GpuDriver::StreamEndCapture(gpu_stream, &graph_));
  uint64_t end_nanos = tsl::Env::Default()->NowNanos();

  if (!traced.ok())
    return absl::InternalError(
        absl::StrCat("Failed to capture gpu graph: ", traced.message()));

  VLOG(5) << "Traced into the GPU command buffer graph " << graph_ << " (took "
          << (end_nanos - start_nanos) / 1000 << " μs)";

  return tsl::OkStatus();
}

GpuCommandBuffer::Dependencies GpuCommandBuffer::GetDependencies() {
  return nodes_.empty() ? Dependencies() : Dependencies{nodes_.back()};
}

tsl::Status GpuCommandBuffer::CheckNotFinalized() {
  if (state_ == State::kFinalized)
    return absl::InternalError(
        "Command can't be added to a command buffer after it was finalized");
  return tsl::OkStatus();
}

tsl::Status GpuCommandBuffer::CheckPrimary() {
  if (mode_ != Mode::kPrimary)
    return absl::InternalError(
        "Command can't be added to a non-primary command buffer");
  return tsl::OkStatus();
}

tsl::Status GpuCommandBuffer::Launch(const ThreadDim& threads,
                                     const BlockDim& blocks,
                                     const Kernel& kernel,
                                     const KernelArgs& args) {
  TF_RETURN_IF_ERROR(CheckNotFinalized());

  const GpuKernel* gpu_kernel = AsGpuKernel(&kernel);
  GpuFunctionHandle gpu_func = gpu_kernel->AsGpuFunctionHandle();

  auto* packed_args = DynCast<KernelArgsPackedArrayBase>(&args);
  if (!packed_args)
    return absl::InternalError("Unsupported kernel arguments type");

  void** kernel_params =
      const_cast<void**>(packed_args->argument_addresses().data());

  // Adds a new kernel node to the graph under construction.
  if (state_ == State::kCreate) {
    Dependencies deps = GetDependencies();
    GpuGraphNodeHandle* node = &nodes_.emplace_back();
    return GpuDriver::GraphAddKernelNode(
        node, graph_, absl::MakeSpan(deps), kernel.name(), gpu_func, blocks.x,
        blocks.y, blocks.z, threads.x, threads.y, threads.z,
        args.number_of_shared_bytes(), kernel_params, /*extra=*/nullptr);
  }

  // Updates kernel node in the executable graph.
  if (state_ == State::kUpdate) {
    GpuGraphNodeHandle node = nodes_[node_update_idx_++];
    return GpuDriver::GraphExecKernelNodeSetParams(
        exec_, node, kernel.name(), gpu_func, blocks.x, blocks.y, blocks.z,
        threads.x, threads.y, threads.z, args.number_of_shared_bytes(),
        kernel_params, /*extra=*/nullptr);
  }

  return UnsupportedStateError(state_);
}

tsl::Status GpuCommandBuffer::AddNestedCommandBuffer(
    const CommandBuffer& nested) {
  TF_RETURN_IF_ERROR(CheckNotFinalized());
  TF_RETURN_IF_ERROR(CheckPrimary());

  GpuGraphHandle child_graph = GpuCommandBuffer::Cast(&nested)->graph();

  // Adds a child graph node to the graph under construction.
  if (state_ == State::kCreate) {
    Dependencies deps = GetDependencies();
    GpuGraphNodeHandle* node = &nodes_.emplace_back();
    return GpuDriver::GraphAddChildNode(node, graph_, absl::MakeSpan(deps),
                                        child_graph);
  }

  // Updates child graph node in the executable graph.
  if (state_ == State::kUpdate) {
    GpuGraphNodeHandle node = nodes_[node_update_idx_++];
    return GpuDriver::GraphExecChildNodeSetParams(exec_, node, child_graph);
  }

  return UnsupportedStateError(state_);
}

tsl::Status GpuCommandBuffer::MemcpyDeviceToDevice(DeviceMemoryBase* dst,
                                                   const DeviceMemoryBase& src,
                                                   uint64_t size) {
  TF_RETURN_IF_ERROR(CheckNotFinalized());

  // Adds a new memcpy node to the graph under construction.
  if (state_ == State::kCreate) {
    Dependencies deps = GetDependencies();
    GpuGraphNodeHandle* node = &nodes_.emplace_back();
    return GpuDriver::GraphAddMemcpyD2DNode(
        parent_->gpu_context(), node, graph_, absl::MakeSpan(deps),
        AsDevicePtr(*dst), AsDevicePtr(src), size);
  }

  return UnsupportedStateError(state_);
}

tsl::Status GpuCommandBuffer::If(StreamExecutor* executor,
                                 DeviceMemory<bool> predicate,
                                 CommandBuffer::Builder then_builder) {
  DCHECK(executor->implementation() == parent_);  // NOLINT

  SetConditionKernel set_condition(executor);

  {  // Load kernels that update condition handle value.
    MultiKernelLoaderSpec spec(/*arity=*/1);
    spec.AddInProcessSymbol(gpu::GetSetConditionKernel(), "set_condition");
    TF_RETURN_IF_ERROR(executor->GetKernel(spec, &set_condition));
  }

  using ConditionalParams = GpuDriver::GpuGraphConditionalNodeParams;
  using ConditionalResult = GpuDriver::GpuGraphConditionalNodeParams::Result;

  // Conditional command buffers always created in nested mode.
  CommandBuffer::Mode nested = CommandBuffer::Mode::kNested;

  if (state_ == State::kCreate) {
    // Create a handle for a conditional node.
    GpuGraphConditionalHandle handle;
    TF_RETURN_IF_ERROR(GpuDriver::GraphConditionalHandleCreate(
        &handle, graph_, parent_->gpu_context(), 0, 0));

    // Add a kernel to update conditional handle value based on a predicate.
    TF_RETURN_IF_ERROR(
        Launch(set_condition, ThreadDim(), BlockDim(), handle, predicate));

    // Add conditional node to the graph.
    Dependencies deps = GetDependencies();
    GpuGraphNodeHandle* node = &nodes_.emplace_back();

    ConditionalParams params;
    params.type = ConditionalParams::Type::kIf;
    params.handle = handle;
    params.context = parent_->gpu_context();

    TF_ASSIGN_OR_RETURN(
        GpuDriver::GpuGraphNodeResult result,
        GpuDriver::GraphAddNode(node, graph_, absl::MakeSpan(deps), params));

    // Set up conditional command buffer.
    GpuGraphHandle then_graph = std::get<ConditionalResult>(result).graph;

    // Wrap conditional graph into command buffer and pass it to the builder.
    auto then_command_buffer = CommandBuffer::Wrap(
        executor, parent_->GetCommandBufferImplementation(
                      nested, then_graph, /*is_owned_graph=*/false));
    TF_RETURN_IF_ERROR(then_builder(&then_command_buffer));
    TF_RETURN_IF_ERROR(then_command_buffer.Finalize());

    return tsl::OkStatus();
  }

  // TODO(ezhulenev): For command buffer update we need to keep conditional
  // handle for the command and command buffer itself as it has a mapping to
  // node handles required for updates.

  return UnsupportedStateError(state_);
}

tsl::Status GpuCommandBuffer::Finalize() {
  TF_RETURN_IF_ERROR(CheckNotFinalized());

  if (mode_ == Mode::kPrimary && state_ == State::kCreate) {
    // If this is the first time we finalize command buffer after construction,
    // we need to instantiate it to an executable graph.
    GpuDriver::GraphInstantiateFlags flags;

    uint64_t start_nanos = tsl::Env::Default()->NowNanos();
    TF_RETURN_IF_ERROR(GpuDriver::GraphInstantiate(&exec_, graph_, flags));
    uint64_t end_nanos = tsl::Env::Default()->NowNanos();

    VLOG(5) << "Instantiated executable graph " << exec_ << " in "
            << (end_nanos - start_nanos) / 1000 << " μs ("
            << "#" << NotifyExecCreated() << ", "
            << "alive executable graphs: " << AliveExecs() << ")";

  } else if (mode_ == Mode::kPrimary && state_ == State::kUpdate) {
    // If this is a finalization after update, we don't have to do anything as
    // each individual command already updated executable graph.
    VLOG(5) << "Finalize executable graph " << exec_ << " update #"
            << num_updates_++ << " "
            << "(alive executable graphs: " << AliveExecs() << ")";

  } else if (mode_ == Mode::kNested) {
    // Nested command buffers do not have executable graphs.
    VLOG(5) << "Finalize nested command buffer without instantiating "
               "executable graph";
  }

  state_ = State::kFinalized;
  return tsl::OkStatus();
}

tsl::Status GpuCommandBuffer::Update() {
  if (state_ != State::kFinalized) {
    return absl::InternalError(
        "Command buffer has to be finalized first before it can be updated");
  }

  if (exec_ == nullptr) {
    if (mode_ == Mode::kPrimary)
      return absl::InternalError(
          "Primary command buffers are expected to have executable graphs");
    return absl::UnimplementedError(
        "Nested command buffer update is deliberately not implemented. One "
        "should create a new nested command buffer and update the primary one "
        "instead");
  }

  VLOG(5) << "Begin primary command buffer update for executable graph "
          << exec_;

  state_ = State::kUpdate;
  node_update_idx_ = 0;
  return tsl::OkStatus();
}

}  // namespace stream_executor::gpu
