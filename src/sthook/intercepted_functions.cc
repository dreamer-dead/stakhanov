// Copyright 2015 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#include "sthook/intercepted_functions.h"

#include <windows.h>

#include <codecvt>
#include <memory>
#include <string>
#include <utility>

#include "base/filesystem_utils.h"
#include "base/scoped_handle.h"
#include "base/string_utils.h"
#include "base/filesystem_utils_win.h"
#include "boost/filesystem.hpp"
#include "gen-cpp/Executor.h"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"
#include "sthook/functions_interceptor.h"
#include "sthook/intercept_helper.h"
#include "sthook/sthook_communication.h"
#include "sthook/std_handles_holder.h"
#include "stproxy/stproxy_communication.h"
#include "thrift/protocol/TBinaryProtocol.h"
#include "thrift/transport/TBufferTransports.h"
#include "thrift/transport/TSocket.h"

namespace {

const wchar_t kStProxyExeName[] = L"stproxy.exe";
const wchar_t kStLaunchExeName[] = L"stlaunch.exe";

typedef BOOL (WINAPI *LPCREATE_PROCESS_W)(
    LPCWSTR application_name,
    LPWSTR command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    LPCWSTR current_directory,
    LPSTARTUPINFOW startup_info,
    LPPROCESS_INFORMATION process_information);

typedef BOOL (WINAPI *LPCREATE_PROCESS_A)(
    LPCSTR application_name,
    LPSTR command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    LPCSTR current_directory,
    LPSTARTUPINFOA startup_info,
    LPPROCESS_INFORMATION process_information);

typedef BOOL (WINAPI *LPWRITE_FILE)(
    HANDLE file,
    LPCVOID buffer,
    DWORD number_of_bytes_to_write,
    LPDWORD number_of_bytes_written,
    LPOVERLAPPED overlapped
);

typedef BOOL (WINAPI *LPWRITE_FILE_EX)(
    HANDLE file,
    LPCVOID buffer,
    DWORD number_of_bytes_to_write,
    LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE completion_routine
);

typedef BOOL (WINAPI *LPSET_STD_HANDLE)(
    DWORD handle_id,
    HANDLE handle
);

typedef BOOL (WINAPI *LPWRITE_CONSOLE)(
    HANDLE console_output,
    const VOID *buffer,
    DWORD number_of_chars_to_write,
    LPDWORD number_of_chars_written,
    LPVOID reserved
);

typedef BOOL (WINAPI *LPDUPLICATE_HANDLE)(
    HANDLE source_process_handle,
    HANDLE source_handle,
    HANDLE target_process_handle,
    LPHANDLE target_handle,
    DWORD desired_access,
    BOOL inherit_handle,
    DWORD options
);


#define FOR_EACH_INTERCEPTS(DO_IT) \
    DO_IT(CloseHandle, nullptr, &AfterCloseHandle, BOOL, HANDLE)


log4cplus::Logger logger_ = log4cplus::Logger::getRoot();

LPCREATE_PROCESS_W g_original_CreateProcessW;
LPCREATE_PROCESS_A g_original_CreateProcessA;
LPWRITE_FILE g_original_WriteFile;
LPWRITE_FILE_EX g_original_WriteFileEx;
LPWRITE_CONSOLE g_original_WriteConsoleA;
LPWRITE_CONSOLE g_original_WriteConsoleW;
LPSET_STD_HANDLE g_original_SetStdHandle;
LPDUPLICATE_HANDLE g_original_DuplicateHandle;
std::wstring g_stproxy_path;

std::wstring InitStProxyPath(HMODULE current_module) {
  std::vector<wchar_t> buffer(MAX_PATH + 1);
  GetModuleFileName(current_module, &buffer[0], MAX_PATH);
  buffer[MAX_PATH] = L'\0';
  boost::filesystem::path cur_dll_path(&buffer[0]);
  boost::filesystem::path result =
      cur_dll_path.parent_path() / kStProxyExeName;
  return result.native();
}

sthook::FunctionsInterceptor* GetInterceptor() {
  // Will leak, but it seems to be less evil then strange crashes
  // during ExitProcess in case when some of patched modules already
  // unloaded.
  static sthook::FunctionsInterceptor* g_interceptor;
  if (!g_interceptor)
    g_interceptor = new sthook::FunctionsInterceptor();
  return g_interceptor;
}

ExecutorIf* GetExecutor() {
  using apache::thrift::TException;
  using apache::thrift::protocol::TBinaryProtocol;
  using apache::thrift::transport::TBufferedTransport;
  using apache::thrift::transport::TSocket;

  static std::unique_ptr<ExecutorClient> g_executor;
  static bool g_initialize_finished;
  if (!g_executor) {
    try {
      boost::shared_ptr<TSocket> socket(new TSocket(
          "localhost", sthook::GetExecutorPort()));
      boost::shared_ptr<TBufferedTransport> transport(
          new TBufferedTransport(socket));
      boost::shared_ptr<TBinaryProtocol> protocol(
          new TBinaryProtocol(transport));
      g_executor.reset(new ExecutorClient(protocol));
      transport->open();
      g_initialize_finished = true;
    } catch (TException& ex) {
      LOG4CPLUS_FATAL(logger_, "Thrift initialization failure " << ex.what());
      g_executor.reset();
      throw;
    }
  }
  if (!g_initialize_finished)
    return nullptr;
  return g_executor.get();
}


NTSTATUS NTAPI NewNtCreateFile(
    PHANDLE file_handle,
    ACCESS_MASK desired_access,
    POBJECT_ATTRIBUTES object_attributes,
    PIO_STATUS_BLOCK io_status_block,
    PLARGE_INTEGER allocation_size,
    ULONG file_attributes,
    ULONG share_access,
    ULONG create_disposition,
    ULONG create_options,
    PVOID ea_buffer,
    ULONG ea_length) {
  NTSTATUS result = NtCreateFile(
      file_handle,
      desired_access,
      object_attributes,
      io_status_block,
      allocation_size,
      file_attributes,
      share_access,
      create_disposition,
      create_options,
      ea_buffer,
      ea_length);
  if (!NT_SUCCESS(result))
     return result;
  if (object_attributes && object_attributes->ObjectName) {
    ExecutorIf* executor = GetExecutor();
    if (executor) {
      // May be if initialization not finished yet. Although we call
      // Initialize() during DllMain, opening Executor communication
      // internlally causes NtCreateFile call, so avoid recursion.
      std::wstring name_str = base::StringFromAPIString(
          *object_attributes->ObjectName);
      static const wchar_t kPossiblePrefix1[] = L"\\??\\";
      static const wchar_t kPossiblePrefix2[] = L"\\\\?\\";
      if (object_attributes->RootDirectory) {
        std::wstring dir_path = base::GetFilePathFromHandle(
            object_attributes->RootDirectory);
        if (dir_path[dir_path.length() - 1] != L'\\')
          dir_path += L"\\";
        name_str = dir_path + name_str;
      }
      if (name_str.find(kPossiblePrefix1) == 0)
        name_str = name_str.substr(wcslen(kPossiblePrefix1));
      if (name_str.find(kPossiblePrefix2) == 0)
        name_str = name_str.substr(wcslen(kPossiblePrefix2));
      if (name_str[name_str.length() - 1] == L'\\')
        name_str = name_str.substr(0, name_str.length() - 1);
      name_str = base::ToLongPathName(name_str);
      DWORD attributes = ::GetFileAttributesW(name_str.c_str());
      // It is OK for attributes to be invalid, for newly created file.
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        // Don't care about directories.
        std::string abs_path_utf8 = base::ToUTF8FromWide(name_str);
        executor->HookedCreateFile(
            abs_path_utf8,
            (desired_access & GENERIC_WRITE) != 0);
      }
    }
  }
  return result;
}

