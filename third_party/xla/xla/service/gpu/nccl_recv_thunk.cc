/* Copyright 2023 The OpenXLA Authors.

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

#include "xla/service/gpu/nccl_recv_thunk.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/computation_placer.h"
#include "xla/service/global_device_id.h"
#include "xla/service/gpu/nccl_api.h"
#include "xla/service/gpu/nccl_collective_thunk.h"
#include "xla/service/gpu/nccl_p2p_thunk_common.h"
#include "xla/service/gpu/thunk.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/stream.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

NcclRecvThunk::NcclRecvThunk(ThunkInfo thunk_info, NcclApi* nccl_api,
                             const HloRecvInstruction* instr,
                             int64_t replica_count, int64_t partition_count,
                             const Buffer& buffer)
    : NcclCollectiveThunk(Thunk::kNcclRecv, thunk_info, nccl_api,
                          /*is_sync=*/false),
      config_(GetNcclP2PConfigForSendRecv(instr, instr->shape().tuple_shapes(0),
                                          replica_count, partition_count)),
      buffer_(buffer),
      stream_kind_(GetStreamKindForSendRecv(instr)),
      execution_counters_(config_.validation_kind ==
                                  NcclP2PConfig::ValidationKind::kConditional
                              ? new ExecutionCounters()
                              : nullptr) {}

absl::Status NcclRecvThunk::Initialize(const InitializeParams& params) {
  TF_RETURN_IF_ERROR(NcclCollectiveThunk::Initialize(params));
  if (execution_counters_) {
    TF_RETURN_IF_ERROR(execution_counters_->Initialize(params.executor));
  }
  return absl::OkStatus();
}

absl::Status NcclRecvThunk::RunNcclCollective(const ExecuteParams& params,
                                              se::Stream& stream,
                                              NcclApi::NcclCommHandle comm) {
  TF_ASSIGN_OR_RETURN(
      std::vector<DeviceBufferPair> device_buffers,
      ConvertToDeviceBuffers(params, {buffer_},
                             config_.config.operand_element_type));
  TF_RET_CHECK(device_buffers.size() == 1) << "Expected one buffer pair.";

  GlobalDeviceId global_device_id = params.collective_params->global_device_id;

  TF_ASSIGN_OR_RETURN(const DeviceAssignment::LogicalID current_logical_id,
                      params.collective_params->device_assn->LogicalIdForDevice(
                          global_device_id));
  const int64_t current_id =
      config_.config.group_mode == CollectiveOpGroupMode::kCrossReplica
          ? current_logical_id.replica_id
          : current_logical_id.computation_id;
  std::string device_string = GetDeviceString(*params.collective_params);

  const NcclP2PConfig::SourceTargetMapEntry source_target =
      NcclP2PConfig::GetSourceTarget(config_.id_to_source_target, current_id);
  DeviceBufferPair& buffer = device_buffers[0];

  // Determine the source IDs for this instance. The source ID is the ID for
  // the peer that will copy its data to this instance. If there is no
  // source, just memzero() the destination buffer.
  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing Recv from device ordinal: " << device_ordinal
          << "current_id " << current_id;

  const std::optional<int64_t> source_id = source_target.source;
  se::DeviceMemoryBase dest_addr = buffer.destination_buffer;

  VLOG(3) << absl::StreamFormat("%s : id = %d, source_id = %d", device_string,
                                current_id, source_id.value_or(-1));

  // Receive data from the source peer to the destination buffer.
  if (source_id) {
    bool should_run =
        config_.validation_kind == NcclP2PConfig::ValidationKind::kInvalid
            ? false
            : true;
    if (config_.validation_kind ==
        NcclP2PConfig::ValidationKind::kConditional) {
      se::StreamExecutor* executor = params.stream->parent();
      TF_ASSIGN_OR_RETURN(int64_t * counter,
                          execution_counters_->GetCounter(executor));
      auto it = config_.source_target_to_bounds.find(
          std::make_pair(*source_target.source, current_id));
      if (it == config_.source_target_to_bounds.end()) {
        return absl::InternalError("Missing bounds for conditional Recv");
      }
      if (*counter < it->second.first || *counter > it->second.second) {
        should_run = false;
      }
      VLOG(3) << "RunNcclCollective counter " << *counter << " " << should_run;
      ++(*counter);
    }
    if (should_run) {
      TF_RETURN_IF_ERROR(nccl_api()->Recv(dest_addr, buffer.element_type,
                                          buffer.element_count, *source_id,
                                          comm, &stream));
    }

  } else {
    // If there is no source peer, i.e. no sender to this instance, zero out
    // the destination buffer.
    VLOG(3) << absl::StreamFormat("%s : collective-Permute: Issuing MemZero",
                                  device_string);
    TF_RETURN_IF_ERROR(stream.MemZero(&dest_addr, dest_addr.size()));
  }
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
