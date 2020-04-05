//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "ProcessesDataView.h"

#include "App.h"
#include "Callstack.h"
#include "Capture.h"
#include "ModulesDataView.h"
#include "OrbitBase/Logging.h"
#include "OrbitType.h"
#include "Params.h"
#include "Pdb.h"
#include "TcpClient.h"

//-----------------------------------------------------------------------------
ProcessesDataView::ProcessesDataView() {
  InitColumnsIfNeeded();
  m_SortingOrders.insert(m_SortingOrders.end(), s_InitialOrders.begin(),
                         s_InitialOrders.end());

  UpdateProcessList();
  m_UpdatePeriodMs = 1000;
  m_IsRemote = false;

  GOrbitApp->RegisterProcessesDataView(this);
}

//-----------------------------------------------------------------------------
std::vector<std::wstring> ProcessesDataView::s_Headers;
std::vector<float> ProcessesDataView::s_HeaderRatios;
std::vector<DataView::SortingOrder> ProcessesDataView::s_InitialOrders;

//-----------------------------------------------------------------------------
void ProcessesDataView::InitColumnsIfNeeded() {
  if (s_Headers.empty()) {
    s_Headers.emplace_back(L"PID");
    s_HeaderRatios.push_back(0);
    s_InitialOrders.push_back(AscendingOrder);

    s_Headers.emplace_back(L"Name");
    s_HeaderRatios.push_back(0.5f);
    s_InitialOrders.push_back(AscendingOrder);

    s_Headers.emplace_back(L"CPU");
    s_HeaderRatios.push_back(0);
    s_InitialOrders.push_back(DescendingOrder);

    s_Headers.emplace_back(L"Type");
    s_HeaderRatios.push_back(0);
    s_InitialOrders.push_back(AscendingOrder);
  }
}

//-----------------------------------------------------------------------------
const std::vector<std::wstring>& ProcessesDataView::GetColumnHeaders() {
  return s_Headers;
}

//-----------------------------------------------------------------------------
const std::vector<float>& ProcessesDataView::GetColumnHeadersRatios() {
  return s_HeaderRatios;
}

//-----------------------------------------------------------------------------
const std::vector<DataView::SortingOrder>&
ProcessesDataView::GetColumnInitialOrders() {
  return s_InitialOrders;
}

//-----------------------------------------------------------------------------
int ProcessesDataView::GetDefaultSortingColumn() { return PDV_CPU; }

//-----------------------------------------------------------------------------
std::wstring ProcessesDataView::GetValue(int row, int col) {
  const Process& process = *GetProcess(row);
  std::wstring value;

  switch (col) {
    case PDV_ProcessID:
      value = std::to_wstring((long)process.GetID());
      break;
    case PDV_ProcessName:
      value = s2ws(process.GetName());
      if (process.IsElevated()) {
        value += L"*";
      }
      break;
    case PDV_CPU:
      value = Format(L"%.1f", process.GetCpuUsage());
      break;
    case PDV_Type:
      value = process.GetIs64Bit() ? L"64 bit" : L"32 bit";
      break;
    default:
      break;
  }

  return value;
}

//-----------------------------------------------------------------------------
std::wstring ProcessesDataView::GetToolTip(int a_Row, int /*a_Column*/) {
  const Process& process = *GetProcess(a_Row);
  return s2ws(process.GetFullName());
}

//-----------------------------------------------------------------------------
#define ORBIT_PROC_SORT(Member)                                            \
  [&](int a, int b) {                                                      \
    return OrbitUtils::Compare(processes[a]->Member, processes[b]->Member, \
                               ascending);                                 \
  }

//-----------------------------------------------------------------------------
void ProcessesDataView::OnSort(int a_Column,
                               std::optional<SortingOrder> a_NewOrder) {
  if (a_Column == -1) {
    a_Column = PdvColumn::PDV_CPU;
  }

  const std::vector<std::shared_ptr<Process>>& processes =
      m_ProcessList.GetProcesses();
  auto pdvColumn = static_cast<PdvColumn>(a_Column);

  if (a_NewOrder.has_value()) {
    m_SortingOrders[pdvColumn] = a_NewOrder.value();
  }

  bool ascending = m_SortingOrders[pdvColumn] == AscendingOrder;
  std::function<bool(int a, int b)> sorter = nullptr;

  switch (pdvColumn) {
    case PDV_ProcessID:
      sorter = ORBIT_PROC_SORT(GetID());
      break;
    case PDV_ProcessName:
      sorter = ORBIT_PROC_SORT(GetName());
      break;
    case PDV_CPU:
      sorter = ORBIT_PROC_SORT(GetCpuUsage());
      break;
    case PDV_Type:
      sorter = ORBIT_PROC_SORT(GetIs64Bit());
      break;
    default:
      break;
  }

  if (sorter) {
    std::sort(m_Indices.begin(), m_Indices.end(), sorter);
  }

  m_LastSortedColumn = a_Column;
  SetSelectedItem();
}

//-----------------------------------------------------------------------------
void ProcessesDataView::OnSelect(int a_Index) {
  m_SelectedProcess = GetProcess(a_Index);
  if (!m_IsRemote) {
    m_SelectedProcess->ListModules();
  } else if (m_SelectedProcess->GetModules().empty()) {
    Message msg(Msg_RemoteProcessRequest);
    msg.m_Header.m_GenericHeader.m_Address = m_SelectedProcess->GetID();
    GTcpClient->Send(msg);
  }

  LOG("process name: %s, address: %x", m_SelectedProcess->GetName().c_str(),
      m_SelectedProcess.get());
  UpdateModuleDataView(m_SelectedProcess);
}

