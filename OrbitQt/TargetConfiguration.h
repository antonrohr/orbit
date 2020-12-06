// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_QT_TARGET_CONFIGURATION_H_
#define ORBIT_QT_TARGET_CONFIGURATION_H_

#include "Connections.h"

namespace orbit_qt {

class StadiaProfilingTarget {
  friend class ProfilingTargetDialog;

 public:
  explicit StadiaProfilingTarget(StadiaConnection&& connection,
                                 std::unique_ptr<ProcessManager> process_manager,
                                 std::unique_ptr<ProcessData> process)
      : connection_(std::move(connection)),
        process_manager_(std::move(process_manager)),
        process_(std::move(process)) {
    CHECK(process_manager_ != nullptr);
    CHECK(process_ != nullptr);
  }
  StadiaConnection* GetConnection() { return &connection_; }
  ProcessManager* GetProcessManager() { return process_manager_.get(); }
  ProcessData* GetProcess() { return process_.get(); }

 private:
  StadiaConnection connection_;
  std::unique_ptr<ProcessManager> process_manager_;
  std::unique_ptr<ProcessData> process_;
};

class LocalTarget {
  friend class ProfilingTargetDialog;

 public:
  explicit LocalTarget(LocalConnection&& connection,
                       std::unique_ptr<ProcessManager> process_manager,
                       std::unique_ptr<ProcessData> process)
      : connection_(connection),
        process_manager_(std::move(process_manager)),
        process_(std::move(process)) {
    CHECK(process_manager_ != nullptr);
    CHECK(process_ != nullptr);
  }
  LocalConnection* GetConnection() { return &connection_; }
  ProcessManager* GetProcessManager() { return process_manager_.get(); }
  ProcessData* GetProcess() { return process_.get(); }

 private:
  LocalConnection connection_;
  std::unique_ptr<ProcessManager> process_manager_;
  std::unique_ptr<ProcessData> process_;
};

class FileTarget {
  friend class ProfilingTargetDialog;

 public:
  explicit FileTarget(std::filesystem::path capture_file_path)
      : capture_file_path_(capture_file_path) {}
  [[nodiscard]] const std::filesystem::path& GetCaptureFilePath() const {
    return capture_file_path_;
  }

 private:
  std::filesystem::path capture_file_path_;
};

using ConnectionConfiguration = std::variant<StadiaProfilingTarget, LocalTarget, FileTarget>;

}  // namespace orbit_qt

#endif  // ORBIT_QT_TARGET_CONFIGURATION_H_