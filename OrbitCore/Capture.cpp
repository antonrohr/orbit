//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "Capture.h"

#include <fstream>
#include <ostream>

#include "ConnectionManager.h"
#include "Core.h"
#include "CoreApp.h"
#include "EventBuffer.h"
#include "EventTracer.h"
#include "Injection.h"
#include "Log.h"
#include "OrbitBase/Logging.h"
#include "OrbitSession.h"
#include "OrbitUnreal.h"
#include "Params.h"
#include "Path.h"
#include "Pdb.h"
#include "SamplingProfiler.h"
#include "Serialization.h"
#include "TcpClient.h"
#include "TcpForward.h"
#include "TcpServer.h"
#include "TestRemoteMessages.h"
#include "TimerManager.h"
#include "absl/strings/str_format.h"

#ifndef _WIN32
std::shared_ptr<Pdb> GPdbDbg;
#endif

bool Capture::GInjected = false;
std::string Capture::GInjectedProcess;
bool Capture::GIsSampling = false;
bool Capture::GIsTesting = false;
uint32_t Capture::GFunctionIndex = -1;
uint32_t Capture::GNumInstalledHooks;
bool Capture::GHasContextSwitches;
Timer Capture::GTestTimer;
uint64_t Capture::GNumContextSwitches;
ULONG64 Capture::GNumLinuxEvents;
ULONG64 Capture::GNumProfileEvents;
std::string Capture::GPresetToLoad;
std::string Capture::GProcessToInject;

std::vector<std::shared_ptr<Function>> Capture::GSelectedFunctions;
std::map<uint64_t, Function*> Capture::GSelectedFunctionsMap;
std::map<uint64_t, Function*> Capture::GVisibleFunctionsMap;
std::unordered_map<uint64_t, uint64_t> Capture::GFunctionCountMap;
std::shared_ptr<CallStack> Capture::GSelectedCallstack;
std::vector<uint64_t> Capture::GSelectedAddressesByType[Function::NUM_TYPES];
std::unordered_map<uint64_t, std::shared_ptr<CallStack>> Capture::GCallstacks;
std::unordered_map<uint64_t, LinuxAddressInfo> Capture::GAddressInfos;
std::unordered_map<uint64_t, std::string> Capture::GAddressToFunctionName;
Mutex Capture::GCallstackMutex;
std::unordered_map<uint64_t, std::string> Capture::GZoneNames;
TextBox* Capture::GSelectedTextBox;
ThreadID Capture::GSelectedThreadId;
Timer Capture::GCaptureTimer;
std::chrono::system_clock::time_point Capture::GCaptureTimePoint;

std::shared_ptr<SamplingProfiler> Capture::GSamplingProfiler = nullptr;
std::shared_ptr<Process> Capture::GTargetProcess = nullptr;
std::shared_ptr<Session> Capture::GSessionPresets = nullptr;

void (*Capture::GClearCaptureDataFunc)();
std::vector<std::shared_ptr<SamplingProfiler>> GOldSamplingProfilers;
bool Capture::GUnrealSupported = false;

// The user_data pointer is provided by caller when registering capture
// callback. It is then passed to the callback and can be used to store context
// such as a pointer to a class.
Capture::SamplingDoneCallback Capture::sampling_done_callback_ = nullptr;
void* Capture::sampling_done_callback_user_data_ = nullptr;

//-----------------------------------------------------------------------------
void Capture::Init() { GTargetProcess = std::make_shared<Process>(); }

//-----------------------------------------------------------------------------
bool Capture::Inject(std::string_view remote_address) {
  Injection inject;
  std::string dllName = Path::GetDllPath(GTargetProcess->GetIs64Bit());

  GTcpServer->Disconnect();

  GInjected =
      inject.Inject(remote_address, dllName, *GTargetProcess, "OrbitInit");
  if (GInjected) {
    ORBIT_LOG(
        absl::StrFormat("Injected in %s", GTargetProcess->GetName().c_str()));
    GInjectedProcess = GTargetProcess->GetName();
  }

  // Wait for connections
  int numTries = 50;
  while (!GTcpServer->HasConnection() && numTries-- > 0) {
    ORBIT_LOG(absl::StrFormat("Waiting for connection on port %i",
                              GTcpServer->GetPort()));
    Sleep(100);
  }

  GInjected = GInjected && GTcpServer->HasConnection();

  return GInjected;
}

