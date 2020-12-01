// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ProfilingTargetDialog.h"

#include <absl/flags/declare.h>
#include <absl/flags/flag.h>
#include <absl/strings/str_format.h>
#include <absl/time/time.h>
#include <grpc/impl/codegen/connectivity_state.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractState>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHistoryState>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QModelIndexList>
#include <QObject>
#include <QPushButton>
#include <QRadioButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <Qt>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "ConnectToStadiaWidget.h"
#include "Connections.h"
#include "MainThreadExecutor.h"
#include "OrbitBase/Logging.h"
#include "OrbitClientServices/ProcessManager.h"
#include "OverlayWidget.h"
#include "Path.h"
#include "ProcessItemModel.h"
#include "TargetConfiguration.h"
#include "process.pb.h"
#include "servicedeploymanager.h"

ABSL_DECLARE_FLAG(bool, local);

namespace {
const int kProcessesRowHeight = 19;
const int kLocalTryConnectTimeoutMs = 1000;
}  // namespace

namespace orbit_qt {

using orbit_grpc_protos::ProcessInfo;

ProfilingTargetDialog::ProfilingTargetDialog(
    SshConnectionArtifacts* ssh_connection_artifacts, MainThreadExecutor* main_thread_executor,
    std::optional<ConnectionConfiguration> connection_configuration, QWidget* parent)
    : QDialog(parent),
      ui_(std::make_unique<Ui::ProfilingTargetDialog>()),
      current_target_(TargetEnum::kFile),
      main_thread_executor_(main_thread_executor),
      local_grpc_port_(ssh_connection_artifacts->GetGrpcPort().grpc_port),
      s_stadia_(&state_machine_),
      s_s_history_(&s_stadia_),
      s_s_connecting_(&s_stadia_),
      s_s_connected_(&s_stadia_),
      s_s_processes_loading_(&s_s_connected_),
      s_s_process_selected_(&s_s_connected_),
      s_s_no_process_selected_(&s_s_connected_),
      s_file_(&state_machine_),
      s_f_history_(&s_file_),
      s_f_file_selected_(&s_file_),
      s_f_no_file_selected_(&s_file_),
      s_local_(&state_machine_),
      s_l_history_(&s_local_),
      s_l_connecting_(&s_local_),
      s_l_connected_(&s_local_),
      s_l_processes_loading_(&s_l_connected_),
      s_l_process_selected_(&s_l_connected_),
      s_l_no_process_selected_(&s_l_connected_) {
  CHECK(ssh_connection_artifacts != nullptr);
  CHECK(main_thread_executor_ != nullptr);

  setWindowFlags(Qt::Window);

  ui_->setupUi(this);

  SetupStateMachine();

  process_proxy_model_.setSourceModel(&process_model_);
  process_proxy_model_.setSortRole(Qt::EditRole);
  process_proxy_model_.setFilterCaseSensitivity(Qt::CaseInsensitive);
  ui_->processesTableView->setModel(&process_proxy_model_);
  ui_->processesTableView->setSortingEnabled(true);
  ui_->processesTableView->sortByColumn(static_cast<int>(ProcessItemModel::Column::kCpu),
                                        Qt::DescendingOrder);

  ui_->processesTableView->horizontalHeader()->resizeSection(
      static_cast<int>(ProcessItemModel::Column::kPid), 60);
  ui_->processesTableView->horizontalHeader()->resizeSection(
      static_cast<int>(ProcessItemModel::Column::kCpu), 60);
  ui_->processesTableView->horizontalHeader()->setSectionResizeMode(
      static_cast<int>(ProcessItemModel::Column::kName), QHeaderView::Stretch);
  ui_->processesTableView->verticalHeader()->setDefaultSectionSize(kProcessesRowHeight);
  ui_->processesTableView->verticalHeader()->setVisible(false);

  if (absl::GetFlag(FLAGS_local)) {
    ui_->localFrame->setVisible(true);
  }

  QObject::connect(ui_->loadFromFileButton, &QPushButton::clicked, this,
                   &ProfilingTargetDialog::SelectFile);
  QObject::connect(ui_->loadCaptureRadioButton, &QRadioButton::clicked, this, [this](bool checked) {
    if (!checked) ui_->loadCaptureRadioButton->setChecked(true);
  });
  QObject::connect(ui_->localProfilingRadioButton, &QRadioButton::clicked, this,
                   [this](bool checked) {
                     if (!checked) ui_->localProfilingRadioButton->setChecked(true);
                   });
  QObject::connect(ui_->processesTableView->selectionModel(), &QItemSelectionModel::currentChanged,
                   this, &ProfilingTargetDialog::ProcessSelectionChanged);
  QObject::connect(ui_->processesTableView, &QTableView::doubleClicked, this, &QDialog::accept);
  QObject::connect(ui_->processFilterLineEdit, &QLineEdit::textChanged, &process_proxy_model_,
                   &QSortFilterProxyModel::setFilterFixedString);
  QObject::connect(ui_->confirmButton, &QPushButton::clicked, this, &QDialog::accept);

  if (!connection_configuration.has_value()) {
    if (absl::GetFlag(FLAGS_local)) {
      state_machine_.setInitialState(&s_local_);
    } else if (ui_->stadiaWidget->IsActive()) {
      state_machine_.setInitialState(&s_stadia_);
    } else {
      state_machine_.setInitialState(&s_file_);
    }
    ui_->stadiaWidget->Start(ssh_connection_artifacts);
    return;
  }

  std::optional<StadiaConnection> stadia_connection = std::nullopt;

  std::visit(
      [this, &stadia_connection](auto&& target) {
        using Target = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<Target, StadiaProfilingTarget>) {
          stadia_connection = std::move(target.connection_);
          process_manager_ = std::move(target.process_manager_);
          process_ = std::move(target.process_);
          process_manager_->SetProcessListUpdateListener(
              [this](ProcessManager* process_manager) { OnProcessListUpdate(process_manager); });

          s_stadia_.setInitialState(&s_s_connected_);
          s_s_history_.setDefaultState(&s_s_connected_);
          state_machine_.setInitialState(&s_stadia_);
        } else if constexpr (std::is_same_v<Target, LocalTarget>) {
          local_grpc_channel_ = target.GetConnection()->GetGrpcChannel();
          process_manager_ = std::move(target.process_manager_);
          process_ = std::move(target.process_);
          process_manager_->SetProcessListUpdateListener(
              [this](ProcessManager* process_manager) { OnProcessListUpdate(process_manager); });

          s_local_.setInitialState(&s_l_connected_);
          s_l_history_.setDefaultState(&s_l_connected_);
          state_machine_.setInitialState(&s_local_);
        } else if constexpr (std::is_same_v<Target, FileTarget>) {
          selected_file_path_ = target.capture_file_path_;
          s_file_.setInitialState(&s_f_file_selected_);
          s_f_history_.setDefaultState(&s_f_file_selected_);
          state_machine_.setInitialState(&s_file_);
        } else {
          UNREACHABLE();
        }
      },
      connection_configuration.value());

