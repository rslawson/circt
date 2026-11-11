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
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#ifdef ARC_RUNTIME_JIT_BIND
#define ARC_RUNTIME_JITBIND_FNDECL
#include "circt/Dialect/Arc/Runtime/JITBind.h"
#endif

#include <array>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
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
    case FmtDescriptor::Action_String: {
      const char *str = va_arg(args, const char *);
      llvm::StringRef s = str ? llvm::StringRef(str) : llvm::StringRef();
      int32_t padding = std::max<int32_t>(
          fmt->stringFmt.specifierWidth - static_cast<int32_t>(s.size()), 0);
      if (!fmt->stringFmt.isLeftAligned)
        os << std::string(padding, fmt->stringFmt.paddingChar);
      os << s;
      if (fmt->stringFmt.isLeftAligned)
        os << std::string(padding, fmt->stringFmt.paddingChar);
      break;
    }
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

uint8_t arcRuntimeSimulationFailed() {
  return impl::ModelInstance::simulationFailed;
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

// --- Dynamic Strings ---

/// Return a stable, interned copy of the given string.
static const char *internString(llvm::StringRef str) {
  static std::unordered_set<std::string> strings;
  return strings.emplace(str).first->c_str();
}

/// Return a view of a dynamic string, which may be null.
static llvm::StringRef getString(const char *str) {
  return str ? llvm::StringRef(str) : llvm::StringRef();
}

const char *arcRuntimeIR_stringIntern(const char *str) {
  return internString(getString(str));
}

const char *arcRuntimeIR_stringFormat(const FmtDescriptor *fmt, ...) {
  std::string result;
  llvm::raw_string_ostream os(result);
  va_list args;
  va_start(args, fmt);
  formatToStream(os, fmt, args);
  va_end(args);
  return internString(result);
}

const char *arcRuntimeIR_stringConcat(const char *lhs, const char *rhs) {
  return internString((getString(lhs) + getString(rhs)).str());
}

int32_t arcRuntimeIR_stringLength(const char *str) {
  return getString(str).size();
}

int32_t arcRuntimeIR_stringCompare(const char *lhs, const char *rhs) {
  return getString(lhs).compare(getString(rhs));
}

const char *arcRuntimeIR_stringFromInt(const uint64_t *value,
                                       uint32_t numBits) {
  if (numBits == 0)
    return internString("");
  unsigned numBytes = llvm::divideCeil(numBits, 8);
  llvm::APInt bits(numBits,
                   llvm::ArrayRef(value, llvm::divideCeil(numBits, 64)));
  bits = bits.zext(numBytes * 8);
  std::string result;
  for (unsigned i = numBytes; i > 0; --i)
    if (auto c = static_cast<char>(bits.extractBitsAsZExtValue(8, (i - 1) * 8)))
      result.push_back(c);
  return internString(result);
}

void arcRuntimeIR_stringToInt(const char *str, uint64_t *value,
                              uint32_t numBits) {
  llvm::APInt result(std::max(numBits, 1u), 0);
  for (char c : getString(str)) {
    result <<= 8;
    result |= llvm::APInt(result.getBitWidth(), static_cast<uint8_t>(c));
  }
  for (unsigned i = 0, e = llvm::divideCeil(numBits, 64); i < e; ++i)
    value[i] = result.extractBitsAsZExtValue(
        std::min(64u, result.getBitWidth() - i * 64), i * 64);
}

uint8_t arcRuntimeIR_stringGet(const char *str, int32_t index) {
  auto s = getString(str);
  if (index < 0 || static_cast<size_t>(index) >= s.size())
    return 0;
  return s[index];
}

const char *arcRuntimeIR_stringSubstr(const char *str, int32_t start,
                                      int32_t end) {
  auto s = getString(str);
  size_t clampedStart = std::max<int32_t>(start, 0);
  size_t clampedEnd = std::max<int32_t>(end, 0);
  if (clampedEnd <= clampedStart)
    return internString("");
  return internString(s.slice(clampedStart, clampedEnd));
}

const char *arcRuntimeIR_stringChangeCase(const char *str, uint8_t upper) {
  return internString(upper ? getString(str).upper() : getString(str).lower());
}

uint8_t arcRuntimeIR_valuePlusArgsString(const char *format,
                                         const char **value) {
  llvm::StringRef formatRef(format);
  auto percent = formatRef.find('%');
  if (percent == llvm::StringRef::npos)
    return 0;
  auto arg = findPlusArg(formatRef.take_front(percent));
  if (!arg)
    return 0;
  *value = internString(*arg);
  return 1;
}

// --- Memory Loading ---

void arcRuntimeIR_readMem(const char *fileName, uint8_t *memory,
                          uint64_t numWords, uint32_t wordBits,
                          uint32_t wordStride, uint8_t isBinary,
                          uint8_t descending, int64_t lowAddress,
                          int64_t startAddress, int64_t finishAddress) {
  auto buffer = llvm::MemoryBuffer::getFile(fileName, /*IsText=*/true);
  if (!buffer) {
    std::cerr << "WARNING: $readmem" << (isBinary ? 'b' : 'h')
              << ": cannot open file \"" << fileName << "\"" << std::endl;
    return;
  }

  // Determine the range of addresses to load and the direction.
  int64_t highAddress = lowAddress + static_cast<int64_t>(numWords) - 1;
  int64_t address = startAddress >= 0 ? startAddress : lowAddress;
  int64_t lastAddress = finishAddress >= 0 ? finishAddress : highAddress;
  int64_t step = lastAddress < address ? -1 : 1;
  unsigned radix = isBinary ? 2 : 16;
  unsigned numBytes = (wordBits + 7) / 8;

  llvm::StringRef text = (*buffer)->getBuffer();
  while (!text.empty()) {
    // Skip whitespace and comments.
    text = text.ltrim();
    if (text.consume_front("//")) {
      text = text.drop_until([](char c) { return c == '\n'; });
      continue;
    }
    if (text.consume_front("/*")) {
      auto end = text.find("*/");
      text = end == llvm::StringRef::npos ? llvm::StringRef()
                                          : text.drop_front(end + 2);
      continue;
    }
    if (text.empty())
      break;

    // Extract the next token.
    auto token = text.take_until([](char c) { return std::isspace(c); });
    text = text.drop_front(token.size());

    // Handle address specifications, which are always hexadecimal.
    if (token.consume_front("@")) {
      uint64_t newAddress;
      if (token.getAsInteger(16, newAddress)) {
        std::cerr << "WARNING: $readmem: invalid address in file \"" << fileName
                  << "\"" << std::endl;
        return;
      }
      address = static_cast<int64_t>(newAddress);
      continue;
    }

    // Parse the word. Unknown bits are loaded as zeros, and underscores are
    // ignored.
    std::string digits;
    for (char c : token) {
      if (c == '_')
        continue;
      if (c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?')
        c = '0';
      digits.push_back(c);
    }
    llvm::APInt word;
    if (llvm::StringRef(digits).getAsInteger(radix, word)) {
      std::cerr << "WARNING: $readmem: invalid word \"" << token.str()
                << "\" in file \"" << fileName << "\"" << std::endl;
      return;
    }
    word = word.zextOrTrunc(std::max(wordBits, 1u));

    // Store the word if its address is within the memory and the range to be
    // loaded.
    if (address < lowAddress || address > highAddress)
      return;
    auto index = descending ? address - lowAddress : highAddress - address;
    auto *ptr = memory + index * wordStride;
    for (unsigned i = 0; i < numBytes; ++i)
      ptr[i] = word.extractBitsAsZExtValue(
          std::min(8u, word.getBitWidth() - i * 8), i * 8);
    if (address == lastAddress)
      return;
    address += step;
  }
}

// --- File I/O ---

namespace {
/// The files opened through `$fopen`. File descriptors have the most
/// significant bit set, and index into `files` with the remaining bits, offset
/// by the three predefined descriptors for stdin, stdout, and stderr.
/// Multichannel descriptors have a bit set for each channel, where bit 0 is
/// stdout and the remaining bits index into `channels`.
struct FileTable {
  std::vector<std::unique_ptr<llvm::raw_fd_ostream>> files;
  std::array<std::unique_ptr<llvm::raw_fd_ostream>, 31> channels;
};
} // namespace

static FileTable &getFileTable() {
  static FileTable table;
  return table;
}

static constexpr uint32_t kFileDescriptorBit = 0x80000000;
static constexpr uint32_t kNumPredefinedFiles = 3;

int32_t arcRuntimeIR_fopen(const char *fileName, int32_t mode) {
  // Reading files is not supported.
  if (mode == 0 || mode == 3)
    return 0;
  auto flags = llvm::sys::fs::OF_None;
  if (mode == 2 || mode == 5)
    flags = llvm::sys::fs::OF_Append;
  std::error_code ec;
  auto stream = std::make_unique<llvm::raw_fd_ostream>(fileName, ec, flags);
  if (ec)
    return 0;

  auto &table = getFileTable();
  if (mode < 0) {
    // Allocate a channel of a multichannel descriptor.
    for (unsigned i = 0; i < table.channels.size(); ++i) {
      if (table.channels[i])
        continue;
      table.channels[i] = std::move(stream);
      return 1 << (i + 1);
    }
    return 0;
  }

  // Allocate a file descriptor, reusing closed ones.
  unsigned index = 0;
  while (index < table.files.size() && table.files[index])
    ++index;
  if (index == table.files.size())
    table.files.emplace_back();
  table.files[index] = std::move(stream);
  return kFileDescriptorBit | (index + kNumPredefinedFiles);
}

/// Get the stream slot of a descriptor, or null if the descriptor does not
/// refer to a file opened through `arcRuntimeIR_fopen`.
static std::unique_ptr<llvm::raw_fd_ostream> *getFileSlot(uint32_t descriptor) {
  auto &table = getFileTable();
  if (descriptor & kFileDescriptorBit) {
    auto index = descriptor & ~kFileDescriptorBit;
    if (index < kNumPredefinedFiles ||
        index - kNumPredefinedFiles >= table.files.size())
      return nullptr;
    return &table.files[index - kNumPredefinedFiles];
  }
  for (unsigned i = 0; i < table.channels.size(); ++i)
    if (descriptor & (1u << (i + 1)))
      return &table.channels[i];
  return nullptr;
}

void arcRuntimeIR_fclose(int32_t descriptor) {
  if (auto *slot = getFileSlot(descriptor))
    slot->reset();
}

uint8_t *arcRuntimeIR_getFileStream(int32_t descriptor) {
  uint32_t desc = descriptor;
  llvm::raw_ostream *stream = &llvm::nulls();
  if (desc == (kFileDescriptorBit | 1) || desc == 1)
    stream = &llvm::outs();
  else if (desc == (kFileDescriptorBit | 2))
    stream = &llvm::errs();
  else if (!(desc & kFileDescriptorBit) && (desc & 1))
    stream = &llvm::outs();
  else if (auto *slot = getFileSlot(desc); slot && *slot)
    stream = slot->get();
  return reinterpret_cast<uint8_t *>(stream);
}

void arcRuntimeIR_flushStream(uint8_t *stream) {
  reinterpret_cast<llvm::raw_ostream *>(stream)->flush();
}

void arcRuntimeIR_flushAll() {
  llvm::outs().flush();
  llvm::errs().flush();
  auto &table = getFileTable();
  for (auto &file : table.files)
    if (file)
      file->flush();
  for (auto &file : table.channels)
    if (file)
      file->flush();
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

static const APICallbacks apiCallbacksGlobal{&arcRuntimeIR_allocInstance,
                                             &arcRuntimeIR_deleteInstance,
                                             &arcRuntimeIR_onEval,
                                             &arcRuntimeIR_onInitialized,
                                             &arcRuntimeIR_format,
                                             &arcRuntimeIR_formatToStream,
                                             &arcRuntimeIR_getStdoutStream,
                                             &arcRuntimeIR_getStderrStream,
                                             &arcRuntimeIR_swapTraceBuffer,
                                             &arcRuntimeIR_dumpFile,
                                             &arcRuntimeIR_dumpVars,
                                             &arcRuntimeIR_testPlusArgs,
                                             &arcRuntimeIR_valuePlusArgs,
                                             &arcRuntimeIR_readMem,
                                             &arcRuntimeIR_fopen,
                                             &arcRuntimeIR_fclose,
                                             &arcRuntimeIR_getFileStream,
                                             &arcRuntimeIR_flushStream,
                                             &arcRuntimeIR_flushAll,
                                             &arcRuntimeIR_stringIntern,
                                             &arcRuntimeIR_stringFormat,
                                             &arcRuntimeIR_stringConcat,
                                             &arcRuntimeIR_stringLength,
                                             &arcRuntimeIR_stringCompare,
                                             &arcRuntimeIR_stringFromInt,
                                             &arcRuntimeIR_stringToInt,
                                             &arcRuntimeIR_stringGet,
                                             &arcRuntimeIR_stringSubstr,
                                             &arcRuntimeIR_stringChangeCase,
                                             &arcRuntimeIR_valuePlusArgsString};

const APICallbacks &getArcRuntimeAPICallbacks() { return apiCallbacksGlobal; }

} // namespace circt::arc::runtime
#endif
