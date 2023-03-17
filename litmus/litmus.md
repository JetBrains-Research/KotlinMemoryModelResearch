# Litmus Tests for Kotlin Shared Memory Model (KoMeM)

This is a work-in-progress document describing a set of litmus tests 
for testing Kotlin compiler conformance to the shared memory model specification.
It contains a standard set of litmus tests from literature,  
and is mainly inspired by the analogous test suite from the [jcstress](https://github.com/openjdk/jcstress) 
project aiming to test JVM implementation conformance to JMM.

The litmus tests are grouped according to the higher-level properties 
of the memory model that they are aiming to check. 

The set of programming primitives covered by the tests is listed below:
- non-atomic (plain) accesses;
- atomic sequentially consistent (volatile) accesses;
- locks.

The litmus tests in this doc are given in pseudo-code for clarity.
How they are coded in Kotlin is an orthogonal question 
(also note that the atomics and locks API is still unstable in Kotlin Multiplatform).


### Access Atomicity

### Sequential Consistency

Sequential consistency (SC) is well-know property requiring that the result 
of any concurrent execution should be equivalent to executing individual 
atomic actions in some total sequential order.
 
Non-atomic accesses do not guarantee sequentially consistent semantics.
This can be checked with the standard _Store Buffering (SB)_ litmus test.

```
(SB)

plain var x, y;
local var a, b; 
===============

x = 1  || y = 1 
a = y  || b = x

===============
Expected outcomes:
a=0, b=1
a=1, b=0
a=1, b=1
a=0, b=0 (non-SC outcome!)
```

Using sequentially consistent atomics should restore sequentially consistent semantics.

```
(SB+SC)

volatile var x, y;
local var a, b; 
===============

x = 1  || y = 1 
a = y  || b = x

===============
Expected outcomes:
a=0, b=1
a=1, b=0
a=1, b=1
Forbidden outcomes:
a=0, b=0 (non-SC outcome!)
```

__TODO:__
check out [this paper](https://drops.dagstuhl.de/opus/volltexte/2022/16234/pdf/LIPIcs-ECOOP-2022-6.pdf)
for more litmus test examples, showcasing difference in the semantics of LLVM SeqCst and Java Volatile accesses.

#### Notes 

- In JMM/LLVM only `volatile` (`seq_cst`) accesses guarantee sequentially-consistent semantics.
  Any weaker access mode (e.g. `acquire`/`release`, `opaque`) would fail same as non-atomic accesses.

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_07_Consensus.java)


### Mutual Exclusion

Locks should provide mutual exclusion property.

```
(MUTEX)

lock l;
plain var x;
local var a, b; 
===============

withLock(l) { || withLock(l) { 
  a = ++x     ||   b = ++x
}             || }

===============
Expected outcomes:
a=1, b=2
a=2, b=1
Forbidden outcomes:
a=1, b=1 (violates mutual exclusion)
```


The variant of Store Buffering test with locks should allow even less number of outcomes than (SB+SC)
(because of mutual exclusion again).

```
(SB+Lock)

lock l;
plain var x, y;
local var a, b; 
===============

withLock(l) { || withLock(l) { 
  x = 1       ||   y = 1
  a = y       ||   b = x
}             || }

===============
Expected outcomes:
a=0, b=1
a=1, b=0
Forbidden outcomes:
a=1, b=1 (violates mutual exclusion)
a=0, b=0 (weak outcome!)
```

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/primitives/mutex/Mutex_04_Synchronized.java)

### Synchronizes-With and Happens-Before Rules

Synchronizes-with relation specifies the rules of inter-thread synchronization.  

Reads-from relation on non-atomic accesses do not establish synchronizes-with (and thus happens-before) relations. 
Subsequent reads can observe "stale" values.
This can be checked with the standard _Message Passing (SB)_ litmus test.

```
plain var x, y;
local var a, b; 
===============

x = 1  || a = y 
y = 1  || b = x

===============
Expected outcomes:
a=0, b=0
a=0, b=1
a=1, b=1
a=1, b=0 (weak outcome!)
```


Reads-from relation on volatile accesses should establish synchronizes-with relation.
Subsequent reads (even non-atomic one) are forbidden to observe "stale" values.


```
plain var x;
volatile var y;
local var a, b; 
===============

x = 1  || a = y 
y = 1  || b = x

===============
Expected outcomes:
a=0, b=0
a=0, b=1
a=1, b=1
Forbidden outcomes:
a=1, b=0 (weak outcome!)
```

Matching lock-release and lock-acquire pairs also should establish synchronizes-with relation.

```
lock l;
plain var x, y;
local var a, b; 
===============

x = 1         || withLock(l) { 
withLock(l) { ||   a = y    
  y = 1       || }
}             || b = x

===============
Expected outcomes:
a=0, b=0
a=0, b=1
a=1, b=1
Forbidden outcomes:
a=1, b=0 (weak outcome!)
```