  ui_->stadiaWidget->Start(ssh_connection_artifacts, std::move(stadia_connection));
}

std::optional<ConnectionConfiguration> ProfilingTargetDialog::Exec() {
  state_machine_.start();
  int rc = exec();
  state_machine_.stop();

  if (rc != 1) {
    // User closed dialog
    return std::nullopt;
  }

  if (process_manager_ != nullptr) {
    process_manager_->SetProcessListUpdateListener(nullptr);
  }

  switch (current_target_) {
    case TargetEnum::kStadia:
      return StadiaProfilingTarget(ui_->stadiaWidget->StopAndClearConnection().value(),
                                   std::move(process_manager_), std::move(process_));
    case TargetEnum::kLocal:
      return LocalTarget(LocalConnection(std::move(local_grpc_channel_)),
                         std::move(process_manager_), std::move(process_));
    case TargetEnum::kFile:
      return FileTarget(selected_file_path_);
    default:
      UNREACHABLE();
      return std::nullopt;
  }
}

void ProfilingTargetDialog::ProcessSelectionChanged(const QModelIndex& current) {
  if (!current.isValid()) {
    process_ = nullptr;
    emit NoProcessSelected();
    return;
  }

  CHECK(current.model() == &process_proxy_model_);
  process_ = std::make_unique<ProcessData>(*current.data(Qt::UserRole).value<const ProcessInfo*>());
  emit ProcessSelected();
}