//-----------------------------------------------------------------------------
bool Capture::InjectRemote(std::string_view remote_address) {
  Injection inject;
  std::string dllName = Path::GetDllPath(GTargetProcess->GetIs64Bit());
  GTcpServer->Disconnect();

  GInjected = inject.Inject(remote_address, dllName, *GTargetProcess,
                            "OrbitInitRemote");

  if (GInjected) {
    ORBIT_LOG(
        absl::StrFormat("Injected in %s", GTargetProcess->GetName().c_str()));
    GInjectedProcess = GTargetProcess->GetName();
  }

  return GInjected;
}

//-----------------------------------------------------------------------------
void Capture::SetTargetProcess(const std::shared_ptr<Process>& a_Process) {
  if (a_Process != GTargetProcess) {
    GInjected = false;
    GInjectedProcess = "";

    GTargetProcess = a_Process;
    GSamplingProfiler = std::make_shared<SamplingProfiler>(
        a_Process);  // TODO(antonrohr) make unique?
    GSelectedFunctionsMap.clear();
    GFunctionCountMap.clear();
    GOrbitUnreal.Clear();
    GTargetProcess->LoadDebugInfo();
    GTargetProcess->ClearWatchedVariables();
  }
}

//-----------------------------------------------------------------------------
bool Capture::Connect(std::string_view remote_address) {
  if (!GInjected) {
    Inject(remote_address);
  }

  return GInjected;
}

//-----------------------------------------------------------------------------
// TODO: This method is resposible for too many things. We should split the
//  server side logic and client side logic into separate methods/classes.
outcome::result<void, std::string> Capture::StartCapture(
    std::string_view remote_address) {
  if (GTargetProcess->GetName().empty()) {
    return outcome::failure(
        "No process selected. Please choose a target process for the capture.");
  }

  SCOPE_TIMER_LOG("Capture::StartCapture");

  GCaptureTimer.Start();
  GCaptureTimePoint = std::chrono::system_clock::now();

#ifdef WIN32
  if (!IsRemote()) {
    if (!Connect(remote_address)) {
      return outcome::failure("Connection error.");
    }
  }
#endif

  GInjected = true;
  ++Message::GSessionID;
  GTcpServer->Send(Msg_NewSession);
  GTimerManager->StartRecording();

  ClearCaptureData();
  SendFunctionHooks();

  if (Capture::IsTrackingEvents()) {
#ifdef WIN32
    GEventTracer.Start();
#else
    UNUSED(remote_address);
#endif
  } else if (Capture::IsRemote()) {
    Capture::NewSamplingProfiler();
    Capture::GSamplingProfiler->SetIsLinuxPerf(true);
    Capture::GSamplingProfiler->StartCapture();
  }

  if (GCoreApp != nullptr) {
    GCoreApp->SendToUiNow("startcapture");

    if (!GSelectedFunctionsMap.empty()) {
      GCoreApp->SendToUiNow("gotolive");
    }
  }

  return outcome::success();
}

//-----------------------------------------------------------------------------
void Capture::StopCapture() {
  if (IsTrackingEvents()) {
#ifdef WIN32
    GEventTracer.Stop();
#endif
  } else if (Capture::IsRemote()) {
    Capture::GSamplingProfiler->StopCapture();
    Capture::GSamplingProfiler->ProcessSamples();

    if (GCoreApp != nullptr) {
      GCoreApp->RefreshCaptureView();
    }
  }

  if (!GInjected) {
    return;
  }

  TcpEntity* tcpEntity = Capture::GetMainTcpEntity();
  if (tcpEntity) {  // TODO(antonrohr) think about this: does it mean it might
                    // continue on the service?
    tcpEntity->Send(Msg_StopCapture);
  }
  if (GTimerManager) {
    GTimerManager->StopRecording();
  }
}

