// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_QT_PROFILING_TARGET_DIALOG_H_
#define ORBIT_QT_PROFILING_TARGET_DIALOG_H_

#include <QDialog>
#include <QHistoryState>
#include <QModelIndex>
#include <QObject>
#include <QSortFilterProxyModel>
#include <QState>
#include <QStateMachine>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "Connections.h"
#include "MainThreadExecutor.h"
#include "OrbitClientData/ProcessData.h"
#include "OrbitClientServices/ProcessManager.h"
#include "ProcessItemModel.h"
#include "TargetConfiguration.h"
#include "grpcpp/channel.h"
#include "ui_ProfilingTargetDialog.h"

namespace orbit_qt {

class ProfilingTargetDialog : public QDialog {
  Q_OBJECT

 public:
  explicit ProfilingTargetDialog(SshConnectionArtifacts* ssh_connection_artifacts,
                                 MainThreadExecutor* main_thread_executor,
                                 std::optional<ConnectionConfiguration> connection_configuration,
                                 QWidget* parent = nullptr);
  std::optional<ConnectionConfiguration> Exec();
 private slots:
  void SelectFile();
  void SetupStadiaProcessManager();
  void SetupLocalProcessManager();
  void TearDownProcessManager();
  void ProcessSelectionChanged(const QModelIndex& current);
  void ConnectToLocal();

 signals:
  void FileSelected();
  void ProcessSelected();
  void NoProcessSelected();
  void StadiaIsConnected();
  void LocalIsConnected();
  void TryConnectToLocal();

 private:
  enum class TargetEnum { kStadia, kLocal, kFile };

  std::unique_ptr<Ui::ProfilingTargetDialog> ui_;

  TargetEnum current_target_;

  ProcessItemModel process_model_;
  QSortFilterProxyModel process_proxy_model_;

  MainThreadExecutor* main_thread_executor_;

  std::unique_ptr<ProcessData> process_;
  std::unique_ptr<ProcessManager> process_manager_;

  std::shared_ptr<grpc::Channel> local_grpc_channel_;
  uint16_t local_grpc_port_;

  std::filesystem::path selected_file_path_;

  // State Machine & States
  QStateMachine state_machine_;
  QState s_stadia_;
  QHistoryState s_s_history_;
  QState s_s_connecting_;
  QState s_s_connected_;
  QState s_s_processes_loading_;
  QState s_s_process_selected_;
  QState s_s_no_process_selected_;

  QState s_file_;
  QHistoryState s_f_history_;
  QState s_f_file_selected_;
  QState s_f_no_file_selected_;

  QState s_local_;
  QHistoryState s_l_history_;
  QState s_l_connecting_;
  QState s_l_connected_;
  QState s_l_processes_loading_;
  QState s_l_process_selected_;
  QState s_l_no_process_selected_;

  void SetupStateMachine();
  void SetupStadiaStates();
  void SetupFileStates();
  void SetupLocalStates();
  void TrySelectProcess(const ProcessData& process);
  void OnProcessListUpdate(ProcessManager* process_manager);
  void SetupProcessManager(const std::shared_ptr<grpc::Channel>& grpc_channel);
};

}  // namespace orbit_qt

#endif  // ORBIT_QT_PROFILING_TARGET_DIALOG_H_