void ProcessesDataView::UpdateModuleDataView(
    std::shared_ptr<Process> a_Process) {
  if (m_ModulesDataView) {
    m_ModulesDataView->SetProcess(a_Process);
    Capture::SetTargetProcess(a_Process);
    GOrbitApp->FireRefreshCallbacks();
  }
}

//-----------------------------------------------------------------------------
void ProcessesDataView::OnTimer() { Refresh(); }

//-----------------------------------------------------------------------------
void ProcessesDataView::Refresh() {
  if (Capture::IsCapturing()) {
    return;
  }

  if (!m_IsRemote) {
    m_ProcessList.Refresh();
    m_ProcessList.UpdateCpuTimes();
  }
  UpdateProcessList();
  OnSort(m_LastSortedColumn, {});
  OnFilter(m_Filter);
  SetSelectedItem();

  if (Capture::GTargetProcess && !Capture::IsCapturing()) {
    Capture::GTargetProcess->UpdateThreadUsage();
  }

  GParams.m_ProcessFilter = ws2s(m_Filter);
}

//-----------------------------------------------------------------------------
void ProcessesDataView::SetSelectedItem() {
  int initialIndex = m_SelectedIndex;
  m_SelectedIndex = -1;

  for (uint32_t i = 0; i < (uint32_t)GetNumElements(); ++i) {
    if (m_SelectedProcess &&
        GetProcess(i)->GetID() == m_SelectedProcess->GetID()) {
      m_SelectedIndex = i;
      return;
    }
  }

  if (GParams.m_AutoReleasePdb && initialIndex != -1) {
    ClearSelectedProcess();
  }
}

//-----------------------------------------------------------------------------
void ProcessesDataView::ClearSelectedProcess() {
  std::shared_ptr<Process> process = std::make_shared<Process>();
  Capture::SetTargetProcess(process);
  m_ModulesDataView->SetProcess(process);
  m_SelectedProcess = process;
  GPdbDbg = nullptr;
  GOrbitApp->FireRefreshCallbacks();
}

//-----------------------------------------------------------------------------
bool ProcessesDataView::SelectProcess(const std::wstring& a_ProcessName) {
  for (uint32_t i = 0; i < GetNumElements(); ++i) {
    Process& process = *GetProcess(i);
    if (process.GetFullName().find(ws2s(a_ProcessName)) != std::string::npos) {
      OnSelect(i);
      Capture::GPresetToLoad = "";
      return true;
    }
  }

  return false;
}

//-----------------------------------------------------------------------------
std::shared_ptr<Process> ProcessesDataView::SelectProcess(DWORD a_ProcessId) {
  Refresh();

  for (uint32_t i = 0; i < GetNumElements(); ++i) {
    Process& process = *GetProcess(i);
    if (process.GetID() == a_ProcessId) {
      OnSelect(i);
      Capture::GPresetToLoad = "";
      return m_SelectedProcess;
    }
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
void ProcessesDataView::OnFilter(const std::wstring& a_Filter) {
  std::vector<uint32_t> indices;
  const std::vector<std::shared_ptr<Process>>& processes =
      m_ProcessList.GetProcesses();

  std::vector<std::wstring> tokens = Tokenize(ToLower(a_Filter));

  for (uint32_t i = 0; i < processes.size(); ++i) {
    const Process& process = *processes[i];
    std::wstring name = s2ws(ToLower(process.GetName()));
    std::wstring type = process.GetIs64Bit() ? L"64" : L"32";

    bool match = true;

    for (std::wstring& filterToken : tokens) {
      if (!(name.find(filterToken) != std::wstring::npos ||
            type.find(filterToken) != std::wstring::npos)) {
        match = false;
        break;
      }
    }

    if (match) {
      indices.push_back(i);
    }
  }

  m_Indices = indices;

  if (m_LastSortedColumn != -1) {
    OnSort(m_LastSortedColumn, {});
  }
}

//-----------------------------------------------------------------------------
void ProcessesDataView::UpdateProcessList() {
  size_t num_processes = m_ProcessList.GetProcesses().size();
  m_Indices.resize(num_processes);
  std::iota(m_Indices.begin(), m_Indices.end(), 0);
}

//-----------------------------------------------------------------------------
void ProcessesDataView::SetRemoteProcessList(
    std::shared_ptr<ProcessList> a_RemoteProcessList) {
  m_IsRemote = true;
  m_ProcessList.UpdateFromRemote(a_RemoteProcessList);
  UpdateProcessList();
  OnSort(m_LastSortedColumn, {});
  OnFilter(m_Filter);
  SetSelectedItem();
}

//-----------------------------------------------------------------------------
void ProcessesDataView::SetRemoteProcess(std::shared_ptr<Process> a_Process) {
  CHECK(m_ProcessList.Contains(a_Process->GetID()));

  m_ProcessList.UpdateProcess(a_Process);
  m_SelectedProcess = a_Process;
  UpdateModuleDataView(m_SelectedProcess);
}

//-----------------------------------------------------------------------------
std::shared_ptr<Process> ProcessesDataView::GetProcess(
    unsigned int a_Row) const {
  return m_ProcessList.GetProcessByIndex(m_Indices[a_Row]);
}
