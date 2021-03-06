// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_EXECUTED_COMMAND_INFO_H_
#define STEXECUTOR_EXECUTED_COMMAND_INFO_H_

#include <string>
#include <vector>

#include "boost/filesystem.hpp"
#include "base/filesystem_utils.h"
#include "stexecutor/rules_mappers/file_info.h"

struct ExecutedCommandInfo {
  int exit_code = 0;
  int command_id = 0;
  bool has_errors = false;
  std::vector<rules_mappers::FileInfo> input_files;
  std::vector<rules_mappers::FileInfo> output_files;
  std::vector<boost::filesystem::path> removed_rel_paths;
  std::string result_stdout;
  std::string result_stderr;
};

#endif  // STEXECUTOR_EXECUTED_COMMAND_INFO_H_

