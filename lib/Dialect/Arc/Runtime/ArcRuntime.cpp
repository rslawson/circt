//===- ArcRuntime.cpp - Default implementation of the ArcRuntime-----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the default implementation of a runtime library for
// arcilator simulations.
//
//===----------------------------------------------------------------------===//

#define ARC_RUNTIME_ENABLE_EXPORT

#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "circt/Dialect/Arc/Runtime/Common.h"
#include "circt/Dialect/Arc/Runtime/FmtDescriptor.h"
#include "circt/Dialect/Arc/Runtime/IRInterface.h"
#include "circt/Dialect/Arc/Runtime/Internal.h"
#include "circt/Dialect/Arc/Runtime/ModelInstance.h"
#include "circt/Support/FormatInteger.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#ifdef ARC_RUNTIME_JIT_BIND
#define ARC_RUNTIME_JITBIND_FNDECL
#include "circt/Dialect/Arc/Runtime/JITBind.h"
#endif

#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace circt::arc::runtime;

static inline impl::ModelInstance *getModelInstance(const ArcState *instance) {
  assert(instance->impl != nullptr && "Instance is null");
  return reinterpret_cast<impl::ModelInstance *>(instance->impl);
}

ArcState *arcRuntimeAllocateInstance(const ArcRuntimeModelInfo *model,
                                     const char *args) {
  if (model->apiVersion != ARC_RUNTIME_API_VERSION) {
    impl::fatalError("API version mismatch.\nMake sure to use an ArcRuntime "
                     "release matching "
                     "the arcilator version used to build the hardware model.");
    return nullptr;
  }

  auto *statePtr = reinterpret_cast<ArcState *>(
      calloc(1, sizeof(ArcState) + model->numStateBytes));
  assert(reinterpret_cast<intptr_t>(&statePtr->modelState[0]) % 16 == 0 &&
         "Simulation state must be 16 byte aligned");
  statePtr->impl = new impl::ModelInstance(model, args, statePtr);
  statePtr->magic = ARC_RUNTIME_MAGIC;
  return statePtr;
}

void arcRuntimeDeleteInstance(ArcState *instance) {
  if (instance->impl)
    delete reinterpret_cast<impl::ModelInstance *>(instance->impl);
  free(instance);
}

uint64_t arcRuntimeGetAPIVersion() { return ARC_RUNTIME_API_VERSION; }

void arcRuntimeOnEval(ArcState *instance) {
  getModelInstance(instance)->onEval(instance);
}

void arcRuntimeOnInitialized(ArcState *instance) {
  getModelInstance(instance)->onInitialized(instance);
}

ArcState *arcRuntimeGetStateFromModelState(uint8_t *modelState,
                                           uint64_t offset) {
  if (!modelState)
    impl::fatalError("State pointer is null");
  uint8_t *modPtr = static_cast<uint8_t *>(modelState) - offset;
  ArcState *statePtr = reinterpret_cast<ArcState *>(modPtr - sizeof(ArcState));
  if (statePtr->magic != ARC_RUNTIME_MAGIC)
    impl::fatalError("Incorrect magic number for state");
  return statePtr;
}

// --- IR Exports ---

uint8_t *arcRuntimeIR_allocInstance(const ArcRuntimeModelInfo *model,
                                    const char *args) {
  ArcState *statePtr = arcRuntimeAllocateInstance(model, args);
  return statePtr->modelState;
}

void arcRuntimeIR_onEval(uint8_t *modelState) {
  arcRuntimeOnEval(arcRuntimeGetStateFromModelState(modelState, 0));
}

void arcRuntimeIR_onInitialized(uint8_t *modelState) {
  arcRuntimeOnInitialized(arcRuntimeGetStateFromModelState(modelState, 0));
}

void arcRuntimeIR_deleteInstance(uint8_t *modelState) {
  arcRuntimeDeleteInstance(arcRuntimeGetStateFromModelState(modelState, 0));
}

static void formatToStream(llvm::raw_ostream &os, const FmtDescriptor *fmt,
                           va_list args) {
  while (fmt->action != FmtDescriptor::Action_End) {
    switch (fmt->action) {
    case FmtDescriptor::Action_Literal: {
      std::string_view s(va_arg(args, const char *), fmt->literal.width);
      os << s;
      break;
    }
    case FmtDescriptor::Action_LiteralSmall: {
      std::string_view s(fmt->smallLiteral.data);
      os << s;
      break;
    }
    case FmtDescriptor::Action_Int: {
      uint64_t *words = va_arg(args, uint64_t *);
      int64_t numWords = llvm::divideCeil(fmt->intFmt.bitwidth, 64);
      llvm::APInt apInt(fmt->intFmt.bitwidth, llvm::ArrayRef(words, numWords));
      std::optional<int32_t> specifierWidth;
      if (fmt->intFmt.specifierWidth >= 0)
        specifierWidth = fmt->intFmt.specifierWidth;
      circt::formatInteger(os, apInt, fmt->intFmt.radix,
                           fmt->intFmt.isUpperCase, fmt->intFmt.isLeftAligned,
                           fmt->intFmt.paddingChar, specifierWidth,
                           fmt->intFmt.isSigned);
      break;
    }
    case FmtDescriptor::Action_Char:
      os << static_cast<char>(va_arg(args, int));
      break;
    case FmtDescriptor::Action_End:
      break;
    }
    fmt++;
  }
}

void arcRuntimeIR_format(const FmtDescriptor *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  formatToStream(llvm::outs(), fmt, args);

  va_end(args);
}

