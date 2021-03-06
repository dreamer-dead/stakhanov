// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_EXECUTING_ENGINE_H_
#define STEXECUTOR_EXECUTING_ENGINE_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/scoped_handle.h"
#include "boost/filesystem.hpp"
#include "stexecutor/executed_command_info.h"

class BuildDirectoryState;
class CumulativeExecutionResponseBuilder;
class ProcessCreationRequest;
class ProcessCreationResponse;
class ProcessManagementConfig;
class FilesStorage;

namespace rules_mappers {
class RulesMapper;
struct CachedExecutionResponse;
struct FileInfo;
}

// Thread-safe class, incapsulating main decision-making
// logic about executing child commands.
class ExecutingEngine {
 public:
  enum SpecialCommandIds {
    kInvalidCommandId =-1,
    kCacheHitCommandId,
    kRootCommandId,
    // Must be the last member of enum.
    kFirstUserCommandId
  };

  ExecutingEngine(
      std::unique_ptr<FilesStorage> files_storage,
      std::unique_ptr<rules_mappers::RulesMapper> rules_mapper,
      std::unique_ptr<BuildDirectoryState> build_dir_state,
      std::unique_ptr<ProcessManagementConfig> process_management_config);
  ~ExecutingEngine();

  ProcessCreationResponse AttemptCacheExecute(
      int parent_command_id,
      const ProcessCreationRequest& resuest);
  void SaveCommandResults(const ExecutedCommandInfo& command_info);
  void AssociatePIDWithCommandId(
      int32_t pid, int child_command_id,
      bool should_append_std_streams,
      bool* do_not_track);
  void RegisterByPID(
      int32_t pid,
      int* command_id,
      std::vector<int>* command_ids_should_append_std_streams,
      bool* is_safe_to_use_hoax_proxy,
      bool* should_buffer_std_streams,
      bool* should_ignore_output_files);

  FilesStorage* files_storage() const {
    return files_storage_.get();
  }

  BuildDirectoryState* build_dir_state() const {
    return build_dir_state_.get();
  }

 private:
  std::mutex instance_lock_;

  void CompleteCumulativeResponse(CumulativeExecutionResponseBuilder* builder);
  ProcessCreationResponse HandleCacheMissUnderInstanceLock(
      CumulativeExecutionResponseBuilder* parent_builder,
      int command_id,
      bool new_request_ignores_std_streams_from_children,
      const ProcessCreationRequest& process_creation_request);

  CumulativeExecutionResponseBuilder* GetResponseBuilder(int command_id) {
    auto it = active_commands_.find(command_id);
    if (it != active_commands_.end())
      return it->second.get();
    return nullptr;
  }

  std::unique_ptr<FilesStorage> files_storage_;
  std::unique_ptr<rules_mappers::RulesMapper> rules_mapper_;
  std::unique_ptr<BuildDirectoryState> build_dir_state_;
  std::unique_ptr<ProcessManagementConfig> process_management_config_;
  // Command is "active" if either it process is not completed yet, or any
  // of commands, corresponding to it child processes is "active"
  std::unordered_map<
      int, std::unique_ptr<CumulativeExecutionResponseBuilder>>
          active_commands_;

  std::unordered_map<int32_t, int> pid_to_unassigned_command_id_;
  int next_command_id_;
};

#endif  // STEXECUTOR_EXECUTING_ENGINE_H_