//-----------------------------------------------------------------------------
void Capture::ClearCaptureData() {
  GFunctionCountMap.clear();
  GCallstacks.clear();
  GAddressInfos.clear();
  GAddressToFunctionName.clear();
  GZoneNames.clear();
  GSelectedTextBox = nullptr;
  GSelectedThreadId = 0;
  GNumProfileEvents = 0;
  GTcpServer->ResetStats();
  GOrbitUnreal.NewSession();
  GHasContextSwitches = false;
  GNumLinuxEvents = 0;
  GNumContextSwitches = 0;
}

//-----------------------------------------------------------------------------
MessageType GetMessageType(Function::OrbitType a_type) {
  static std::map<Function::OrbitType, MessageType> typeMap;
  if (typeMap.empty()) {
    typeMap[Function::NONE] = Msg_FunctionHook;
    typeMap[Function::ORBIT_TIMER_START] = Msg_FunctionHookZoneStart;
    typeMap[Function::ORBIT_TIMER_STOP] = Msg_FunctionHookZoneStop;
    typeMap[Function::ORBIT_LOG] = Msg_FunctionHook;
    typeMap[Function::ORBIT_OUTPUT_DEBUG_STRING] =
        Msg_FunctionHookOutputDebugString;
    typeMap[Function::UNREAL_ACTOR] = Msg_FunctionHookUnrealActor;
    typeMap[Function::ALLOC] = Msg_FunctionHookAlloc;
    typeMap[Function::FREE] = Msg_FunctionHookFree;
    typeMap[Function::REALLOC] = Msg_FunctionHookRealloc;
    typeMap[Function::ORBIT_DATA] = Msg_FunctionHookOrbitData;
  }

  assert(typeMap.size() == Function::OrbitType::NUM_TYPES);

  return typeMap[a_type];
}

//-----------------------------------------------------------------------------
void Capture::PreFunctionHooks() {
  // Clear selected functions
  for (auto& selected_addresses : GSelectedAddressesByType) {
    selected_addresses.clear();
  }

  // Clear current argument tracking data
  GTcpServer->Send(Msg_ClearArgTracking);

  // Find OutputDebugStringA
  if (GParams.m_HookOutputDebugString) {
    if (DWORD64 outputAddr = GTargetProcess->GetOutputDebugStringAddress()) {
      GSelectedAddressesByType[Function::ORBIT_OUTPUT_DEBUG_STRING].push_back(
          outputAddr);
    }
  }

  // Find alloc/free functions
  GTargetProcess->FindCoreFunctions();

  // Unreal
  CheckForUnrealSupport();
}

std::vector<std::shared_ptr<Function>> Capture::GetSelectedFunctions() {
  std::vector<std::shared_ptr<Function>> selected_functions;
  for (auto& func : GTargetProcess->GetFunctions()) {
    if (func->IsSelected() || func->IsOrbitFunc()) {
      selected_functions.push_back(func);
    }
  }
  return selected_functions;
}