extern "C" __kernel_entry NTSTATUS NTAPI LdrLoadDll(
    PWCHAR path_to_file,
    ULONG* flags,
    PUNICODE_STRING module_file_name,
    PHANDLE module_handle);

NTSTATUS NTAPI NewLdrLoadDll(
    PWCHAR path_to_file,
    ULONG* flags,
    PUNICODE_STRING module_file_name,
    PHANDLE module_handle) {
  NTSTATUS result = LdrLoadDll(
      path_to_file,
      flags,
      module_file_name,
      module_handle);
  if (module_handle && *module_handle) {
    GetInterceptor()->NewModuleLoaded(static_cast<HMODULE>(*module_handle));
  }
  return result;
}

std::vector<std::wstring> SplitCommandLineWide(const wchar_t* command_line) {
  // TODO(vchigrin): May be it worth to use some undocumented
  // functions from Visual C++ CRT to avoid dependency from shell32.dll
  int argc = 0;
  WCHAR** argv = CommandLineToArgvW(command_line, &argc);
  if (!argv) {
    DWORD error = GetLastError();
    LOG4CPLUS_ERROR(logger_, "CommandLineToArgv fails, error " << error);
    return std::vector<std::wstring>();
  }
  std::vector<std::wstring> result;
  result.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    result.push_back(argv[i]);
  }
  LocalFree(argv);
  return result;
}

