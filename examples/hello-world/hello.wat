(module
(import "env" "putchar" (func $putchar (param i32) (result i32)))
(memory 256)
(export "memory" (memory 0))
(global $sp (mut i32) (i32.const 16777216))
(func $main (result i32)
  (local i32)
  (local $state i32)
block $exit
loop $top
block $S4
block $S3
block $S2
block $S1
block $S0
local.get $state
br_table $S0 $S1 $S2 $S3 $S4 $exit
end
i32.const 16
local.set 0
i32.const 3
local.set $state
br $top
end
local.get 0
i32.load8_s
call $putchar
drop
end
local.get 0
i32.const 1
i32.add
local.set 0
end
local.get 0
i32.load8_s
i32.const 0
i32.ne
if
i32.const 1
local.set $state
br $top
end
i32.const 0
return
end
end
end
unreachable
)
(export "main" (func $main))
(data (i32.const 16) "\48\65\6c\6c\6f\2c\20\77\6f\72\6c\64\20\66\72\6f\6d\20\6c\63\63\2d\77\61\73\6d\21\0a\00")
)