//-----------------------------------------------------------------------------
void Capture::SendFunctionHooks() {
  PreFunctionHooks();

  GSelectedFunctions = GetSelectedFunctions();

  for (auto& func : GSelectedFunctions) {
    uint64_t address = func->GetVirtualAddress();
    GSelectedAddressesByType[func->GetOrbitType()].push_back(address);
    GSelectedFunctionsMap[address] = func.get();
    func->ResetStats();
    GFunctionCountMap[address] = 0;
  }

  GVisibleFunctionsMap = GSelectedFunctionsMap;

  if (GClearCaptureDataFunc) {
    GClearCaptureDataFunc();
  }

  if (Capture::IsRemote()) {
    for (auto& function : GSelectedFunctions) {
      LOG("Send Selected Function: %s", function->PrettyName().c_str());
    }

    std::string selectedFunctionsData =
        SerializeObjectBinary(GSelectedFunctions);
    GTcpClient->Send(Msg_RemoteSelectedFunctionsMap,
                     selectedFunctionsData.data(),
                     selectedFunctionsData.size());

    Message msg(Msg_StartCapture);
    msg.m_Header.m_GenericHeader.m_Address = GTargetProcess->GetID();
    GTcpClient->Send(msg);
  }

  // Unreal
  if (GUnrealSupported) {
    const OrbitUnrealInfo& info = GOrbitUnreal.GetUnrealInfo();
    GTcpServer->Send(Msg_OrbitUnrealInfo, info);
  }

  // Send all hooks by type
  for (int i = 0; i < Function::NUM_TYPES; ++i) {
    std::vector<uint64_t>& addresses = GSelectedAddressesByType[i];
    if (!addresses.empty()) {
      MessageType msgType = GetMessageType(static_cast<Function::OrbitType>(i));
      GTcpServer->Send(msgType, addresses);
    }
  }
}

//-----------------------------------------------------------------------------
void Capture::TestHooks() {
  if (!GIsTesting) {
    GIsTesting = true;
    GFunctionIndex = 0;
    GTestTimer.Start();
  } else {
    GIsTesting = false;
  }
}

//-----------------------------------------------------------------------------
void Capture::StartSampling() {
#ifdef WIN32
  if (!GIsSampling && Capture::IsTrackingEvents() &&
      GTargetProcess->GetName().size()) {
    SCOPE_TIMER_LOG("Capture::StartSampling");

    GCaptureTimer.Start();
    GCaptureTimePoint = std::chrono::system_clock::now();

    ClearCaptureData();
    GTimerManager->StartRecording();
    GEventTracer.Start();

    GIsSampling = true;
  }
#endif
}

//-----------------------------------------------------------------------------
void Capture::StopSampling() {
  if (GIsSampling) {
    if (IsTrackingEvents()) {
#ifdef WIN32
      GEventTracer.Stop();
#endif
    }

    GTimerManager->StopRecording();
  }
}

//-----------------------------------------------------------------------------
bool Capture::IsCapturing() {
  return GTimerManager && GTimerManager->m_IsRecording;
}

//-----------------------------------------------------------------------------
TcpEntity* Capture::GetMainTcpEntity() {
  if (Capture::IsRemote()) {
    return GTcpClient.get();
  }
  return GTcpServer.get();
}

//-----------------------------------------------------------------------------
void Capture::Update() {
  if (GIsSampling) {
#ifdef WIN32
    if (GSamplingProfiler->ShouldStop()) {
      GSamplingProfiler->StopCapture();
    }
#endif

    if (GSamplingProfiler->GetState() == SamplingProfiler::DoneProcessing) {
      if (sampling_done_callback_ != nullptr) {
        sampling_done_callback_(GSamplingProfiler,
                                sampling_done_callback_user_data_);
      }
      GIsSampling = false;
    }
  }

  if (GPdbDbg) {
    GPdbDbg->Update();
  }

#ifdef WIN32
  if (!Capture::IsRemote() && GInjected && !GTcpServer->HasConnection()) {
    StopCapture();
    GInjected = false;
  }
#endif
}

//-----------------------------------------------------------------------------
void Capture::DisplayStats() {
  if (GSamplingProfiler) {
    TRACE_VAR(GSamplingProfiler->GetNumSamples());
  }
}