template<typename CHAR_TYPE>
std::vector<std::wstring> SplitCommandLine(
    const CHAR_TYPE* command_line);

std::vector<std::wstring> SplitCommandLine(const wchar_t* command_line) {
  return SplitCommandLineWide(command_line);
}

std::vector<std::wstring> SplitCommandLine(const char* command_line) {
  // TODO(vchigrin): This is terribly inefficient shit-code since uses a LOT
  // of encoding convertions.
  std::wstring command_line_wide = base::ToWideFromANSI(command_line);
  return SplitCommandLineWide(command_line_wide.c_str());
}

std::vector<std::string> GetEnvironmentStringsAsUTF8() {
  const wchar_t* env_block = GetEnvironmentStringsW();
  if (!env_block) {
    DWORD error = GetLastError();
    LOG4CPLUS_ERROR(logger_, "GetEnvironmentStringsW failed, error " << error);
    return std::vector<std::string>();
  }
  std::vector<std::string> result;
  while (*env_block) {
    std::wstring env_string = env_block;
    env_block += (env_string.length() + 1);
    result.push_back(base::ToUTF8FromWide(env_string));
  }
  return result;
}

base::ScopedHandle PrepareFileMapping(const CacheHitInfo& cache_hit_info) {
  DWORD required_file_mapping_size =
      sizeof(STPROXY_SECTION_HEADER) +
      static_cast<DWORD>(cache_hit_info.result_stdout.length() +
                         cache_hit_info.result_stderr.length());
  SECURITY_ATTRIBUTES security_attributes = {0};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.lpSecurityDescriptor = NULL;
  security_attributes.bInheritHandle = TRUE;
  base::ScopedHandle file_mapping_handle(CreateFileMapping(
      INVALID_HANDLE_VALUE,
      &security_attributes,
      PAGE_READWRITE,
      0,
      required_file_mapping_size,
      NULL));
  if (!file_mapping_handle.IsValid()) {
    DWORD error_code = GetLastError();
    LOG4CPLUS_ERROR(logger_, "CreateFileMapping failed, error " << error_code);
    return base::ScopedHandle();
  }
  void* file_mapping_data = MapViewOfFile(
      file_mapping_handle.Get(),
      FILE_MAP_WRITE,
      0, 0,
      required_file_mapping_size);
  if (!file_mapping_data) {
    DWORD error_code = GetLastError();
    LOG4CPLUS_ERROR(logger_, "MapViewOfFile failed, error " << error_code);
    return base::ScopedHandle();
  }
  STPROXY_SECTION_HEADER* header = static_cast<STPROXY_SECTION_HEADER*>(
      file_mapping_data);
  header->exit_code = cache_hit_info.exit_code;
  header->stdout_byte_size = static_cast<DWORD>(
      cache_hit_info.result_stdout.length());
  header->stderr_byte_size = static_cast<DWORD>(
      cache_hit_info.result_stderr.length());
  uint8_t* dest_data = static_cast<uint8_t*>(file_mapping_data);
  dest_data += sizeof(STPROXY_SECTION_HEADER);
  memcpy(dest_data,
         cache_hit_info.result_stdout.c_str(),
         cache_hit_info.result_stdout.length());
  dest_data += cache_hit_info.result_stdout.length();
  memcpy(dest_data,
         cache_hit_info.result_stderr.c_str(),
         cache_hit_info.result_stderr.length());
  UnmapViewOfFile(file_mapping_data);
  return file_mapping_handle;
}

