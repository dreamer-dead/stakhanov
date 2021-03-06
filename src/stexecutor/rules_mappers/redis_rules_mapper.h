// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_RULES_MAPPERS_REDIS_RULES_MAPPER_H_
#define STEXECUTOR_RULES_MAPPERS_REDIS_RULES_MAPPER_H_

#include <memory>
#include <string>
#include <vector>

#include "stexecutor/rules_mappers/rules_mapper_base.h"
#include "stexecutor/rules_mappers/rules_hashing.h"

class RedisClientPool;

namespace rules_mappers {

class RedisRulesMapper : public RulesMapperBase {
 public:
  explicit RedisRulesMapper(
      const std::shared_ptr<RedisClientPool> redis_client_pool);
  ~RedisRulesMapper();

  std::unique_ptr<CachedExecutionResponse> FindCachedResults(
      const ProcessCreationRequest& process_creation_request,
      const BuildDirectoryState& build_dir_state,
      std::vector<FileInfo>* input_files) override;
  void AddRule(
      const ProcessCreationRequest& process_creation_request,
      const std::vector<FileInfo>& input_files,
      std::unique_ptr<CachedExecutionResponse> response) override;

 private:
  std::shared_ptr<RedisClientPool> redis_client_pool_;
};

}  // namespace rules_mappers

#endif  // STEXECUTOR_RULES_MAPPERS_REDIS_RULES_MAPPER_H_

