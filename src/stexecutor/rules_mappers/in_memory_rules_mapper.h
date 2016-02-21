// Copyright 2016 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STEXECUTOR_RULES_MAPPERS_IN_MEMORY_RULES_MAPPER_H_
#define STEXECUTOR_RULES_MAPPERS_IN_MEMORY_RULES_MAPPER_H_

#include <unordered_map>
#include <vector>

#include "stexecutor/rules_mappers/rules_mapper.h"
#include "stexecutor/rules_mappers/rules_hashing.h"

namespace rules_mappers {

class InMemoryRequestResults;

class InMemoryRulesMapper : public RulesMapper {
 public:
  InMemoryRulesMapper();
  ~InMemoryRulesMapper();

  const CachedExecutionResponse* FindCachedResults(
      const ProcessCreationRequest& process_creation_request,
      const BuildDirectoryState& build_dir_state) override;
  void AddRule(
      const ProcessCreationRequest& process_creation_request,
      std::vector<FileInfo> input_files,
      std::unique_ptr<CachedExecutionResponse> response) override;

 private:
  static HashValue ComputeProcessCreationHash(
      const ProcessCreationRequest& process_creation_request);
  std::unordered_map<
      HashValue,
      std::unique_ptr<InMemoryRequestResults>,
      HashValueHasher> rules_;
};

}  // namespace rules_mappers

#endif  // STEXECUTOR_RULES_MAPPERS_IN_MEMORY_RULES_MAPPER_H_