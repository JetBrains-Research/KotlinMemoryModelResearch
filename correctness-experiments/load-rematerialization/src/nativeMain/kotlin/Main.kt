import kotlin.native.concurrent.TransferMode
import kotlin.native.concurrent.Worker
import kotlin.system.exitProcess

const val DUMP_SIZE = 4
const val GARBAGE_SIZE = 4

var sharedVar: Int = 0 // shared memory

val garbageIn = IntArray(GARBAGE_SIZE) { 0 }
val garbageOut = IntArray(GARBAGE_SIZE) { -1 }

fun clearState() {
    sharedVar = 0
    repeat(GARBAGE_SIZE) { index ->
        garbageIn[index] = 0
        garbageIn[index] = -1
    }
}

fun spinWait(): Int {
    val wait = (0..100000).random()
    var garbage = 0 // prevent a compiler from eliminating repeat
    repeat(wait) { garbage++ }
    return garbage
}

fun runTest() {
    val reader = Worker.start()
    val writer = Worker.start()
    val readerFuture = reader.execute(TransferMode.SAFE, {}) {
        spinWait()
        val dump = IntArray(DUMP_SIZE) { 0 }
        val t = sharedVar
        // some operations with t can be added to force a compiler to make the first read here
        // unfortunately, that doesn't help

        fun randIndex() = (0 until GARBAGE_SIZE).random()
        repeat(DUMP_SIZE) { index ->
            val i0 = randIndex()
            val i1 = randIndex()
            val i2 = randIndex()
            val i3 = randIndex()
            val i4 = randIndex()
            val i5 = randIndex()
            val i6 = randIndex()
            val i7 = randIndex()
            val i8 = randIndex()
            val i9 = randIndex()
            garbageOut[i0] = garbageIn[i0]
            garbageOut[i1] = garbageIn[i1]
            garbageOut[i2] = garbageIn[i2]
            garbageOut[i3] = garbageIn[i3]
            garbageOut[i4] = garbageIn[i4]
            garbageOut[i5] = garbageIn[i5]
            garbageOut[i6] = garbageIn[i6]
            garbageOut[i7] = garbageIn[i7]
            garbageOut[i8] = garbageIn[i8]
            garbageOut[i9] = garbageIn[i9]
            dump[index] = t
        }

        fun IntArray.println() = println(joinToString(separator = ", ") { value -> "$value" })
        println("dump")
        dump.println()
        println("garbage")
        garbageIn.println()
        garbageOut.println()

        if (dump.any { it != dump[0] }) {
            println("Gotcha!")
            exitProcess(0)
        }
        println("reader has finished")
    }

    val writerFuture = writer.execute(TransferMode.SAFE, {}) {
        spinWait()
        sharedVar = 42
        println("writer has finished")
    }

    readerFuture.result
    writerFuture.result
}

fun main() {
    repeat(1000) { iteration ->
        println("\n------------------ ITER: $iteration")
        clearState()
        runTest()
    }
}
