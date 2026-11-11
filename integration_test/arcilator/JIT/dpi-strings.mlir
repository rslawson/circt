// RUN: split-file %s %t
// RUN: %host_cc %t/shared_lib.c --shared -fPIC -o %t/shared_lib.so
// RUN: arcilator %t/dpi.mlir --run --jit-entry=Top_main --shared-libs=%t/shared_lib.so | FileCheck --match-full-lines %s
// REQUIRES: arcilator-jit

// DPI functions called from processes can take and return dynamic strings,
// which are passed as `const char *`. Returned strings are copied, since DPI
// functions may reuse their memory.

//--- shared_lib.c
#include <ctype.h>
#include <string.h>

const char *shout(const char *str) {
  static char buffer[64];
  size_t i = 0;
  for (; str[i] && i < sizeof(buffer) - 2; ++i)
    buffer[i] = toupper(str[i]);
  buffer[i++] = '!';
  buffer[i] = 0;
  return buffer;
}

//--- dpi.mlir
// CHECK:      HELLO! WORLD!
sim.func.dpi @shout(in %str : !sim.dstring, return result : !sim.dstring)

hw.module @Top() {
  llhd.process {
    %hello = sim.string.literal "hello"
    %world = sim.string.literal "world"
    %0 = sim.func.dpi.call @shout(%hello) : (!sim.dstring) -> !sim.dstring
    %1 = sim.func.dpi.call @shout(%world) : (!sim.dstring) -> !sim.dstring
    %f0 = sim.fmt.string %0 : !sim.dstring
    %f1 = sim.fmt.string %1 : !sim.dstring
    %sp = sim.fmt.literal " "
    %nl = sim.fmt.literal "\0A"
    %msg = sim.fmt.concat (%f0, %sp, %f1, %nl)
    sim.proc.print %msg
    llhd.halt
  }
  hw.output
}
