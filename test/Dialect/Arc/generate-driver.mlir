// RUN: circt-opt --arc-generate-driver %s | FileCheck %s
// RUN: circt-opt --arc-generate-driver=delta-limit=0 %s | FileCheck %s --check-prefix=NOLIMIT

// CHECK-LABEL: func.func @Foo_main()
// CHECK-NEXT:    arc.sim.instantiate @Foo as [[INST:%.+]] {
// CHECK-NEXT:      [[CTX:%.+]] = arc.as_context [[INST]]
// CHECK-NEXT:      [[NEVER:%.+]] = arith.constant -1 : i64
// CHECK-NEXT:      [[FIRST:%.+]] = arc.get_next_wakeup [[CTX]]
// CHECK-NEXT:      [[ZERO:%.+]] = arith.constant 0 : i64
// CHECK-NEXT:      scf.while ([[W:%.+]] = [[FIRST]], [[N:%.+]] = [[ZERO]]) : (i64, i64) -> (i64, i64) {
// CHECK-NEXT:        [[PENDING:%.+]] = arith.cmpi ne, [[W]], [[NEVER]]
// CHECK-NEXT:        [[TERM:%.+]] = arc.is_terminated [[CTX]]
// CHECK-NEXT:        [[FALSE:%.+]] = arith.constant false
// CHECK-NEXT:        [[RUNNING:%.+]] = arith.cmpi eq, [[TERM]], [[FALSE]]
// CHECK-NEXT:        [[PROCEED:%.+]] = arith.andi [[PENDING]], [[RUNNING]]
// CHECK-NEXT:        scf.condition([[PROCEED]]) [[W]], [[N]]
// CHECK-NEXT:      } do {
// CHECK-NEXT:      ^bb0([[T:%.+]]: i64, [[PREVN:%.+]]: i64):
// CHECK-NEXT:        [[NOW:%.+]] = arc.current_time [[CTX]]
// CHECK-NEXT:        [[SAME:%.+]] = arith.cmpi eq, [[T]], [[NOW]]
// CHECK-NEXT:        [[ONE:%.+]] = arith.constant 1 : i64
// CHECK-NEXT:        [[INC:%.+]] = arith.addi [[PREVN]], [[ONE]]
// CHECK-NEXT:        [[NEWN:%.+]] = arith.select [[SAME]], [[INC]], [[ZERO]]
// CHECK-NEXT:        [[LIMIT:%.+]] = arith.constant 100000 : i64
// CHECK-NEXT:        [[EXCEEDED:%.+]] = arith.cmpi uge, [[NEWN]], [[LIMIT]]
// CHECK-NEXT:        scf.if [[EXCEEDED]] {
// CHECK:               sim.proc.print
// CHECK-NEXT:          arc.terminate [[CTX]], false
// CHECK-NEXT:        } else {
// CHECK-NEXT:          arc.sim.set_time [[INST]], [[T]]
// CHECK-NEXT:          arc.sim.step [[INST]]
// CHECK-NEXT:        }
// CHECK-NEXT:        [[NEXT:%.+]] = arc.get_next_wakeup [[CTX]]
// CHECK-NEXT:        scf.yield [[NEXT]], [[NEWN]]
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:    return

// NOLIMIT-LABEL: func.func @Foo_main()
// NOLIMIT-NOT:     scf.if
// NOLIMIT:         arc.sim.step
// NOLIMIT-LABEL: arc.model @Foo
arc.model @Foo io !hw.modty<> {
^bb0(%arg0: !arc.storage):
}
