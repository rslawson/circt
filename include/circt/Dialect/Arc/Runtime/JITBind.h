//===- JITBind.h - ArcRuntime JIT symbol binding helper -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declares the helper struct used to bind the IR interface to the MLIR
// Execution Engine without relying on the linker.
//
// This file is specific to the runtime implementation statically linked into
// the arcilator tool and not part of the runtime's API.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_ARC_RUNTIME_JITBIND_H
#define CIRCT_DIALECT_ARC_RUNTIME_JITBIND_H

#include "circt/Dialect/Arc/Runtime/Common.h"

namespace circt {
namespace arc {
namespace runtime {

// Forward-declare.
struct FmtDescriptor;

struct APICallbacks {
  uint8_t *(*fnAllocInstance)(const ArcRuntimeModelInfo *model,
                              const char *args);
  void (*fnDeleteInstance)(uint8_t *simState);
  void (*fnOnEval)(uint8_t *simState);
  void (*fnOnInitialized)(uint8_t *simState);
  void (*fnFormat)(const FmtDescriptor *fmt, ...);
  void (*fnFormatToStream)(uint8_t *stream, const FmtDescriptor *fmt, ...);
  uint8_t *(*fnGetStdoutStream)();
  uint8_t *(*fnGetStderrStream)();
  uint64_t *(*fnSwapTraceBuffer)(const uint8_t *simState);
  void (*fnDumpFile)(uint8_t *simState, const char *fileName);
  void (*fnDumpVars)(uint8_t *simState);
  uint8_t (*fnTestPlusArgs)(const char *prefix);
  uint8_t (*fnValuePlusArgs)(const char *format, uint64_t *value,
                             uint32_t numBits);
  void (*fnReadMem)(const char *fileName, uint8_t *memory, uint64_t numWords,
                    uint32_t wordBits, uint32_t wordStride, uint8_t isBinary,
                    uint8_t descending, int64_t lowAddress,
                    int64_t startAddress, int64_t finishAddress);
  int32_t (*fnFopen)(const char *fileName, int32_t mode);
  void (*fnFclose)(int32_t descriptor);
  uint8_t *(*fnGetFileStream)(int32_t descriptor);
  void (*fnFlushStream)(uint8_t *stream);
  void (*fnFlushAll)();
  const char *(*fnStringIntern)(const char *str);
  const char *(*fnStringFormat)(const FmtDescriptor *fmt, ...);
  const char *(*fnStringConcat)(const char *lhs, const char *rhs);
  int32_t (*fnStringLength)(const char *str);
  int32_t (*fnStringCompare)(const char *lhs, const char *rhs);
  const char *(*fnStringFromInt)(const uint64_t *value, uint32_t numBits);
  void (*fnStringToInt)(const char *str, uint64_t *value, uint32_t numBits);
  uint8_t (*fnStringGet)(const char *str, int32_t index);
  const char *(*fnStringSubstr)(const char *str, int32_t start, int32_t end);
  const char *(*fnStringChangeCase)(const char *str, uint8_t upper);
  uint8_t (*fnValuePlusArgsString)(const char *format, const char **value);

  static constexpr char symNameAllocInstance[] = "arcRuntimeIR_allocInstance";
  static constexpr char symNameDeleteInstance[] = "arcRuntimeIR_deleteInstance";
  static constexpr char symNameOnEval[] = "arcRuntimeIR_onEval";
  static constexpr char symNameOnInitialized[] = "arcRuntimeIR_onInitialized";
  static constexpr char symNameFormat[] = "arcRuntimeIR_format";
  static constexpr char symNameFormatToStream[] = "arcRuntimeIR_formatToStream";
  static constexpr char symNameGetStdoutStream[] =
      "arcRuntimeIR_getStdoutStream";
  static constexpr char symNameGetStderrStream[] =
      "arcRuntimeIR_getStderrStream";
  static constexpr char symNameSwapTraceBuffer[] =
      "arcRuntimeIR_swapTraceBuffer";
  static constexpr char symNameDumpFile[] = "arcRuntimeIR_dumpFile";
  static constexpr char symNameDumpVars[] = "arcRuntimeIR_dumpVars";
  static constexpr char symNameTestPlusArgs[] = "arcRuntimeIR_testPlusArgs";
  static constexpr char symNameValuePlusArgs[] = "arcRuntimeIR_valuePlusArgs";
  static constexpr char symNameReadMem[] = "arcRuntimeIR_readMem";
  static constexpr char symNameFopen[] = "arcRuntimeIR_fopen";
  static constexpr char symNameFclose[] = "arcRuntimeIR_fclose";
  static constexpr char symNameGetFileStream[] = "arcRuntimeIR_getFileStream";
  static constexpr char symNameFlushStream[] = "arcRuntimeIR_flushStream";
  static constexpr char symNameFlushAll[] = "arcRuntimeIR_flushAll";
  static constexpr char symNameStringIntern[] = "arcRuntimeIR_stringIntern";
  static constexpr char symNameStringFormat[] = "arcRuntimeIR_stringFormat";
  static constexpr char symNameStringConcat[] = "arcRuntimeIR_stringConcat";
  static constexpr char symNameStringLength[] = "arcRuntimeIR_stringLength";
  static constexpr char symNameStringCompare[] = "arcRuntimeIR_stringCompare";
  static constexpr char symNameStringFromInt[] = "arcRuntimeIR_stringFromInt";
  static constexpr char symNameStringToInt[] = "arcRuntimeIR_stringToInt";
  static constexpr char symNameStringGet[] = "arcRuntimeIR_stringGet";
  static constexpr char symNameStringSubstr[] = "arcRuntimeIR_stringSubstr";
  static constexpr char symNameStringChangeCase[] =
      "arcRuntimeIR_stringChangeCase";
  static constexpr char symNameValuePlusArgsString[] =
      "arcRuntimeIR_valuePlusArgsString";
};

#ifdef ARC_RUNTIME_JITBIND_FNDECL
const APICallbacks &getArcRuntimeAPICallbacks();
#endif // ARC_RUNTIME_JITBIND_FNDECL

} // namespace runtime
} // namespace arc
} // namespace circt

#endif // CIRCT_DIALECT_ARC_RUNTIME_JITBIND_H
