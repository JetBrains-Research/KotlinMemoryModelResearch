# Semantics of Final Fields 

Thid doc discuss research questions related to `final` fields semantics 
in the JMM and whether we need/should provide the same semantics in Kotlin memory model.

## Introduction

In Java, final fields have special treatment in the memory model. 
The classic example demonstrating this looks as follows:

```java
class A {
    final int x;
    A() { x = 42; }

    static A a;
}

void writer_thread() {
    a = new A();
}

void reader_thread() {
    if (a != null) {
        int y = a.x; // guarantee to read 42
    }     
}

```

Note that despite the program above has a race on variable `a`
the JMM guarantees that the read of the final field will observe 
the initialization write of this field in the constructor.
Moreover, the read will synchronize with this write.
There are special JMM rule around final fields that induces happens-before
between write to `x` and read from `x` in this scenario.

The motivation for this special treatment of final fields was (apparently) to allow 
safe publication of immutable objects even in the presence of data races.

Additional reading:

* [1] [Final fields](https://shipilev.net/blog/2014/jmm-pragmatics/#_part_v_finals) in the Aleksey Shipilev's JMM blogpost.
* [2] [Specification](https://docs.oracle.com/javase/specs/jls/se8/html/jls-17.html#jls-17.5) of final fields semantics.
* [3] Another [blogpost](https://shipilev.net/blog/2014/all-fields-are-final/) by Aleksey Shipilev about 
  enforcing same semantics for all fields (not just final).
 
## Problem

In Kotlin, `val` fields are likely to be compiled as `final` fields for JVM target.
Thus, the equivalent of the Java program given above in Kotlin should look like. 

```kotlin
class A() {
    val x: Int

    init { x = 42 }
}

var a: A? = null

void writer_thread() {
    a = A();
}

void reader_thread() {
    if (a != null) {
        val y = a.x // what can be read here?
    }     
}

```

Thus, on JVM this Kotlin program is likely to produce the same outcome as in Java.
The question is --- does the same behavior is guaranteed in Kotlin/Native?

There are several obstacles with supporting `final` fields semantics in Kotlin/Native.

1. At the very least, it requires to emit `release` fence at the end of every constructor (see [1,3]).
   It is likely that K/N compiler currently omits these fences.

2. In JMM spec the definition of happens-before rules w.r.t. final fields is quite complicated (see [2]). 
   What is worse, it defines "dereference chain" relation 
   (roughly, this is similar to address dependency relation in some hardware models)
   which has no direct counterpart in C/C++ or LLVM memory models.
   The problem is similar to abondened `memory_order_consume` semantics in C/C++ and LLVM models. 
   Programming language memory models deliberately avoid defininig "dependency" relations 
   because in the presence of compiler optimizations, that can remove syntactic dependencies, 
   precise definition of "dependency" relation is an open research problem.
   All of this means that at least from the Spec point of view it will be impossible
   to provide same guarantees for `final` fields as in JMM.   

3. The `final` fields semantics in JMM itself is often critisized for being tricky (see [1,3]).
   In particular, one of the problems is that `final` fields guarantees are not provided by the JMM
   if the field can leak in the constructor (see [1] for an example why this case is problematic).
   So maybe we should just abondon `final` fields semantics in Kotlin memory model altogether 
   and rely on more robust synchronization mechanisms?    

## Questions

* What should be the semantics of `val` and `var` fields inititalization in Kotlin?
* Do we want to provide same semantics for `val` Kotlin fields as for Java's `final` fields?
* Do we want to provide same semantics for all Kotlin fields (`val` and `var`) as for Java's `final` fields?
* If we do not guarantee `final` fields semantics for Kotlin `val`, can it break safe publication patterns 
  in existing Kotlin code when it is compiled to Native target?
* What are performance impact for Kotlin/Native if we enforce `final` field semantics for `val` (and `var`)?

### Steps to Answer the Questions

1. Write a litmus test to check if `final` fields guarantee can be broken on Kotlin/Native for `val` fields
   (it is likely to be so, because my guess that K/N compiler currently do not put release fence at the end of the constructor). 

2. Evaluate performance impact of supporting `final` fields semantics in Kotlin/Native.

3. Review how safe publication idioms are currently implemented in typical Kotlin code 
   (e.g. singleton pattern --- `object` [keyword](https://kotlinlang.org/docs/object-declarations.html#object-declarations-overview) in Kotlin). 
   If they rely on `val` field with the intendend `final` semantics, try to reproduce a bug with Kotlin/Native.

## Possible Solutions

1. Do not support `final` fields semantics in Kotlin in any form.

   **Pros**: simplicity, requires no modification in K/N compiler.

   **Cons**: might deem some safe publication idioms already prevailed in Kotlin codebase incorrect.

2. Support `final` fields semantics in Kotlin for `val` fields only.

   **Pros**: close to current JMM formulation.

   **Cons**: likely requires changes to K/N compiler, formally invalid according to the current LLVM spec, inherits problems of JMM `final` fields semantics.

3. Support `final` fields semantics in Kotlin for all fields.

   **Pros**: a little bit more consistent model than current JMM formulation.

   **Cons**: requires changes both to K/JVM and K/N compiler, still invalid according to the current LLVM spec, inherits problems of JMM `final` fields semantics.
