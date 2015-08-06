/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_FRONTEND_PPC_TRANSLATOR_H_
#define XENIA_FRONTEND_PPC_TRANSLATOR_H_

#include <memory>

#include "xenia/base/string_buffer.h"
#include "xenia/cpu/backend/assembler.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/function.h"

namespace xe {
namespace cpu {
namespace frontend {

class PPCFrontend;
class PPCHIRBuilder;
class PPCScanner;

class PPCTranslator {
 public:
  PPCTranslator(PPCFrontend* frontend);
  ~PPCTranslator();

  bool Translate(GuestFunction* function, uint32_t debug_info_flags);

 private:
  void DumpSource(GuestFunction* function, StringBuffer* string_buffer);

  PPCFrontend* frontend_;
  std::unique_ptr<PPCScanner> scanner_;
  std::unique_ptr<PPCHIRBuilder> builder_;
  std::unique_ptr<compiler::Compiler> compiler_;
  std::unique_ptr<backend::Assembler> assembler_;

  StringBuffer string_buffer_;
};

}  // namespace frontend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_FRONTEND_PPC_TRANSLATOR_H_