void ProfilingTargetDialog::SetupStateMachine() {
  state_machine_.setGlobalRestorePolicy(QStateMachine::RestoreProperties);

  SetupStadiaStates();
  SetupFileStates();
  SetupLocalStates();
}

void ProfilingTargetDialog::SetupStadiaStates() {
  // Setup initial and default
  s_stadia_.setInitialState(&s_s_connecting_);
  s_s_history_.setDefaultState(&s_s_connecting_);
  s_s_connected_.setInitialState(&s_s_processes_loading_);

  // PROPERTIES
  // STATE s_stadia_
  s_stadia_.assignProperty(ui_->confirmButton, "text", "Confirm Process");
  s_stadia_.assignProperty(ui_->confirmButton, "enabled", false);
  s_stadia_.assignProperty(ui_->confirmButton, "toolTip",
                           "Please connect to an instance and select a process.");
  s_stadia_.assignProperty(ui_->stadiaWidget, "active", true);
  s_stadia_.assignProperty(ui_->loadCaptureRadioButton, "checked", false);
  s_stadia_.assignProperty(ui_->localProfilingRadioButton, "checked", false);

  // STATE s_s_connecting_
  s_s_connecting_.assignProperty(ui_->processesFrame, "enabled", false);

  // STATE s_s_processes_loading_
  s_s_processes_loading_.assignProperty(ui_->processesTableOverlay, "visible", true);
  s_s_processes_loading_.assignProperty(ui_->processesTableOverlay, "cancelable", false);
  s_s_processes_loading_.assignProperty(ui_->processesTableOverlay, "statusMessage",
                                        "Loading processes...");

  // STATE s_s_process_selected_
  s_s_process_selected_.assignProperty(ui_->confirmButton, "enabled", true);

  // TRANSITIONS (and entered/exit events)
  // STATE s_stadia_
  s_stadia_.addTransition(ui_->loadCaptureRadioButton, &QRadioButton::clicked, &s_f_history_);
  s_stadia_.addTransition(ui_->localProfilingRadioButton, &QRadioButton::clicked, &s_l_history_);
  s_stadia_.addTransition(ui_->stadiaWidget, &ConnectToStadiaWidget::Disconnected,
                          &s_s_connecting_);
  QObject::connect(&s_stadia_, &QState::entered,
                   [this]() { current_target_ = TargetEnum::kStadia; });

  // STATE s_s_connecting_
  s_s_connecting_.addTransition(ui_->stadiaWidget, &ConnectToStadiaWidget::Connected,
                                &s_s_connected_);
  s_s_connecting_.addTransition(this, &ProfilingTargetDialog::StadiaIsConnected, &s_s_connected_);
  QObject::connect(&s_s_connecting_, &QState::entered, this, [this]() {
    if (ui_->stadiaWidget->GetGrpcChannel() != nullptr) {
      emit StadiaIsConnected();
    }
  });

  // STATE s_s_connected_
  QObject::connect(&s_s_connected_, &QState::entered, this,
                   &ProfilingTargetDialog::SetupStadiaProcessManager);
  QObject::connect(&s_s_connected_, &QState::exited, this,
                   &ProfilingTargetDialog::TearDownProcessManager);

  // STATE s_s_processes_loading_
  s_s_processes_loading_.addTransition(this, &ProfilingTargetDialog::ProcessSelected,
                                       &s_s_process_selected_);

  // STATE s_s_no_process_selected_
  s_s_no_process_selected_.addTransition(this, &ProfilingTargetDialog::ProcessSelected,
                                         &s_s_process_selected_);

  // STATE s_s_process_selected
  s_s_process_selected_.addTransition(this, &ProfilingTargetDialog::NoProcessSelected,
                                      &s_s_no_process_selected_);
}

