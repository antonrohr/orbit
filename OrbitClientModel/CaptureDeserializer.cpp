// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "OrbitClientModel/CaptureDeserializer.h"

#include <fstream>
#include <memory>

#include "Callstack.h"
#include "OrbitBase/MakeUniqueForOverwrite.h"
#include "OrbitClientData/FunctionUtils.h"
#include "OrbitClientData/ModuleManager.h"
#include "absl/strings/str_format.h"
#include "capture_data.pb.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/message.h"
#include "process.pb.h"

using orbit_client_protos::CallstackEvent;
using orbit_client_protos::CallstackInfo;
using orbit_client_protos::CaptureHeader;
using orbit_client_protos::CaptureInfo;
using orbit_client_protos::FunctionInfo;
using orbit_client_protos::TimerInfo;
using orbit_grpc_protos::ProcessInfo;
using OrbitClientData::ModuleManager;

namespace capture_deserializer {

void Load(const std::string& file_name, CaptureListener* capture_listener,
          ModuleManager* module_manager, std::atomic<bool>* cancellation_requested) {
  SCOPE_TIMER_LOG(absl::StrFormat("Loading capture from \"%s\"", file_name));

  // Binary
  std::ifstream file(file_name, std::ios::binary);
  if (file.fail()) {
    ERROR("Loading capture from \"%s\": %s", file_name, "file.fail()");
    capture_listener->OnCaptureFailed(
        ErrorMessage(absl::StrFormat("Error opening file \"%s\" for reading", file_name)));
    return;
  }

  return Load(file, file_name, capture_listener, module_manager, cancellation_requested);
}

void Load(std::istream& stream, const std::string& file_name, CaptureListener* capture_listener,
          ModuleManager* module_manager, std::atomic<bool>* cancellation_requested) {
  google::protobuf::io::IstreamInputStream input_stream(&stream);
  google::protobuf::io::CodedInputStream coded_input(&input_stream);

  std::string error_message = absl::StrFormat(
      "Error parsing the capture from \"%s\".\nNote: If the capture "
      "was taken with a previous Orbit version, it could be incompatible. "
      "Please check release notes for more information.",
      file_name);

  CaptureHeader header;
  if (!internal::ReadMessage(&header, &coded_input) || header.version().empty()) {
    ERROR("%s", error_message);
    capture_listener->OnCaptureFailed(ErrorMessage(std::move(error_message)));
    return;
  }
  if (header.version() != internal::kRequiredCaptureVersion) {
    std::string incompatible_version_error_message = absl::StrFormat(
        "The format of capture \"%s\" is no longer supported but could be opened with "
        "Orbit version %s.",
        file_name, header.version());
    ERROR("%s", incompatible_version_error_message);
    capture_listener->OnCaptureFailed(ErrorMessage(std::move(incompatible_version_error_message)));
    return;
  }

  CaptureInfo capture_info;
  if (!internal::ReadMessage(&capture_info, &coded_input)) {
    ERROR("%s", error_message);
    capture_listener->OnCaptureFailed(ErrorMessage(std::move(error_message)));
    return;
  }

  internal::LoadCaptureInfo(capture_info, capture_listener, module_manager, &coded_input,
                            cancellation_requested);
}

namespace internal {

bool ReadMessage(google::protobuf::Message* message,
                 google::protobuf::io::CodedInputStream* input) {
  uint32_t message_size;
  if (!input->ReadLittleEndian32(&message_size)) {
    return false;
  }

  std::unique_ptr<char[]> buffer = make_unique_for_overwrite<char[]>(message_size);
  if (!input->ReadRaw(buffer.get(), message_size)) {
    return false;
  }
  message->ParseFromArray(buffer.get(), message_size);

  return true;
}

void LoadCaptureInfo(const CaptureInfo& capture_info, CaptureListener* capture_listener,
                     ModuleManager* module_manager,
                     google::protobuf::io::CodedInputStream* coded_input,
                     std::atomic<bool>* cancellation_requested) {
  CHECK(capture_listener != nullptr);

  ProcessInfo process_info;
  process_info.set_pid(capture_info.process().pid());
  process_info.set_name(capture_info.process().name());
  process_info.set_cpu_usage(capture_info.process().cpu_usage());
  process_info.set_full_path(capture_info.process().full_path());
  process_info.set_command_line(capture_info.process().command_line());
  process_info.set_is_64_bit(capture_info.process().is_64_bit());
  ProcessData process(process_info);

  if (*cancellation_requested) {
    capture_listener->OnCaptureCancelled();
    return;
  }

  std::vector<orbit_grpc_protos::ModuleInfo> modules;
  absl::flat_hash_map<std::string, orbit_grpc_protos::ModuleInfo> module_map;
  for (const auto& module : capture_info.modules()) {
    orbit_grpc_protos::ModuleInfo module_info;
    module_info.set_file_path(module.file_path());
    module_info.set_file_size(module.file_size());
    module_info.set_address_start(module.address_start());
    module_info.set_address_end(module.address_end());
    module_info.set_build_id(module.build_id());
    module_info.set_load_bias(module.load_bias());
    modules.emplace_back(std::move(module_info));
    module_map[module.file_path()] = modules.back();
  }
  process.UpdateModuleInfos(modules);

  module_manager->AddNewModules(modules);

  if (*cancellation_requested) {
    capture_listener->OnCaptureCancelled();
    return;
  }

  absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> selected_functions;
  for (const auto& function : capture_info.selected_functions()) {
    const auto& module_it = module_map.find(function.loaded_module_path());
    CHECK(module_it != module_map.end());
    ModuleData module(module_it->second);
    uint64_t address = FunctionUtils::GetAbsoluteAddress(function, process, module);
    selected_functions[address] = function;
  }
  TracepointInfoSet selected_tracepoints;
  for (const orbit_client_protos::TracepointInfo& tracepoint_info :
       capture_info.tracepoint_infos()) {
    orbit_grpc_protos::TracepointInfo tracepoint_info_translated;
    tracepoint_info_translated.set_category(tracepoint_info.category());
    tracepoint_info_translated.set_name(tracepoint_info.name());
    selected_tracepoints.emplace(tracepoint_info_translated);
  }

  if (*cancellation_requested) {
    capture_listener->OnCaptureCancelled();
    return;
  }

  capture_listener->OnCaptureStarted(std::move(process), std::move(selected_functions),
                                     std::move(selected_tracepoints));

  for (const auto& address_info : capture_info.address_infos()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnAddressInfo(address_info);
  }

  for (const auto& thread_id_and_name : capture_info.thread_names()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnThreadName(thread_id_and_name.first, thread_id_and_name.second);
  }

  for (const orbit_client_protos::ThreadStateSliceInfo& thread_state_slice :
       capture_info.thread_state_slices()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnThreadStateSlice(thread_state_slice);
  }

  for (const CallstackInfo& callstack : capture_info.callstacks()) {
    CallStack unique_callstack({callstack.data().begin(), callstack.data().end()});
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnUniqueCallStack(std::move(unique_callstack));
  }
  for (CallstackEvent callstack_event : capture_info.callstack_events()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnCallstackEvent(std::move(callstack_event));
  }

  for (const orbit_client_protos::TracepointInfo& tracepoint_info :
       capture_info.tracepoint_infos()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    orbit_grpc_protos::TracepointInfo tracepoint_info_translated;
    tracepoint_info_translated.set_category(tracepoint_info.category());
    tracepoint_info_translated.set_name(tracepoint_info.name());
    capture_listener->OnUniqueTracepointInfo(tracepoint_info.tracepoint_info_key(),
                                             std::move(tracepoint_info_translated));
  }

  for (orbit_client_protos::TracepointEventInfo tracepoint_event_info :
       capture_info.tracepoint_event_infos()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnTracepointEvent(std::move(tracepoint_event_info));
  }

  for (const auto& key_to_string : capture_info.key_to_string()) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnKeyAndString(key_to_string.first, key_to_string.second);
  }

  // Timers
  TimerInfo timer_info;
  while (internal::ReadMessage(&timer_info, coded_input)) {
    if (*cancellation_requested) {
      capture_listener->OnCaptureCancelled();
      return;
    }
    capture_listener->OnTimer(timer_info);
  }

  capture_listener->OnCaptureComplete();
}

}  // namespace internal

}  // namespace capture_deserializer