#### Notes 

- In JMM/LLVM using `acquire`/`release` accesses is enough to establish synchronizes-with relation
  Weaker access mode (i.e. `opaque`) would fail same as non-atomic accesses.

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_06_Causality.java)


### Read-Modify-Write Atomicity

Read-modify-write atomic operations (e.g. Compare-and-Swap) should be atomic.
This can be checked with a number of standard _RMW_ litmus tests.

```
(CAS)

volatile var x;
local var a, b; 
===============

  a = x.CAS(0, 1) || b = x.CAS(0, 1)

===============
Expected outcomes:
a=0, b=1
a=1, b=0
Forbidden outcomes:
a=0, b=0 (violates atomicity)
```


```
(FADD)

volatile var x;
local var a, b; 
===============

  a = x.FADD() || b = x.FADD()

===============
Expected outcomes:
a=0, b=1
a=1, b=0
Forbidden outcomes:
a=0, b=0 (violates atomicity)
```


```
(FADD)

volatile var x;
local var a, b; 
===============

  a = x.FADD() || x = 2
               || b = x

===============
Expected outcomes:
a=0, b=2
a=2, b=3
Forbidden outcomes:
a=0, b=1 (violates atomicity)
```

### Coherence

__Coherence__, also known as _sequential consistency per location_, 
is a property stating that all write accesses to the same variable should be totally ordered, 
and that this order should be consistent with happens-before order.
In particular, it imlies that if a program accesses only one location, 
then it should have sequentially consistent semantics.
 
Non atomic accesses should not guarantee coherence (same as in JVM).
This can be checked with the standard _Read-Read Coherence_ litmus test.


```
(CoRR)

plain var x;
local var a, b; 
===============

x = 1  || a = x 
       || b = x

===============
Expected outcomes:
a=0, b=0
a=0, b=1
a=1, b=1
Allowed (but unlikely to observe) outcomes:
a=1, b=0 (violates coherence!)
```

The motivation for allowing such behaviors stems from the requirement
to enable _Common Subexpression Elimination (CSE)_ and similar optimizations
for non-atomic accesses without relying on _alias-analysis_.

```
(CoRR)
class Holder {
  plain var x;
}
plain var holder1 = Holder();
plain var holder2 = holder1;
local var h1, h2;
local var a, b; 
===============

foo1.x = 1 || h1 = holder1 
           || h2 = holder2
           || if (h1 != null & h2 != null) {
           ||   a = h1.x;
           ||   b = h2.x;
           ||   c = h1.x;
           || }

===============
Expected outcomes:
a=0, b=0, c=0
a=0, b=1, c=1
a=1, b=1, c=1
a=1, b=0, c=1 
Allowed (but unlikely to observe) outcomes:
a=1, b=0, c=0 
```

In the example above, the compiler is likely to transform load `c = h1.x` into `c = a`,
but for `b = h2.x` it is likely to emit separate load, and also it might reorder it with `a = h1.x` load.
In order to forbid this latter reordering, the compiler would need to infer 
that `holder1 == holder2` --- this requires inter-procedural alias analysis.


#### Notes 

- In JMM/LLVM `opaque` (`monotonic` in LLVM terminology) accesses (and stronger) provide coherence guarantee.

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_05_Coherence.java)


### Multi-Copy Atomicity

Multi-copy atomicity is a property stating that all threads 
should observe write accesses to memory in the same order.
Note that this is still weaker than sequential consistency, 
because the read accesses are not obligated to observe globally "latest" write access.

Non atomic accesses should not guarantee multi-copy atomicity.
This can be checked with the standard _Independent Reads of Independent Writes_ litmus test.

```
(IRIW)

plain var x, y;
local var a, b; 
===============

x = 1 || a = x  || c = y || y = 1
      || b = y  || d = x ||

===============
Expected outcomes:
a=1, b=0, c=1, d=0 (weak outcome!)
... (all other possible SC outcomes)
```

Using sequentially consistent atomics should prevent IRIW behavior.

```
(IRIW)

volatile var x, y;
local var a, b; 
===============

x = 1 || a = x  || c = y || y = 1
      || b = y  || d = x ||

===============
Expected outcomes:
... (all possible SC outcomes)
Forbiddent outcomes:
a=1, b=0, c=1, d=0 (weak outcome!)
```  

#### Links:

- Relevant JCStress tests: 
    [1](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/advanced/AdvancedJMM_02_MultiCopyAtomic.java),
    [2](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/advanced/AdvancedJMM_03_NonMCA_Coherence.java)


### Unsafe Publication

__TODO__

### Causality and Out-of-Thin-Air

__TODO__