void ProfilingTargetDialog::SetupLocalStates() {
  // Setup initial and default
  s_local_.setInitialState(&s_l_connecting_);
  s_l_history_.setDefaultState(&s_l_connecting_);
  s_l_connected_.setInitialState(&s_l_processes_loading_);

  // PROPERTIES
  // STATE s_local_
  s_local_.assignProperty(ui_->confirmButton, "text", "Confirm Process");
  s_local_.assignProperty(ui_->confirmButton, "enabled", false);
  s_local_.assignProperty(
      ui_->confirmButton, "toolTip",
      "Please have a OrbitService run on the local machine and select a process.");
  s_local_.assignProperty(ui_->localProfilingRadioButton, "checked", true);
  s_local_.assignProperty(ui_->stadiaWidget, "active", false);
  s_local_.assignProperty(ui_->loadCaptureRadioButton, "checked", false);

  // STATE s_l_connecting_
  s_l_connecting_.assignProperty(ui_->localProfilingStatusMessage, "text", "Connecting...");

  // STATE s_l_connected_
  s_l_connected_.assignProperty(ui_->localProfilingStatusMessage, "text", "Connected");

  // STATE s_l_processes_loading_
  s_l_processes_loading_.assignProperty(ui_->processesTableOverlay, "visible", true);
  s_l_processes_loading_.assignProperty(ui_->processesTableOverlay, "cancelable", false);
  s_l_processes_loading_.assignProperty(ui_->processesTableOverlay, "statusMessage",
                                        "Loading processes...");

  // STATE s_l_process_selected_
  s_l_process_selected_.assignProperty(ui_->confirmButton, "enabled", true);

  // TRANSITIONS (and entered/exit events)
  // STATE s_local_
  s_local_.addTransition(ui_->stadiaWidget, &ConnectToStadiaWidget::Activated, &s_s_history_);
  s_local_.addTransition(ui_->loadCaptureRadioButton, &QRadioButton::clicked, &s_f_history_);
  QObject::connect(&s_local_, &QState::entered, [this] { current_target_ = TargetEnum::kLocal; });

  // STATE s_l_connecting
  s_l_connecting_.addTransition(this, &ProfilingTargetDialog::LocalIsConnected, &s_l_connected_);
  s_l_connecting_.addTransition(this, &ProfilingTargetDialog::TryConnectToLocal, &s_l_connecting_);
  QObject::connect(&s_l_connecting_, &QState::entered, this,
                   &ProfilingTargetDialog::ConnectToLocal);

  // STATE s_l_connected_
  QObject::connect(&s_l_connected_, &QState::entered, this,
                   &ProfilingTargetDialog::SetupLocalProcessManager);
  QObject::connect(&s_l_connected_, &QState::exited, this,
                   &ProfilingTargetDialog::TearDownProcessManager);

  // STATE s_l_processes_loading_
  s_l_processes_loading_.addTransition(this, &ProfilingTargetDialog::ProcessSelected,
                                       &s_l_process_selected_);

  // STATE s_l_no_process_selected_
  s_l_no_process_selected_.addTransition(this, &ProfilingTargetDialog::ProcessSelected,
                                         &s_l_process_selected_);

  // STATE s_l_process_selected_
  s_l_process_selected_.addTransition(this, &ProfilingTargetDialog::NoProcessSelected,
                                      &s_l_no_process_selected_);
}

void ProfilingTargetDialog::SetupFileStates() {
  // Setup initial and default
  s_file_.setInitialState(&s_f_no_file_selected_);
  s_f_history_.setDefaultState(&s_f_no_file_selected_);

  // PROPERTIES
  // STATE s_file_
  s_file_.assignProperty(ui_->confirmButton, "text", "Load Capture");
  s_file_.assignProperty(ui_->confirmButton, "enabled", false);
  s_file_.assignProperty(ui_->confirmButton, "toolTip", "Please select a capture to load");
  s_file_.assignProperty(ui_->stadiaWidget, "active", false);
  s_file_.assignProperty(ui_->loadCaptureRadioButton, "checked", true);
  s_file_.assignProperty(ui_->processesFrame, "enabled", false);
  s_file_.assignProperty(ui_->loadFromFileButton, "enabled", true);
  s_file_.assignProperty(ui_->localProfilingRadioButton, "checked", false);

  // STATE s_f_file_selected_
  s_f_file_selected_.assignProperty(ui_->confirmButton, "enabled", true);

  // TRANSITIONS (and entered/exit events)
  // STATE s_file_
  s_file_.addTransition(ui_->stadiaWidget, &ConnectToStadiaWidget::Activated, &s_s_history_);
  s_file_.addTransition(ui_->localProfilingRadioButton, &QRadioButton::clicked, &s_l_history_);
  s_file_.addTransition(this, &ProfilingTargetDialog::FileSelected, &s_f_file_selected_);
  QObject::connect(&s_file_, &QState::entered, [this]() { current_target_ = TargetEnum::kFile; });

  // STATE s_f_no_file_selected_
  QObject::connect(&s_f_no_file_selected_, &QState::entered, [this] {
    if (selected_file_path_.empty()) SelectFile();
  });

  // STATE s_f_file_selected_
  QObject::connect(&s_f_file_selected_, &QState::entered, [this] {
    ui_->selectedFileLabel->setText(
        QString::fromStdString(selected_file_path_.filename().string()));
  });
}

