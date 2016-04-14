// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_RULES_MAPPERS_REDIS_REQUEST_RESULTS_H_
#define STEXECUTOR_RULES_MAPPERS_REDIS_REQUEST_RESULTS_H_

#include <vector>
#include <string>

#include "stexecutor/rules_mappers/request_results_base.h"

#include "stexecutor/rules_mappers/rules_hashing.h"


class BuildDirectoryState;
class RedisSyncClient;
class RedisValue;

namespace rules_mappers {

struct CachedExecutionResponse;

class RedisRequestResults : public RequestResultsBase {
 public:
  // NOTE: Client must ensure validity of RedisSyncClient till lifetime
  // of this object.
  RedisRequestResults(
      RedisSyncClient* redis_client,
      const HashValue& request_hash);
  ~RedisRequestResults();
  void AddRule(
      const std::vector<FileInfo>& input_files,
      std::unique_ptr<CachedExecutionResponse> response) override;
  std::shared_ptr<const CachedExecutionResponse> FindCachedResults(
      const BuildDirectoryState& build_dir_state,
      std::vector<FileInfo>* input_files) override;

 private:
  FileSet LoadFileSet(const std::string& file_set_hash);
  std::shared_ptr<CachedExecutionResponse> LoadExecutionResponse(
      const std::string& key);
  void SaveFileSet(const std::string& key, const FileSet& file_set);
  void SaveExecutionResponse(
      const std::string& key, const CachedExecutionResponse& response);

  RedisSyncClient* redis_client_;
  const HashValue request_hash_;
  std::vector<FileSet> file_sets_;
};

}  // namespace rules_mappers

#endif  // STEXECUTOR_RULES_MAPPERS_REDIS_REQUEST_RESULTS_H_