void arcRuntimeIR_formatToStream(uint8_t *stream, const FmtDescriptor *fmt,
                                 ...) {
  if (!stream)
    impl::fatalError("Output stream pointer is null");

  va_list args;
  va_start(args, fmt);

  formatToStream(*reinterpret_cast<llvm::raw_ostream *>(stream), fmt, args);

  va_end(args);
}

uint8_t *arcRuntimeIR_getStdoutStream() {
  return reinterpret_cast<uint8_t *>(&llvm::outs());
}

uint8_t *arcRuntimeIR_getStderrStream() {
  return reinterpret_cast<uint8_t *>(&llvm::errs());
}

uint64_t *arcRuntimeIR_swapTraceBuffer(const uint8_t *modelState) {
  auto *modPtr = static_cast<const uint8_t *>(modelState);
  auto *statePtr =
      reinterpret_cast<const ArcState *>(modPtr - sizeof(ArcState));
  if (statePtr->magic != ARC_RUNTIME_MAGIC)
    impl::fatalError("Incorrect magic number for state");
  return getModelInstance(statePtr)->swapTraceBuffer();
}

// --- Plusargs ---

static std::vector<std::string> &getPlusArgs() {
  static std::vector<std::string> plusArgs;
  return plusArgs;
}

void arcRuntimeSetPlusArgs(int numArgs, const char *const *args) {
  auto &plusArgs = getPlusArgs();
  plusArgs.clear();
  for (int i = 0; i < numArgs; ++i)
    if (args[i][0] == '+')
      plusArgs.emplace_back(args[i] + 1);
}

/// Find the first plusarg starting with the given prefix, and return the
/// remainder of it after the prefix.
static std::optional<llvm::StringRef> findPlusArg(llvm::StringRef prefix) {
  for (auto &arg : getPlusArgs())
    if (llvm::StringRef(arg).starts_with(prefix))
      return llvm::StringRef(arg).drop_front(prefix.size());
  return std::nullopt;
}

uint8_t arcRuntimeIR_testPlusArgs(const char *prefix) {
  return findPlusArg(prefix).has_value();
}

uint8_t arcRuntimeIR_valuePlusArgs(const char *format, uint64_t *value,
                                   uint32_t numBits) {
  // Split the format into the prefix and the format specifier, skipping any
  // field width.
  llvm::StringRef formatRef(format);
  auto percent = formatRef.find('%');
  if (percent == llvm::StringRef::npos)
    return 0;
  auto prefix = formatRef.take_front(percent);
  auto spec = formatRef.drop_front(percent + 1).ltrim("0123456789");
  if (spec.empty())
    return 0;
  auto arg = findPlusArg(prefix);
  if (!arg)
    return 0;

  // Parse the value according to the format specifier.
  llvm::APInt result(std::max(numBits, 1u), 0);
  switch (std::tolower(spec[0])) {
  case 'd':
  case 'h':
  case 'x':
  case 'o':
  case 'b': {
    unsigned radix = 10;
    switch (std::tolower(spec[0])) {
    case 'h':
    case 'x':
      radix = 16;
      break;
    case 'o':
      radix = 8;
      break;
    case 'b':
      radix = 2;
      break;
    }
    auto digits = arg->trim();
    bool negative = radix == 10 && digits.consume_front("-");
    llvm::APInt parsed;
    if (digits.getAsInteger(radix, parsed))
      parsed = llvm::APInt(1, 0);
    result = parsed.zextOrTrunc(result.getBitWidth());
    if (negative)
      result.negate();
    break;
  }
  case 'e':
  case 'f':
  case 'g': {
    double real = 0;
    if (arg->trim().getAsDouble(real))
      real = 0;
    result = llvm::APInt(64, llvm::bit_cast<uint64_t>(real))
                 .zextOrTrunc(result.getBitWidth());
    break;
  }
  case 's': {
    // Pack the characters into the integer, with the last character in the
    // least significant bits.
    for (char c : *arg) {
      result <<= 8;
      result |= llvm::APInt(result.getBitWidth(), static_cast<uint8_t>(c));
    }
    break;
  }
  default:
    return 0;
  }

  // Write the result out as 64 bit words.
  for (unsigned i = 0, e = llvm::divideCeil(numBits, 64); i < e; ++i)
    value[i] = result.extractBitsAsZExtValue(
        std::min(64u, result.getBitWidth() - i * 64), i * 64);
  return 1;
}

void arcRuntimeIR_dumpFile(uint8_t *modelState, const char *fileName) {
  auto *statePtr = arcRuntimeGetStateFromModelState(modelState, 0);
  getModelInstance(statePtr)->setDumpFile(fileName);
}

void arcRuntimeIR_dumpVars(uint8_t *modelState) {
  auto *statePtr = arcRuntimeGetStateFromModelState(modelState, 0);
  getModelInstance(statePtr)->startDump(statePtr);
}

#ifdef ARC_RUNTIME_JIT_BIND
namespace circt::arc::runtime {

static const APICallbacks apiCallbacksGlobal{
    &arcRuntimeIR_allocInstance,   &arcRuntimeIR_deleteInstance,
    &arcRuntimeIR_onEval,          &arcRuntimeIR_onInitialized,
    &arcRuntimeIR_format,          &arcRuntimeIR_formatToStream,
    &arcRuntimeIR_getStdoutStream, &arcRuntimeIR_getStderrStream,
    &arcRuntimeIR_swapTraceBuffer, &arcRuntimeIR_dumpFile,
    &arcRuntimeIR_dumpVars,        &arcRuntimeIR_testPlusArgs,
    &arcRuntimeIR_valuePlusArgs};

const APICallbacks &getArcRuntimeAPICallbacks() { return apiCallbacksGlobal; }

} // namespace circt::arc::runtime
#endif