bool CreateProxyProcess(
    const CacheHitInfo& cache_hit_info,
    DWORD creation_flags,
    HANDLE std_input_handle,
    HANDLE std_output_handle,
    HANDLE std_error_handle,
    PROCESS_INFORMATION* process_information) {
  base::ScopedHandle file_mapping_handle = PrepareFileMapping(cache_hit_info);
  if (!file_mapping_handle.IsValid())
    return false;
  std::wostringstream buffer;
  buffer << std::hex << file_mapping_handle.Get();
  std::wstring handle_str = buffer.str();
  std::vector<wchar_t> command_line;
  // stproxy_path + 1 space + file_mapping_handle + zero terminator.
  command_line.reserve(g_stproxy_path.length() + 1 + handle_str.length() + 1);
  std::copy(
      g_stproxy_path.begin(), g_stproxy_path.end(),
      std::back_inserter(command_line));
  command_line.push_back(L' ');
  std::copy(
      handle_str.begin(), handle_str.end(),
      std::back_inserter(command_line));
  command_line.push_back(L'\0');
  STARTUPINFO startup_info = {0};
  startup_info.cb = sizeof(startup_info);
  startup_info.hStdInput = std_input_handle;
  startup_info.hStdOutput = std_output_handle;
  startup_info.hStdError = std_error_handle;
  BOOL result = g_original_CreateProcessW(
        NULL,
        &command_line[0],
        NULL,
        NULL,
        TRUE,  // Inherit handles
        creation_flags & ~EXTENDED_STARTUPINFO_PRESENT,
        NULL,
        NULL,
        &startup_info,
        process_information);
  if (!result) {
    DWORD error = GetLastError();
    LOG4CPLUS_ERROR(logger_, "Failed  create proxy process, error " << error);
  }
  return result;
}

template<typename CHAR_TYPE, typename STARTUPINFO_TYPE, typename FUNCTION>
BOOL CreateProcessImpl(
    FUNCTION actual_function,
    const CHAR_TYPE* application_name,
    CHAR_TYPE* command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    const CHAR_TYPE* current_directory,
    STARTUPINFO_TYPE* startup_info,
    LPPROCESS_INFORMATION process_information) {
  bool request_suspended = (creation_flags & CREATE_SUSPENDED) != 0;
  creation_flags |= CREATE_SUSPENDED;

  std::string exe_path;
  if (application_name) {
    exe_path = base::AbsPathUTF8(
        std::basic_string<CHAR_TYPE>(application_name));
  }
  std::vector<std::string> arguments_utf8;
  if (command_line) {
    std::vector<std::wstring> arguments = SplitCommandLine(command_line);
    for (const std::wstring& argument: arguments) {
      // TODO(vchigrin): Is it OK to convert everything to long path?
      std::wstring native_long_path = base::ToLongPathName(argument);
      arguments_utf8.push_back(base::ToUTF8FromWide(native_long_path));
    }
    if (exe_path.empty() && !arguments.empty())
      exe_path = base::AbsPathUTF8(arguments[0]);
  }
  std::string startup_dir_utf8;
  if (current_directory) {
    startup_dir_utf8 = base::ToUTF8(
        base::ToLongPathName(std::basic_string<CHAR_TYPE>(current_directory)));
  } else {
    boost::filesystem::path current_dir = boost::filesystem::current_path();
    startup_dir_utf8 = base::ToUTF8FromWide(current_dir.wstring());
  }

  // TODO(vchigrin): Analyze in executor, if we can use cached
  // results of this invokation, create some dummy process driver
  // instead of actual process creation.
  CacheHitInfo cache_hit_info;
  GetExecutor()->OnBeforeProcessCreate(
      cache_hit_info,
      exe_path,
      arguments_utf8,
      startup_dir_utf8,
      GetEnvironmentStringsAsUTF8());
  BOOL result = FALSE;
  if (cache_hit_info.cache_hit) {
    result = CreateProxyProcess(
        cache_hit_info,
        creation_flags,
        startup_info->hStdInput,
        startup_info->hStdOutput,
        startup_info->hStdError,
        process_information);
  } else {
    result = actual_function(
        application_name,
        command_line,
        process_attributes,
        thread_attributes,
        inherit_handles,
        creation_flags,
        environment,
        current_directory,
        startup_info,
        process_information);
  }
  if (!result)
    return result;
  if (!cache_hit_info.cache_hit) {
    // Cache hits should not go through all pipeline with
    // injecting interceptor DLLs, tracking files, etc.
    GetExecutor()->OnSuspendedProcessCreated(
        process_information->dwProcessId,
        cache_hit_info.executor_command_id);
  }
  if (!request_suspended)
    ResumeThread(process_information->hThread);
  return result;
}


