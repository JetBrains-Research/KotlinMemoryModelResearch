# Litmus Tests for Kotlin Shared Memory Model (KoMeM)

This is a work-in-progress document describing a set of litmus tests for testing Kotlin compiler conformance to the shared memory model specification.
The litmus tests are grouped according to the higher-level properties
of the memory model that they are aiming to check.

The test suite contains a standard set of litmus tests from literature, 
and is mainly inspired by the analogous test suite from the [jcstress](https://github.com/openjdk/jcstress) 
project aiming to test JVM implementations conformance to Java Memory Model (JMM).

The set of programming primitives covered by the tests is listed below:
- non-atomic (plain) accesses;
- atomic sequentially consistent (volatile) accesses;
- read-modify-write operations (e.g. CAS) on atomics;
- locks.

The litmus tests in this doc are given in pseudocode because 
currently the atomics and locks API is still unstable in Kotlin Multiplatform

<!-- TOC -->
* [Summary](#summary)
  * [Proposed Changes with respect to JMM](#proposed-changes-with-respect-to-jmm)
* [Glossary](#glossary)
* [Guarantees and Litmus Tests](#guarantees-and-litmus-tests)
  * [Access Atomicity](#access-atomicity)
  * [Sequential Consistency for Volatile Accesses](#sequential-consistency-for-volatile-accesses)
  * [Mutual Exclusion for Locks](#mutual-exclusion-for-locks)
  * [Synchronizes-With and Happens-Before Rules](#synchronizes-with-and-happens-before-rules)
  * [Sequential Consistency for Data-Race Free Programs (DRF-SC)](#sequential-consistency-for-data-race-free-programs--drf-sc-)
  * [Read-Modify-Write Atomicity](#read-modify-write-atomicity)
  * [Coherence](#coherence)
  * [Multi-Copy Atomicity for Volatile Accesses](#multi-copy-atomicity-for-volatile-accesses)
  * [Initialization and Unsafe Publication Guarantees](#initialization-and-unsafe-publication-guarantees)
  * [Progress Guarantees for Volatile Accesses](#progress-guarantees-for-volatile-accesses)
  * [Causality and Out-of-Thin-Air](#causality-and-out-of-thin-air)
<!-- TOC -->

## Summary

The main design goals of the Kotlin Memory Model are listed below. 

* Being close to JMM for the purpose of Java ecosystem compatibility.
* Being pragmatic, as simple as possible, and close to industry state-of-the-art.
* Requiring minimal changes to Kotlin compiler (ideally, none).

Non-goals:

* Inventing a new complicated academic memory model.

With these goals in mind, we propose the following main properties of memory model.

* __Access atomicity__ for primitive types (except `Long` and `Double`), and references.
  Since JMM provides the same guarantee, it would be reasonable to also require it from Kotlin.

  __Alternatives:__
  + access atomicity for __all primitive types__ and references
    * Pros: simpler model, no difference between `Int`/`Long` and `Float`/`Double` types.
    * Cons: would require significant changes in the Kotlin/JVM and Kotlin/Native compilers ---
      special treatment of `Long` and `Double` variables when emitting platform code in backend.
  + _access atomicity_ __only for references__
    * Pros: simpler model, no difference between `Int`/`Long` and `Float`/`Double` types, 
      no need to worry about exotic architectures with non-machine-word-sized `int` in Kotlin/Native compiler. 
    * Cons: weaker semantics compared to JMM  

* __Data Race Freedom Guarantee__ --- "correctly synchronizing programs have sequentially consistent semantics".
  It is a standard requirement for memory models asserting that
  race-free programs should exhibit only sequentially consistent behaviors.

* __Happens-Before__  guarantees with a standard set of rules,
  requiring that synchronizing operations (matching unlock/lock events, writes/reads on volatile variables, etc)
  establish happens-before relation.

* __Progress Guarantees__  for atomic variables, requiring that all writes
  to atomic variables will be eventually observed by all threads.
  Read and write operations on __non-atomic__ variables __do not give any 
  progress guarantess__ (similarly, as in JMM).

* __Safe Initialization__ for all variables even in presence of races 
  (unsafe publication). This property asserts that uninitialized "garbage"
  values cannot be read under any circumstances. It is guaranteed by JMM, 
  and should be guaranteed for any safe language. 

* __Unspecified Behavior__ for __racy programs__.
  Rigorous specification of non-atomic accesses behavior in presence of races 
  is a decade-lasting open research problem. 
  Existing research proposals are too complicated and fragile to rely on in practice.
  Declaring the semantics of racy programs to be implementation-defined 
  gives us freedom to refine the specification in the future, 
  when a satisfactory solution will be discovered. At the same time it is still an improvement over current 
  [specification](https://kotlinlang.org/spec/concurrency.html#concurrency)
  which declares behavior of all concurrent programs to be platform-defined. 

### Proposed Changes with respect to JMM

Despite our efforts to stick close to JMM, 
we propose the following changes aiming to address some problematic aspects of JMM.

* No special treatment of __final fields__, which, in context of Kotlin, would correspond to final `val` properties. 
  Final fields semantics in JMM is often criticised for being
  overly-complicated, loosely specified, and fragile.
  The special treatment of final fields in JMM was introduced
  for enabling safe publication patterns for immutable objects,
  while avoiding performance overhead of `volatile` accesses
  (at the time of first JMM spec Java had only this kind of atomic accesses).
  From that time, many languages (including Java itself)
  have adopted `acquire` and `release` access modes,
  which provide more rigorous approach for implementing lightweight publication patterns.
  For these reasons, instead of duplicating problematic final fields semantics in Kotlin,
  __we would recommend__ to instead provide __Acquire and Release__ atomic accesses in Kotlin.

## Glossary

Access modes:

| JMM name             | LLVM name            | C++ name             | Provided Guarantees                         |
|----------------------|----------------------|----------------------|---------------------------------------------|
| ---                  | `NotAtomic`          | non-atomic           | Undefined behavior in case of races         |
| `plain`              | `Unordered`          | ---                  | No Out-of-thin-Air values (vaguely defined) |
| `opaque`             | `Monotonic`          | `relaxed`            | Coherence                                   |
| `release` & `acqure` | `Release` & `Acqure` | `release` & `acqure` | Release-Acquire Consistency                 |
| `volatile`           | `SeqCst`             | `seq_cst`            | Sequential Consistency                      |

Common relations:

- _Program-Order_ --- total order among all memory access events within each thread.
- _Reads-From_ --- relation binding write access to all read accesses, that read from this write.
- _Synchronizes-With_ --- relation connecting events that provide inter-thread synchronization.
- _Happens-Before_ --- roughly, an order in which events "observe" each other; formally, 
  a transitive closure of the union of program-order and synchronizes-with relations.

#### Links

- [JDK9 memory order modes](https://gee.cs.oswego.edu/dl/html/j9mm.html)
- [LLVM access modes](https://llvm.org/docs/Atomics.html)
- [C++ memory_order](https://en.cppreference.com/w/cpp/atomic/memory_order)

## Guarantees and Litmus Tests

Next we provide a more detailed description of various 
memory model guarantees, together with litmus tests 
aiming to check these properties.

### Access Atomicity

Accesses to variables/fields of the reference types 
and primitive types except `Long` and `Double` should be atomic.

```
(ATOM)

plain var x: Int;
local var a: Int; 
===============

x = -1 || a = x 

===============
Expected outcomes:
a=0
a=-1
```

#### Notes

- [JVM does not guarantee atomicity](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.7) 
  for `Long` and `Double`, hence we cannot guarantee it by default either.
  Still, to get a simpler model, it might be worth to actually provide atomicity for these types.
  This, however, would require some effort. One solution might be 
  to compile accesses to `Long` and `Double` variables/fields as `opaque` accesses on JVM, 
  and as `unordered` accesses on LLVM. 
  

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_02_AccessAtomicity.java)


### Sequential Consistency for Volatile Accesses

Sequential consistency (SC) is well-know property requiring that the result 
of any concurrent execution should be equivalent to executing individual 
atomic actions in some total sequential order.
 
Non-atomic accesses do not guarantee sequentially consistent semantics.
This can be checked with the standard _Store Buffering (SB)_ litmus test.

```
(SB)

plain var x, y: Int;
local var a, b: Int; 
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
(SB+Vol)

volatile var x, y: Int;
local var a, b: Int; 
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
check out the [Liu-al:ECOOP22](https://drops.dagstuhl.de/opus/volltexte/2022/16234/pdf/LIPIcs-ECOOP-2022-6.pdf)
paper on VarHandler semantics
for more litmus test examples, showcasing difference in the semantics of LLVM SeqCst and Java Volatile accesses.

#### Notes 

- In JMM/LLVM only `volatile` (`seq_cst`) accesses guarantee sequentially-consistent semantics.
  Any weaker access mode (e.g. `acquire`/`release`, `opaque`) would fail same as non-atomic accesses.

#### Links

- Relevant JCStress [tests](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_07_Consensus.java)


### Mutual Exclusion for Locks

Locks should provide mutual exclusion property.

```
(MUTEX)

plain var l: Lock;
plain var x: int;
local var a, b: Int; 
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

plain var l: Lock; 
plain var x, y: Int;
local var a, b: Int;
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
This can be checked with the standard _Message Passing (MP)_ litmus test.

```
(MP)

plain var x, y: Int;
local var a, b: Int; 
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
(MP+Vol)

plain var x: Int;
volatile var y: Int;
local var a, b: Int; 
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
(MP+Lock)

plain var l: Lock; 
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


### Sequential Consistency for Data-Race Free Programs (DRF-SC)

DRF is a standard requirement to memory models asserting that
race-free programs should exhibit only sequentially consistent behaviors.
In simple words "correctly synchronizing programs have sequentially consistent semantics".

Definition of data-race is standard: two accesses form data race 
if they are not ordered by happens-before relation, 
at least one of them is a write access, and at least one is non-atomic access.
Thus, we say memory model M satisfies DRF-SC, if for any program 
that is race-free under sequential consistency, model M allows only 
sequentially consistent behaviors.

Let us consider another variant of message passing litmus test.

```
(MP-DRF)

plain var x: Int;
volatile var y: Int;
local var a: Int; 
===============

x = 1  || if (y) {
y = 1  ||   a = x 
       || }

===============
Expected outcomes:
a=1
Forbiddent outcomes:
a!=0
```

Here the only allowed outcome is `a=1`.
This can be proven with the help of DRF-SC. 
Under sequential consistency, read from `y` can either 
occur before write `y = 1` (in this case then branch is not executed at all), 
or after it. In the latter case the read `a = x` should observe the "last" write to `x`, 
which is `x = 1`. Because both of these executions are race-free
we can apply DRF-SC and conclude that these executions are the only possible 
executions under Kotlin memory model.

Note that in the example above write `y = 1` and read from `y` do not constitute "data race"
according to DRF-SC definition, because both are `volatile` accesses.


### Read-Modify-Write Atomicity

Read-modify-write atomic operations (e.g. Compare-and-Swap) should be atomic.
This can be checked with a number of standard _RMW_ litmus tests.

```
(CAS)

volatile var x: Int;
local var a, b: Int; 
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

volatile var x: Int;
local var a, b: Int; 
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
(FADD+WR)

volatile var x: Int;
local var a, b: Int; 
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

In terms of ordering guarantees, read-modify-write operations 
should provide same guarantees as a pair of volatile read and write accesses.

```
(MP+CAS)

plain var x: Int;
volatile var y: Int;
local var a, b: Int; 
===============

x = 1  || a = y.CAS(1, 2) 
y = 1  || b = x

===============
Expected outcomes:
a=0, b=0
a=0, b=1
a=1, b=1
Forbidden outcomes:
a=1, b=0 (weak outcome!)
```


### Coherence

__Coherence__, also known as _sequential consistency per location_, 
is a property stating that all write accesses to the same variable should be totally ordered, 
and that this order should be consistent with happens-before order.
In particular, it implies that if a program accesses only one location, 
then it should have sequentially consistent semantics.
 
Non-atomic accesses should not guarantee coherence (same as in JVM).
This can be checked with the standard _Read-Read Coherence_ litmus test.


```
(CoRR)

plain var x: Int;
local var a, b: Int; 
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
(CoRR-CSE)
class Holder {
  plain var x: Int;
}
plain var holder1 = Holder();
plain var holder2 = holder1;
local var h1, h2: Holder;
local var a, b: Int; 
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


### Multi-Copy Atomicity for Volatile Accesses

Multi-copy atomicity is a property stating that all threads 
should observe write accesses to memory in the same order.
Note that this is still weaker than sequential consistency, 
because the read accesses are not obligated to observe globally "latest" write access.

Non-atomic accesses should not guarantee multi-copy atomicity.
This can be checked with the standard _Independent Reads of Independent Writes_ litmus test.

```
(IRIW)

plain var x, y: Int;
local var a, b: Int; 
===============

x = 1 || a = x  || c = y || y = 1
      || b = y  || d = x ||

===============
Expected outcomes:
a=1, b=0, c=1, d=0 (weak outcome!)
... (all other possible SC outcomes)
```

Sequentially consistent atomics provide multi-copy atomicity.

```
(IRIW+Vol)

volatile var x, y: Int;
local var a, b: Int; 
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


### Initialization and Unsafe Publication Guarantees

As a safe language, Kotlin should guarantee that variables and fields 
should always be initialized, even in the presence of data-races and unsafe publication.
Uninitialized "garbage" values cannot be read under any circumstances.

```
(UPUB)

class Holder {
  plain var x: Int;
}
plain var h: Holder;
local var t: Holder;
local var a: Int;
===============

h = new Holder() || t = h 
                 || if (t != null) {
                 ||   a = t.x
                 || }

===============
Expected outcomes:
a=0
Forbidden outcomes:
a!=0 ("garbage" value) 
```

Note that unlike JMM, we do not aim to provide any guarantees 
for custom initialization writes in user-provided constructor.

```
(UPUB+Ctor)

class Holder {
  plain val x: Int = 1;
}
plain var h: Holder;
local var t: Holder;
local var a: Int;
===============

h = new Holder() || t = h 
                 || if (t != null) {
                 ||   a = t.x
                 || }

===============
Expected outcomes:
a=0
a=1
Forbidden outcomes:
a!=0 || a!=1 ("garbage" value) 
```

For the example above, JMM guarantees for read `a = t.x` to observe value 1.
The final fields semantics in JMM is often criticised for being 
overly-complicated, loosely specified, and fragile.
For example, if the object reference is leaked from the constructor,
the guarantee described above is no longer applicable.

#### Links

- Relevant JCStress [test](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_08_Finals.java)
- [An example](https://shipilev.net/blog/2014/jmm-pragmatics/#_premature_publication) 
  of final field semantics violation due to object reference leak in the constructor.

### Progress Guarantees for Volatile Accesses

There is no progress guarantees for non-atomic accesses.
This allows the compiler to hoist read from non-atomic variable out of the loop body.

```
(WHILE)

plain var x: Int;
===============

x = 1  || while (x == 0) {} 

===============
Expected outcomes:
-- program terminates
-- program hungs
```

For volatile accesses, eventual progress is guaranteed.

```
(WHILE+Vol)

volatile var x: Int;
===============

x = 1  || while (x == 0) {} 

===============
Expected outcomes:
-- program terminates
Forbidden outcomes:
-- program hungs
```

#### Links

- Relevant JCStress [test](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_04_Progress.java)


### Causality and Out-of-Thin-Air

Causality rules aim to prevent paradoxical behaviors 
where inter-thread communications result in read value 
to appear _out-of-thin-air (OOTA)_ due to causality loops. 
The following test, which is called _Load Buffering with Dependencies (LB+DEPS)_, 
presents an example of a possible thin-air outcome.

```
(LB+DEPS)

plain var x, y: Int;
local var a, b: Int; 
===============

a = x  || b = y 
y = a  || x = b

===============
Expected outcomes:
a=0, b=0
Forbidden outcomes:
a=1, b=1 (out-of-thin-air outcome)
```

In the example above, the outcome with both reads 
observing value 1 is forbidden because of the dependencies
between instructions in both threads. 
Note that if we remove these dependencies, the outcome is allowed
(and it can even be observed on some ARM machines):

```
(LB)

plain var x, y: Int;
local var a, b: Int; 
===============

a = x  || b = y 
y = 1  || x = 1

===============
Expected outcomes:
a=0, b=0
a=1, b=0
a=0, b=1
a=1, b=1 (weak outcome)
```

Here the processor or the compiler are allowed to move stores before the loads.

In these two examples it is easy to spot what outcome is OOTA and what is valid one.
However, in general, it might be not so obvious, because it might be hard 
to determine what dependencies are "real" and what are "fake".
Consider another example: 

```
(LB+FakeDEPS)

plain var x, y: Int;
local var a, b: Int; 
===============

a = x          || b = y 
y = 1 + a * 0  || x = b

===============
Expected outcomes:
a=0, b=0
Forbidden outcomes:
a=1, b=1 (out-of-thin-air outcome)
```

Here we still have dependencies between instructions in both threads, 
but the dependency in the left thread is _fake_, because it can be 
removed by an optimizing compiler (due to constant folding).

In general, a rigorous specification of "true dependencies" is an open research problem.
For this reason, we currently do not aim to provide the specification for
concurrent programs which have racy non-atomic (plain) accesses.
Instead, we are going to say that the semantics of such programs is __platform-dependent__.
This gives us an opportunity to refine the specification in the future, 
when the research community will provide a satisfactory solution to OOTA problem.

Note that we still provide a specification for (non-atomic) race-free programs.

In particular, if we make all variables `volatile` in the (LB) example above,
the weak behavior should be forbidden.

```
(LB+Vol)

volatile var x, y: Int;
local var a, b: Int; 
===============

a = x  || b = y 
y = 1  || x = 1

===============
Expected outcomes:
a=0, b=0
a=1, b=0
a=0, b=1
Forbidden outcomes:
a=1, b=1 (weak outcome)
```

In general, for atomic accesses, we require that union of 
program order and reads-from relations is acyclic --- 
thus preventing all kinds of causality loops.

#### Notes

- In JMM/LLVM all access modes, stronger than `opaque`/`monotonic` guarantee acyclicity
  of program-order and reads-from relations. 
  However, for `opaque`/`monotonic` access mode there is no such guarantee, 
  thus it subject to the same paradoxes as `plain` access mode in JMM.

#### Links

- Relevant JCStress [test](https://github.com/openjdk/jcstress/blob/master/jcstress-samples/src/main/java/org/openjdk/jcstress/samples/jmm/basic/BasicJMM_10_OOTA.java)