void ProfilingTargetDialog::TearDownProcessManager() {
  process_model_.Clear();

  if (process_manager_ != nullptr) {
    process_manager_->ShutdownAndWait();
    process_manager_ = nullptr;
  }
}

void ProfilingTargetDialog::SetupProcessManager(
    const std::shared_ptr<grpc::Channel>& grpc_channel) {
  CHECK(grpc_channel != nullptr);

  if (process_manager_ != nullptr) return;

  process_manager_ = ProcessManager::Create(grpc_channel, absl::Milliseconds(1000));
  process_manager_->SetProcessListUpdateListener(
      [this](ProcessManager* process_manager) { OnProcessListUpdate(process_manager); });
}

void ProfilingTargetDialog::SetupStadiaProcessManager() {
  SetupProcessManager(ui_->stadiaWidget->GetGrpcChannel());
}

void ProfilingTargetDialog::SelectFile() {
  const QString file = QFileDialog::getOpenFileName(
      this, "Open Capture...", QString::fromStdString(Path::CreateOrGetCaptureDir().string()),
      "*.orbit");
  if (!file.isEmpty()) {
    selected_file_path_ = std::filesystem::path(file.toStdString());

    emit FileSelected();
  }
}

void ProfilingTargetDialog::TrySelectProcess(const ProcessData& process) {
  QModelIndexList matches = process_proxy_model_.match(
      process_proxy_model_.index(0, static_cast<int>(ProcessItemModel::Column::kName)),
      Qt::DisplayRole, QVariant::fromValue(QString::fromStdString(process.name())));

  if (matches.isEmpty()) return;

  LOG("Selecting remembered process: %s", process.name());

  ui_->processesTableView->selectionModel()->setCurrentIndex(
      matches[0], {QItemSelectionModel::SelectCurrent, QItemSelectionModel::Rows});
}

void ProfilingTargetDialog::OnProcessListUpdate(ProcessManager* process_manager) {
  main_thread_executor_->Schedule([this, process_manager]() {
    // When this function call is scheduled, the process_manager might not exist anymore
    if (process_manager == nullptr) return;

    bool had_processes_before = process_model_.HasProcesses();
    process_model_.SetProcesses(process_manager->GetProcessList());
    if (!had_processes_before) return;

    if (ui_->processesTableView->selectionModel()->hasSelection()) return;

    if (process_ != nullptr) {
      TrySelectProcess(*process_);
    }

    if (ui_->processesTableView->selectionModel()->hasSelection()) return;

    ui_->processesTableView->selectRow(0);
  });
}

void ProfilingTargetDialog::ConnectToLocal() {
  process_model_.Clear();
  if (local_grpc_channel_ == nullptr) {
    local_grpc_channel_ =
        grpc::CreateCustomChannel(absl::StrFormat("127.0.0.1:%d", local_grpc_port_),
                                  grpc::InsecureChannelCredentials(), grpc::ChannelArguments());
  }

  if (local_grpc_channel_->GetState(true) != GRPC_CHANNEL_READY) {
    LOG("Local grpc connection not ready, Trying to connect to local OrbitService again in %d ms.",
        kLocalTryConnectTimeoutMs);
    QTimer::singleShot(kLocalTryConnectTimeoutMs, this, &ProfilingTargetDialog::TryConnectToLocal);
    return;
  }

  emit LocalIsConnected();
}

void ProfilingTargetDialog::SetupLocalProcessManager() { SetupProcessManager(local_grpc_channel_); }

}  // namespace orbit_qt