BOOL WINAPI NewCreateProcessA(
    LPCSTR application_name,
    LPSTR command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    LPCSTR current_directory,
    LPSTARTUPINFOA startup_info,
    LPPROCESS_INFORMATION process_information) {
  return CreateProcessImpl(
      g_original_CreateProcessA,
      application_name,
      command_line,
      process_attributes,
      thread_attributes,
      inherit_handles,
      creation_flags,
      environment,
      current_directory,
      startup_info,
      process_information);
}

BOOL WINAPI NewCreateProcessW(
    LPCWSTR application_name,
    LPWSTR command_line,
    LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit_handles,
    DWORD creation_flags,
    LPVOID environment,
    LPCWSTR current_directory,
    LPSTARTUPINFOW startup_info,
    LPPROCESS_INFORMATION process_information) {
  return CreateProcessImpl(
      g_original_CreateProcessW,
      application_name,
      command_line,
      process_attributes,
      thread_attributes,
      inherit_handles,
      creation_flags,
      environment,
      current_directory,
      startup_info,
      process_information);
}

BOOL WINAPI NewWriteFile(
    HANDLE file,
    LPCVOID buffer,
    DWORD number_of_bytes_to_write,
    LPDWORD number_of_bytes_written,
    LPOVERLAPPED overlapped) {
  StdHandles::type handle_type = StdHandles::StdOutput;
  StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
  if (instance && instance->IsStdHandle(file, &handle_type)) {
    std::string data(
        static_cast<const char*>(buffer),
        number_of_bytes_to_write);
    GetExecutor()->PushStdOutput(handle_type, data);
  }
  return g_original_WriteFile(
      file,
      buffer,
      number_of_bytes_to_write,
      number_of_bytes_written,
      overlapped);
}

BOOL WINAPI NewWriteFileEx(
    HANDLE file,
    LPCVOID buffer,
    DWORD number_of_bytes_to_write,
    LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE completion_routine) {
  StdHandles::type handle_type = StdHandles::StdOutput;
  StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
  if (instance && instance->IsStdHandle(file, &handle_type)) {
    std::string data(
        static_cast<const char*>(buffer),
        number_of_bytes_to_write);
    GetExecutor()->PushStdOutput(handle_type, data);
  }
  return g_original_WriteFileEx(
      file,
      buffer,
      number_of_bytes_to_write,
      overlapped,
      completion_routine);
}

BOOL WINAPI NewWriteConsoleA(
    HANDLE console_output,
    const VOID *buffer,
    DWORD number_of_chars_to_write,
    LPDWORD number_of_chars_written,
    LPVOID reserved) {
  StdHandles::type handle_type = StdHandles::StdOutput;
  StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
  if (instance && instance->IsStdHandle(console_output, &handle_type)) {
    std::string data(
        static_cast<const char*>(buffer),
        number_of_chars_to_write * sizeof(CHAR));
    GetExecutor()->PushStdOutput(handle_type, data);
  }
  return g_original_WriteConsoleA(
      console_output,
      buffer,
      number_of_chars_to_write,
      number_of_chars_written,
      reserved);
}

