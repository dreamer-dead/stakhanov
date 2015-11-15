// Copyright 2015 The "Stakhanov" project authors. All rights reserved.
// Use of this source code is governed by a GPLv2 license that can be
// found in the LICENSE file.

#ifndef STHOOK_FUNCTIONS_INTERCEPTOR_H_
#define STHOOK_FUNCTIONS_INTERCEPTOR_H_

#include <windows.h>

#include <string>
#include <vector>
#include <unordered_map>

#include "log4cplus/logger.h"

namespace sthook {

// Intercepts set of function calls from all modules (except excluded) to the
// specified dll.
class FunctionsInterceptor {
 public:
  // Map from function name in intercepted_dll to new address.
  typedef std::unordered_map<std::string, void*> InterceptedFunctions;
  FunctionsInterceptor();
  ~FunctionsInterceptor();
  bool Hook(const std::string& intercepted_dll,
            const InterceptedFunctions& intercepts,
            HMODULE excluded_module);
  void Unhook();
  // Called to patch IAT on newly loaded module.
  void PatchIAT(HMODULE module);

 private:
  struct PatchInformation {
    void** patched_address;
    void* old_value;
    void* new_value;
  };

  std::vector<HMODULE> GetLoadedModules();
  void FillOrdinalToName(HMODULE module);
  const IMAGE_OPTIONAL_HEADER32* GetPEOptionalHeader(const uint8_t* image_base);
  const IMAGE_DATA_DIRECTORY* GetImageExportDir(const uint8_t* image_base);
  template<typename ImportDescriptorType>
  void HookImportDirectory(
      const uint8_t* base_address,
      const IMAGE_DATA_DIRECTORY& import_directory);
  void HookImportDescriptor(
      const uint8_t* base_address,
      const IMAGE_THUNK_DATA* name_table,
      const IMAGE_THUNK_DATA* address_table);
  void Patch(void** dest, void* val, bool remember);

  std::string intercepted_dll_;
  InterceptedFunctions intercepts_;
  std::vector<PatchInformation> patches_;
  std::unordered_map<uint32_t, std::string> ordinal_to_name_;
  bool hooked_;

  log4cplus::Logger logger_;
};

}  // namespace sthook
#endif  // STHOOK_FUNCTIONS_INTERCEPTOR_H_