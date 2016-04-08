// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_EXECUTOR_IMPL_H_
#define STEXECUTOR_EXECUTOR_IMPL_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/scoped_handle.h"
#include "gen-cpp/Executor.h"
#include "stexecutor/executed_command_info.h"

class DllInjector;
class ExecutingEngine;
class ExecutorFactory;

class ExecutorImpl : public ExecutorIf {
 public:
  ExecutorImpl(
      DllInjector* dll_injector,
      ExecutingEngine* executing_engine,
      ExecutorFactory* executor_factory);
  void Initialize(
      const int32_t current_pid, const bool is_root_process) override;
  bool HookedCreateFile(
      const std::string& abs_path, const bool for_writing) override;
  void HookedRenameFile(
      const std::string& old_name_str,
      const std::string& new_name_str) override;
  void PushStdOutput(
      const StdHandles::type handle, const std::string& data) override;
  void OnBeforeProcessCreate(
      CacheHitInfo& result,
      const std::string& exe_path,
      const std::vector<std::string>& arguments,
      const std::string& startup_dir,
      const std::vector<std::string>& environment) override;
  void OnSuspendedProcessCreated(
      const int32_t child_pid,
      const int32_t executor_command_id,
      const bool append_std_streams) override;
  void OnFileDeleted(const std::string& abs_path) override;
  void FillExitCode();
  const ExecutedCommandInfo& command_info() const {
    return command_info_;
  }
  int command_id() const {
    return command_info_.command_id;
  }

 private:
  DllInjector* dll_injector_;
  ExecutingEngine* executing_engine_;
  ExecutorFactory* executor_factory_;
  ExecutedCommandInfo command_info_;
  base::ScopedHandle process_handle_;
  // List of Executors, related to parent processes, that share same
  // stdout and stderr handles.
  std::vector<std::shared_ptr<ExecutorImpl>>
      executors_should_append_std_streams_;
  // PushStdOutput can be called from alien thread because of
  // Child-Parent std streams sharing. So we use mutex to protect it.
  std::mutex std_handles_lock_;
};

#endif  // STEXECUTOR_EXECUTOR_IMPL_H_