BOOL WINAPI NewWriteConsoleW(
    HANDLE console_output,
    const VOID *buffer,
    DWORD number_of_chars_to_write,
    LPDWORD number_of_chars_written,
    LPVOID reserved) {
  StdHandles::type handle_type = StdHandles::StdOutput;
  StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
  if (instance && instance->IsStdHandle(console_output, &handle_type)) {
    // TODO(vchigrin): Is it OK to cache code-page dependent output data?
    // May be we should convert to UTF-8 all output, both from
    // WriteConsole and WriteFile?
    DWORD code_page = GetConsoleOutputCP();
    std::wstring wide_buffer(
        static_cast<const wchar_t*>(buffer), number_of_chars_to_write);
    std::string ansi_string = base::ToANSIFromWide(
        wide_buffer,
        code_page);
    GetExecutor()->PushStdOutput(handle_type, ansi_string);
  }
  return g_original_WriteConsoleW(
      console_output,
      buffer,
      number_of_chars_to_write,
      number_of_chars_written,
      reserved);
}

BOOL WINAPI NewSetStdHandle(
    DWORD handle_id,
    HANDLE handle_val) {
  BOOL result = g_original_SetStdHandle(handle_id, handle_val);
  if (result &&
     (handle_id == STD_OUTPUT_HANDLE || handle_id == STD_ERROR_HANDLE)) {
    StdHandles::type handle_type = (handle_id == STD_OUTPUT_HANDLE ?
        StdHandles::StdOutput : StdHandles::StdError);
    StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
    if (instance)
      instance->SetStdHandle(handle_type, handle_val);
  }
  return result;
}

BOOL WINAPI NewDuplicateHandle(
    HANDLE source_process_handle,
    HANDLE source_handle,
    HANDLE target_process_handle,
    LPHANDLE target_handle,
    DWORD desired_access,
    BOOL inherit_handle,
    DWORD options) {
  const BOOL result = g_original_DuplicateHandle(
      source_process_handle,
      source_handle,
      target_process_handle,
      target_handle,
      desired_access,
      inherit_handle,
      options);
  if (!result)
    return result;
  const HANDLE current_process = GetCurrentProcess();
  StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
  if (source_process_handle == current_process &&
     target_process_handle == current_process &&
     instance) {
    instance->MarkDuplicatedHandle(source_handle, *target_handle);
    if (options & DUPLICATE_CLOSE_SOURCE)
      instance->MarkHandleClosed(source_handle);
  }
  return result;
}

void AfterCloseHandle(BOOL result, HANDLE handle) {
  if (result) {
    StdHandlesHolder* instance = StdHandlesHolder::GetInstance();
    if (instance)
      instance->MarkHandleClosed(handle);
  }
}

}  // namespace

namespace sthook {

#define DEFINE_INTERCEPT(function_name, before_call, after_call, result, ...) \
extern InterceptHelperData function_name##_InterceptData = { \
    #function_name, before_call, after_call };

