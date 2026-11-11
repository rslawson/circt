// RUN: circt-opt --arc-generate-driver %s | FileCheck %s

// CHECK-LABEL: func.func @Foo_main()
// CHECK-NEXT:    arc.sim.instantiate @Foo as [[INST:%.+]] {
// CHECK-NEXT:      [[CTX:%.+]] = arc.as_context [[INST]]
// CHECK-NEXT:      [[NEVER:%.+]] = arith.constant -1 : i64
// CHECK-NEXT:      [[FIRST:%.+]] = arc.get_next_wakeup [[CTX]]
// CHECK-NEXT:      scf.while ([[W:%.+]] = [[FIRST]]) : (i64) -> i64 {
// CHECK-NEXT:        [[PENDING:%.+]] = arith.cmpi ne, [[W]], [[NEVER]]
// CHECK-NEXT:        [[TERM:%.+]] = arc.is_terminated [[CTX]]
// CHECK-NEXT:        [[FALSE:%.+]] = arith.constant false
// CHECK-NEXT:        [[RUNNING:%.+]] = arith.cmpi eq, [[TERM]], [[FALSE]]
// CHECK-NEXT:        [[PROCEED:%.+]] = arith.andi [[PENDING]], [[RUNNING]]
// CHECK-NEXT:        scf.condition([[PROCEED]]) [[W]]
// CHECK-NEXT:      } do {
// CHECK-NEXT:      ^bb0([[T:%.+]]: i64):
// CHECK-NEXT:        arc.sim.set_time [[INST]], [[T]]
// CHECK-NEXT:        arc.sim.step [[INST]]
// CHECK-NEXT:        [[NEXT:%.+]] = arc.get_next_wakeup [[CTX]]
// CHECK-NEXT:        scf.yield [[NEXT]]
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:    return
// CHECK-LABEL: arc.model @Foo
arc.model @Foo io !hw.modty<> {
^bb0(%arg0: !arc.storage):
}