//-----------------------------------------------------------------------------
outcome::result<void, std::string> Capture::SaveSession(
    const std::string& filename) {
  Session session;
  session.m_ProcessFullPath = GTargetProcess->GetFullPath();

  GCoreApp->SendToUiNow("UpdateProcessParams");
  session.m_Arguments = GParams.m_Arguments;
  session.m_WorkingDirectory = GParams.m_WorkingDirectory;

  for (auto& func : GTargetProcess->GetFunctions()) {
    if (func->IsSelected()) {
      session.m_Modules[func->GetLoadedModulePath()].m_FunctionHashes.push_back(
          func->Hash());
    }
  }

  std::string filename_with_ext = filename;
  if (!absl::EndsWith(filename, ".opr")) {
    filename_with_ext += ".opr";
  }

  std::ofstream file(filename_with_ext, std::ios::binary);
  if (file.fail()) {
    ERROR("Saving session in \"%s\": %s", filename_with_ext, "file.fail()");
    return outcome::failure("Error opening the file for writing");
  }

  try {
    SCOPE_TIMER_LOG(
        absl::StrFormat("Saving session in \"%s\"", filename_with_ext));
    cereal::BinaryOutputArchive archive(file);
    archive(cereal::make_nvp("Session", session));
    return outcome::success();
  } catch (std::exception& e) {
    ERROR("Saving session in \"%s\": %s", filename_with_ext, e.what());
    return outcome::failure("Error serializing the session");
  }
}

//-----------------------------------------------------------------------------
void Capture::NewSamplingProfiler() {
  if (GSamplingProfiler) {
    // To prevent destruction while processing data...
    GOldSamplingProfilers.push_back(GSamplingProfiler);
  }

  Capture::GSamplingProfiler =
      std::make_shared<SamplingProfiler>(Capture::GTargetProcess);
}

//-----------------------------------------------------------------------------
bool Capture::IsTrackingEvents() {
#ifdef __linux
  return !IsRemote();
#else
  if (GTargetProcess->GetIsRemote() && !GTcpServer->IsLocalConnection()) {
    return false;
  }

  return GParams.m_TrackContextSwitches || GParams.m_TrackSamplingEvents;
#endif
}

//-----------------------------------------------------------------------------
bool Capture::IsRemote() {
  return GTargetProcess && GTargetProcess->GetIsRemote();
}

//-----------------------------------------------------------------------------
bool Capture::IsLinuxData() {
  bool isLinux = false;
#if __linux__
  isLinux = true;
#endif
  return IsRemote() || isLinux;
}

//-----------------------------------------------------------------------------
void Capture::RegisterZoneName(uint64_t a_ID, const char* a_Name) {
  GZoneNames[a_ID] = a_Name;
}

//-----------------------------------------------------------------------------
void Capture::AddCallstack(CallStack& a_CallStack) {
  ScopeLock lock(GCallstackMutex);
  Capture::GCallstacks[a_CallStack.m_Hash] =
      std::make_shared<CallStack>(a_CallStack);
}

//-----------------------------------------------------------------------------
std::shared_ptr<CallStack> Capture::GetCallstack(CallstackID a_ID) {
  ScopeLock lock(GCallstackMutex);

  auto it = Capture::GCallstacks.find(a_ID);
  if (it != Capture::GCallstacks.end()) {
    return it->second;
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
LinuxAddressInfo* Capture::GetAddressInfo(uint64_t address) {
  auto address_info_it = GAddressInfos.find(address);
  if (address_info_it == GAddressInfos.end()) {
    return nullptr;
  }
  return &address_info_it->second;
}

//-----------------------------------------------------------------------------
void Capture::CheckForUnrealSupport() {
  GUnrealSupported = GCoreApp != nullptr &&
                     GCoreApp->GetUnrealSupportEnabled() &&
                     GOrbitUnreal.HasFnameInfo();
}

//-----------------------------------------------------------------------------
void Capture::PreSave() {
  // Add selected functions' exact address to sampling profiler
  for (auto& pair : GSelectedFunctionsMap) {
    GSamplingProfiler->UpdateAddressInfo(pair.first);
  }
}

//-----------------------------------------------------------------------------
void Capture::TestRemoteMessages() { TestRemoteMessages::Get().Run(); }
