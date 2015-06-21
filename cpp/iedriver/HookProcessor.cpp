// Licensed to the Software Freedom Conservancy (SFC) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The SFC licenses this file
// to you under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "HookProcessor.h"
#include <ctime>
#include <vector>
#include <Sddl.h>
#include "logging.h"

#define MAX_BUFFER_SIZE 32768
#define NAMED_PIPE_BUFFER_SIZE 1024
#define LOW_INTEGRITY_SDDL_SACL L"S:(ML;;NW;;;LW)"
#define PIPE_CONNECTION_TIMEOUT_IN_MILLISECONDS 5000

// Define a shared data segment.  Variables in this segment can be
// shared across processes that load this DLL.
#pragma data_seg("SHARED")
int data_buffer_size = MAX_BUFFER_SIZE;
char data_buffer[MAX_BUFFER_SIZE];
#pragma data_seg()

#pragma comment(linker, "/section:SHARED,RWS")

namespace webdriver {

class CopyDataHolderWindow : public CWindowImpl<CopyDataHolderWindow> {
 public:
  DECLARE_WND_CLASS(L"CopyDataHolderWindow")
  BEGIN_MSG_MAP(CopyDataHolderWindow)
  END_MSG_MAP()

  LRESULT CopyData(HWND destination_window_handle,
                   int data_size,
                   void* pointer_to_data) {
    if (data_size > data_buffer_size) {
      LOG(WARN) << "Destination data buffer not large enough";
    }
    
    // Copy the contents of local memory into a temporary structure
    // for transport across the process boundary.
    std::vector<char> buffer(data_size);
    memcpy_s(&buffer[0], data_size, pointer_to_data, data_size);

    // Send the data across using SendMessage with the WM_COPYDATA
    // message. N.B., the window procedure in the other process *must*
    // have a handler for the WM_COPYDATA message, which copies the 
    // content from the message payload into a local buffer. The
    // HookProcessor::CopyDataToBuffer method provides a common
    // implementation to copy the data into the shared buffer location.
    COPYDATASTRUCT data;
    data.dwData = 1;
    data.cbData = static_cast<int>(buffer.size());
    data.lpData = &buffer[0];
    LRESULT result = ::SendMessage(destination_window_handle,
                                   WM_COPYDATA,
                                   reinterpret_cast<WPARAM>(this->m_hWnd),
                                   reinterpret_cast<LPARAM>(&data));
    return result;
  }
};

HookProcessor::HookProcessor() {
  this->window_handle_ = NULL;
  this->hook_procedure_handle_ = NULL;
  this->pipe_handle_ = NULL;
  this->communication_type_ = OneWay;
}

HookProcessor::~HookProcessor() {
  this->Dispose();
}

void HookProcessor::Initialize(const std::string& hook_procedure_name,
                               const int hook_procedure_type) {
  HookSettings hook_settings;
  hook_settings.hook_procedure_name = hook_procedure_name;
  hook_settings.hook_procedure_type = hook_procedure_type;
  hook_settings.window_handle = NULL;
  hook_settings.communication_type = OneWay;
  this->Initialize(hook_settings);
}

void HookProcessor::Initialize(const HookSettings& settings) {
  this->window_handle_ = settings.window_handle;
  this->pipe_handle_ = INVALID_HANDLE_VALUE;
  this->communication_type_ = settings.communication_type;
  if (settings.communication_type == TwoWay) {
    this->CreateReturnPipe();
  }
  bool is_hook_installed = this->InstallWindowsHook(settings.hook_procedure_name,
                                                    settings.hook_procedure_type);
}

void HookProcessor::CreateReturnPipe() {
  // Set security descriptor so low-integrity processes can write to it.
  PSECURITY_DESCRIPTOR pointer_to_descriptor;
  ::ConvertStringSecurityDescriptorToSecurityDescriptor(LOW_INTEGRITY_SDDL_SACL,
                                                        SDDL_REVISION_1,
                                                        &pointer_to_descriptor,
                                                        NULL);
  SECURITY_ATTRIBUTES security_attributes;
  security_attributes.lpSecurityDescriptor = pointer_to_descriptor;
  security_attributes.nLength = sizeof(security_attributes);
  this->pipe_handle_ = ::CreateNamedPipe(L"\\\\.\\pipe\\nhsupspipe",
                                         PIPE_ACCESS_DUPLEX,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                         PIPE_UNLIMITED_INSTANCES,
                                         NAMED_PIPE_BUFFER_SIZE,
                                         0,
                                         0,
                                         &security_attributes);

  // failed to create pipe?
  if (this->pipe_handle_ == INVALID_HANDLE_VALUE) {
    LOG(WARN) << "Failed to create named pipe. Communication back from browser will not work.";
  }
}

void HookProcessor::Dispose() {
  ClearBuffer();

  if (this->pipe_handle_ != NULL) {
    ::CloseHandle(this->pipe_handle_);
    this->pipe_handle_ = NULL;
  }

  if (this->hook_procedure_handle_ != NULL) {
    this->UninstallWindowsHook();
    this->hook_procedure_handle_ = NULL;
  }
}

bool HookProcessor::InstallWindowsHook(const std::string& hook_proc_name,
                                       const int hook_proc_type) {
  LOG(TRACE) << "Entering HookProcessor::InstallWindowsHook";

  HINSTANCE instance_handle = _AtlBaseModule.GetModuleInstance();

  FARPROC hook_procedure_address = ::GetProcAddress(instance_handle,
                                                    hook_proc_name.c_str());
  if (hook_procedure_address == NULL) {
    LOGERR(WARN) << "Unable to get address of hook procedure named "
                 << hook_proc_name;
    return false;
  }
  HOOKPROC hook_procedure = reinterpret_cast<HOOKPROC>(hook_procedure_address);

  // Install the Windows hook.
  DWORD thread_id = 0;
  if (this->window_handle_ != NULL) {
    thread_id = ::GetWindowThreadProcessId(this->window_handle_, NULL);
  }
  this->hook_procedure_handle_ = ::SetWindowsHookEx(hook_proc_type,
                                                    hook_procedure,
                                                    instance_handle,
                                                    thread_id);
  if (this->hook_procedure_handle_ == NULL) {      
    LOGERR(WARN) << "Unable to set windows hook";
    return false;
  }
  return true;
}

void HookProcessor::UninstallWindowsHook() {
  LOG(TRACE) << "Entering HookProcessor::UninstallWindowsHook";
  if (this->hook_procedure_handle_ != NULL) {
    ::UnhookWindowsHookEx(this->hook_procedure_handle_);
    ::CloseHandle(this->hook_procedure_handle_);
  }
}

bool HookProcessor::PushData(int data_size,
                             void* pointer_to_data) {
  LOG(TRACE) << "Entering HookProcessor::PushData";
  if (this->hook_procedure_handle_ == NULL) {      
    LOG(WARN) << "No hook procedure has been set";
    return false;
  }
  CopyDataHolderWindow holder;
  holder.Create(/*HWND*/ HWND_MESSAGE,
                /*_U_RECT rect*/ CWindow::rcDefault,
                /*LPCTSTR szWindowName*/ NULL,
                /*DWORD dwStyle*/ NULL,
                /*DWORD dwExStyle*/ NULL,
                /*_U_MENUorID MenuOrID*/ 0U,
                /*LPVOID lpCreateParam*/ NULL);
  LRESULT result = holder.CopyData(this->window_handle_,
                                   data_size,
                                   pointer_to_data);
  LOG(INFO) << "SendMessage result? " << result;
  holder.DestroyWindow();
  return true;
}

bool HookProcessor::PushData(const std::wstring& data) {
  std::wstring mutable_data = data;
  return this->PushData(static_cast<int>(mutable_data.size() * sizeof(wchar_t)), &mutable_data[0]);
}

int HookProcessor::PullData(std::vector<char>* data) {
  std::vector<char> buffer(NAMED_PIPE_BUFFER_SIZE);

   // Wait for the client to connect; if it succeeds, 
   // the function returns a nonzero value. If the function
   // returns zero, GetLastError returns ERROR_PIPE_CONNECTED. 
  bool is_connected = ::ConnectNamedPipe(this->pipe_handle_, NULL) ? true : (::GetLastError() == ERROR_PIPE_CONNECTED);
  if (is_connected) {
    unsigned long bytes_read = 0;
    bool is_read_successful = ::ReadFile(this->pipe_handle_,
                                         &buffer[0],
                                         NAMED_PIPE_BUFFER_SIZE,
                                         &bytes_read,
                                         NULL);
    while (!is_read_successful && ERROR_MORE_DATA == ::GetLastError()) {
      data->insert(data->end(), buffer.begin(), buffer.begin() + bytes_read);
      is_read_successful = ::ReadFile(this->pipe_handle_,
                                      &buffer[0],
                                      NAMED_PIPE_BUFFER_SIZE,
                                      &bytes_read,
                                      NULL);
    }
    if (is_read_successful) {
      data->insert(data->end(), buffer.begin(), buffer.begin() + bytes_read);
    }
  }
  return static_cast<int>(data->size());
}

int HookProcessor::GetDataBufferSize() {
  return data_buffer_size;
}

void HookProcessor::SetDataBufferSize(int size) {
  data_buffer_size = size;
}

void HookProcessor::CopyDataToBuffer(int source_data_size, void* source) {
  // clear the shared buffer before putting data into it
  ClearBuffer();
  if (source_data_size < data_buffer_size) {
    data_buffer_size = source_data_size;
  }
  memcpy_s(data_buffer,
           data_buffer_size,
           source,
           data_buffer_size);
}

void HookProcessor::CopyDataFromBuffer(int destination_data_size, void* destination) {
  if (data_buffer_size >= destination_data_size) {
    destination_data_size = data_buffer_size;
  }
  memcpy_s(destination, destination_data_size, data_buffer, destination_data_size); 
  // clear the shared buffer after taking data out of it.
  ClearBuffer();
}

void HookProcessor::CopyWStringToBuffer(const std::wstring& data) {
  std::wstring mutable_data = data;
  int mutable_data_size = static_cast<int>(mutable_data.size() * sizeof(wchar_t));
  CopyDataToBuffer(mutable_data_size, &mutable_data[0]);
}

std::wstring HookProcessor::CopyWStringFromBuffer() {
  // Allocate a buffer of wchar_t the length of the data in the
  // shared memory buffer, plus one extra wide char, so that we
  // can null terminate.
  int local_buffer_size = GetDataBufferSize() + sizeof(wchar_t);
  std::vector<wchar_t> local_buffer(local_buffer_size / sizeof(wchar_t));

  // Copy the data from the shared memory buffer, and force
  // a terminating null char into the local vector, then 
  // convert to wstring.
  CopyDataFromBuffer(local_buffer_size, &local_buffer[0]);
  local_buffer[local_buffer.size() - 1] = L'\0';
  std::wstring data = &local_buffer[0];
  return data;
}

void HookProcessor::ClearBuffer() {
  // Zero out the shared buffer
  data_buffer_size = MAX_BUFFER_SIZE;
  memset(data_buffer, 0, MAX_BUFFER_SIZE);
}

void HookProcessor::WriteDataToPipe(const int process_id, const int data_size, void* data) {
  HANDLE pipe_handle = INVALID_HANDLE_VALUE;

  // Create the named pipe handle. Retry up until a timeout to give
  // the driver end of the pipe time to start listening for a connection.
  BOOL is_pipe_available = ::WaitNamedPipe(L"\\\\.\\pipe\\nhsupspipe", PIPE_CONNECTION_TIMEOUT_IN_MILLISECONDS);
  if (is_pipe_available) {
    pipe_handle = ::CreateFile(L"\\\\.\\pipe\\nhsupspipe",
                               GENERIC_READ | GENERIC_WRITE,
                               0, 
                               NULL, 
                               OPEN_EXISTING,
                               0,
                               NULL);
    DWORD last_error = ::GetLastError();
  }

  // if everything ok set mode to message mode
  if (INVALID_HANDLE_VALUE != pipe_handle) {
    DWORD pipe_mode = PIPE_READMODE_MESSAGE;
    // if this fails bail out
    if (::SetNamedPipeHandleState(pipe_handle, &pipe_mode, NULL, NULL)) {
      unsigned long bytes_written = 0;
      bool is_write_successful = ::WriteFile(pipe_handle,
                                             data,
                                             data_size,
                                             &bytes_written,
                                             NULL);
    }
    ::CloseHandle(pipe_handle); 
  }
}

} // namespace webdriver