  FOR_EACH_INTERCEPTS(DEFINE_INTERCEPT)
#undef DEFINE_INTERCEPT

REGISTRATION_PTR g_intercepts_table[] = {
#define DEFINE_REGISER(function_name, before_call, after_call, result, ...) \
  InterceptHelper< \
      &function_name##_InterceptData, result, __VA_ARGS__>::Register,
  FOR_EACH_INTERCEPTS(DEFINE_REGISER)
#undef DEFINE_INTERCEPT
};

bool InstallHooks(HMODULE current_module) {
  FunctionsInterceptor::DllInterceptedFunctions ntdll_intercepts;
  // Intercepted either from kernelbase.dll on system that has it or
  // from kernel32.dll on older systems. We must hook kernelbase since some
  // processes link directly with it, skipping kernel32.dll (e.g. cmd.exe).
  FunctionsInterceptor::DllInterceptedFunctions kernel_intercepts;
  ntdll_intercepts.insert(std::make_pair("NtCreateFile", &NewNtCreateFile));
  ntdll_intercepts.insert(std::make_pair("LdrLoadDll", &NewLdrLoadDll));
  kernel_intercepts.insert(
      std::make_pair("CreateProcessA", &NewCreateProcessA));
  kernel_intercepts.insert(
      std::make_pair("CreateProcessW", &NewCreateProcessW));
  kernel_intercepts.insert(
      std::make_pair("WriteFile", &NewWriteFile));
  kernel_intercepts.insert(
      std::make_pair("WriteFileEx", &NewWriteFileEx));
  kernel_intercepts.insert(
      std::make_pair("WriteConsoleA", &NewWriteConsoleA));
  kernel_intercepts.insert(
      std::make_pair("WriteConsoleW", &NewWriteConsoleW));
  kernel_intercepts.insert(
      std::make_pair("SetStdHandle", &NewSetStdHandle));
  kernel_intercepts.insert(
      std::make_pair("DuplicateHandle", &NewDuplicateHandle));
  FunctionsInterceptor::Intercepts intercepts;
  intercepts.insert(
      std::pair<std::string, FunctionsInterceptor::DllInterceptedFunctions>(
          "ntdll.dll", ntdll_intercepts));
  HMODULE kernel_module = GetModuleHandleA("kernel32.dll");
  for (size_t i = 0;
      i < sizeof(g_intercepts_table) / sizeof(g_intercepts_table[0]); ++i) {
    g_intercepts_table[i](kernel_module, &kernel_intercepts);
  }

  intercepts.insert(
      std::pair<std::string, FunctionsInterceptor::DllInterceptedFunctions>(
          "kernel32.dll", kernel_intercepts));
  LOG4CPLUS_ASSERT(logger_, kernel_module);
  g_original_CreateProcessW = reinterpret_cast<LPCREATE_PROCESS_W>(
      GetProcAddress(kernel_module, "CreateProcessW"));
  g_original_CreateProcessA = reinterpret_cast<LPCREATE_PROCESS_A>(
      GetProcAddress(kernel_module, "CreateProcessA"));
  g_original_WriteFile = reinterpret_cast<LPWRITE_FILE>(
      GetProcAddress(kernel_module, "WriteFile"));
  g_original_WriteFileEx = reinterpret_cast<LPWRITE_FILE_EX>(
      GetProcAddress(kernel_module, "WriteFileEx"));
  g_original_WriteConsoleA = reinterpret_cast<LPWRITE_CONSOLE>(
      GetProcAddress(kernel_module, "WriteConsoleA"));
  g_original_WriteConsoleW = reinterpret_cast<LPWRITE_CONSOLE>(
      GetProcAddress(kernel_module, "WriteConsoleW"));
  g_original_SetStdHandle = reinterpret_cast<LPSET_STD_HANDLE>(
      GetProcAddress(kernel_module, "SetStdHandle"));
  g_original_DuplicateHandle = reinterpret_cast<LPDUPLICATE_HANDLE>(
      GetProcAddress(kernel_module, "DuplicateHandle"));
  LOG4CPLUS_ASSERT(logger_, g_original_CreateProcessW);
  LOG4CPLUS_ASSERT(logger_, g_original_CreateProcessA);
  LOG4CPLUS_ASSERT(logger_, g_original_WriteFile);
  LOG4CPLUS_ASSERT(logger_, g_original_WriteFileEx);
  LOG4CPLUS_ASSERT(logger_, g_original_WriteConsoleA);
  LOG4CPLUS_ASSERT(logger_, g_original_WriteConsoleW);
  LOG4CPLUS_ASSERT(logger_, g_original_SetStdHandle);
  LOG4CPLUS_ASSERT(logger_, g_original_DuplicateHandle);
  g_stproxy_path = InitStProxyPath(current_module);
  return GetInterceptor()->Hook(intercepts, current_module);
}

void Initialize() {
  boost::filesystem::path current_exe = base::GetCurrentExecutablePath();
  const bool is_st_launch = (current_exe.filename() == kStLaunchExeName);
  GetExecutor()->Initialize(GetCurrentProcessId(), is_st_launch);
  StdHandlesHolder::Initialize();
}

}  // namespace sthook
