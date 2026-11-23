package ai.pocket.heyclaude.wake

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import kotlin.math.ceil

/**
 * JVM tests for the openWakeWord windowing logic with a fake model runner (no ONNX native
 * code). The fake melspectrogram emits `ceil(n/160 - 3)` frames like the real model and
 * encodes a running global frame id into each frame so windows can be checked exactly:
 * the raw value is chosen so that the *transformed* value (`x/10 + 2`) equals the id.
 */
class FeatureBufferTest {

    private class FakeModels : FeatureModels {
        val melInputLengths = mutableListOf<Int>()
        val melInputs = mutableListOf<FloatArray>()
        val embeddingWindows = mutableListOf<FloatArray>()
        val wakeInputs = mutableListOf<FloatArray>()
        var nextFrameId = FIRST_FRAME_ID
        var embeddingCounter = 0

        override fun melspectrogram(samples: FloatArray, length: Int, out: FloatArray): Int {
            melInputLengths += length
            melInputs += samples.copyOf(length)
            val frames = ceil(length / 160.0 - 3).toInt()
            for (f in 0 until frames) {
                val raw = (nextFrameId - 2f) * 10f // transformed: raw/10 + 2 == id
                for (b in 0 until FeatureBuffer.MEL_BINS) out[f * FeatureBuffer.MEL_BINS + b] = raw
                nextFrameId++
            }
            return frames
        }

        override fun embedding(melWindow: FloatArray, out: FloatArray) {
            embeddingWindows += melWindow.copyOf()
            embeddingCounter++
            out.fill(embeddingCounter.toFloat())
        }

        override fun wakeScore(features: FloatArray): Float {
            wakeInputs += features.copyOf()
            return SCORE
        }

        /** Transformed value of mel row [row] in a captured window. */
        fun row(window: FloatArray, row: Int) = window[row * FeatureBuffer.MEL_BINS]
    }

    private lateinit var fake: FakeModels
    private lateinit var buf: FeatureBuffer

    @Before
    fun setUp() {
        fake = FakeModels()
        buf = FeatureBuffer(fake, N)
    }

    private fun chunk(start: Int = 0) = ShortArray(FeatureBuffer.CHUNK_SAMPLES) { (start + it).toShort() }

    @Test
    fun constantsMatchReference() {
        assertEquals(1280, FeatureBuffer.CHUNK_SAMPLES)
        assertEquals(480, FeatureBuffer.OVERLAP_SAMPLES)
        assertEquals(1760, FeatureBuffer.RAW_WINDOW_SAMPLES)
        assertEquals(32, FeatureBuffer.MEL_BINS)
        assertEquals(76, FeatureBuffer.MEL_WINDOW_FRAMES)
        assertEquals(8, FeatureBuffer.MEL_STEP_FRAMES)
        assertEquals(96, FeatureBuffer.EMBEDDING_DIM)
    }

    @Test
    fun melInputIsChunkFirstThenChunkPlus480Overlap() {
        buf.process(chunk(0))
        buf.process(chunk(10_000))
        buf.process(chunk(20_000))
        assertEquals(listOf(1280, 1760, 1760), fake.melInputLengths)
        // Second input = last 480 samples of chunk 1 + chunk 2, as float (not normalised).
        val second = fake.melInputs[1]
        assertEquals((1280 - 480).toFloat(), second[0], 0f)
        assertEquals(1279f, second[479], 0f)
        assertEquals(10_000f, second[480], 0f)
        assertEquals(10_000f + 1279f, second[1759], 0f)
        val third = fake.melInputs[2]
        assertEquals(10_000f + 800f, third[0], 0f)
        assertEquals(20_000f, third[480], 0f)
    }

    @Test
    fun melFramesPerChunk() {
        buf.process(chunk())
        assertEquals(5, buf.lastMelFrames)
        repeat(5) {
            buf.process(chunk())
            assertEquals(FeatureBuffer.MEL_STEP_FRAMES, buf.lastMelFrames)
        }
    }

    @Test
    fun firstEmbeddingWindowIsOnesPaddingPlusTransformedFrames() {
        buf.process(chunk())
        val w = fake.embeddingWindows.single()
        assertEquals(76 * 32, w.size)
        for (r in 0 until 71) assertEquals("padding row $r", 1f, fake.row(w, r), 0f)
        for (r in 71 until 76) assertEquals((FIRST_FRAME_ID + r - 71).toFloat(), fake.row(w, r), 1e-3f)
    }

    @Test
    fun embeddingWindowIsLast76FramesAdvancingBy8() {
        val calls = 20
        repeat(calls) { buf.process(chunk()) }
        assertEquals(calls, fake.embeddingWindows.size)
        for (k in 1 until calls) {
            val w = fake.embeddingWindows[k]
            // Total frames produced after call k (0-based): 5 + 8k; window holds the last 76.
            val newestId = FIRST_FRAME_ID + 5 + 8 * k - 1
            val prev = fake.embeddingWindows[k - 1]
            assertEquals(newestId.toFloat(), fake.row(w, 75), 1e-3f)
            // Step 8: row r of window k == row r+8 of window k-1.
            for (r in 0 until 68) assertEquals(fake.row(prev, r + 8), fake.row(w, r), 0f)
            if (newestId - 75 >= FIRST_FRAME_ID) {
                for (r in 0 until 76) assertEquals((newestId - 75 + r).toFloat(), fake.row(w, r), 1e-3f)
            }
        }
        // After 10 calls 5 + 72 = 77 >= 76 real frames: no padding left.
        val w9 = fake.embeddingWindows[9]
        assertTrue(fake.row(w9, 0) >= FIRST_FRAME_ID)
    }

    @Test
    fun scoreIsZeroUntilWarmAndWakeModelNotRun() {
        for (i in 1 until N) {
            assertEquals(0f, buf.process(chunk()), 0f)
            assertFalse(buf.isWarm)
        }
        assertTrue(fake.wakeInputs.isEmpty())
        assertEquals(SCORE, buf.process(chunk()), 0f)
        assertTrue(buf.isWarm)
        assertEquals(1, fake.wakeInputs.size)
    }

    @Test
    fun wakeInputHoldsLastNEmbeddingsOldestFirst() {
        repeat(N + 3) { buf.process(chunk()) }
        val input = fake.wakeInputs.last()
        assertEquals(N * FeatureBuffer.EMBEDDING_DIM, input.size)
        // Embeddings 4..N+3 (the fake numbers them 1, 2, 3, ...).
        for (i in 0 until N) {
            val expected = (4 + i).toFloat()
            for (d in 0 until FeatureBuffer.EMBEDDING_DIM) {
                assertEquals(expected, input[i * FeatureBuffer.EMBEDDING_DIM + d], 0f)
            }
        }
    }

    @Test
    fun embeddingFramesFollowModelShape() {
        val fake28 = FakeModels()
        val b = FeatureBuffer(fake28, 28)
        repeat(27) { assertEquals(0f, b.process(chunk()), 0f) }
        assertEquals(SCORE, b.process(chunk()), 0f)
        assertEquals(28 * 96, fake28.wakeInputs.single().size)
    }

    @Test
    fun resetClearsEverything() {
        repeat(N + 2) { buf.process(chunk()) }
        assertTrue(buf.isWarm)
        buf.reset()
        assertFalse(buf.isWarm)
        assertEquals(0, buf.embeddingsProduced)
        assertTrue(buf.melWindowSnapshot().all { it == 1f })
        assertTrue(buf.featuresSnapshot().all { it == 0f })

        val melCalls = fake.melInputLengths.size
        val wakeCalls = fake.wakeInputs.size
        assertEquals(0f, buf.process(chunk(5)), 0f)
        // No overlap carried over: first call after reset is a bare 1280-sample chunk.
        assertEquals(1280, fake.melInputLengths[melCalls])
        assertEquals(5f, fake.melInputs[melCalls][0], 0f)
        assertEquals(5, buf.lastMelFrames)
        val w = fake.embeddingWindows.last()
        for (r in 0 until 71) assertEquals(1f, fake.row(w, r), 0f)
        repeat(N - 2) { assertEquals(0f, buf.process(chunk()), 0f) }
        assertEquals(wakeCalls, fake.wakeInputs.size)
        assertEquals(SCORE, buf.process(chunk()), 0f)
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsWrongFrameSize() {
        buf.process(ShortArray(1279))
    }

    @Test
    fun transformIsDivTenPlusTwo() {
        val models = object : FeatureModels {
            var window: FloatArray? = null
            override fun melspectrogram(samples: FloatArray, length: Int, out: FloatArray): Int {
                for (i in 0 until 5 * 32) out[i] = 30f
                return 5
            }
            override fun embedding(melWindow: FloatArray, out: FloatArray) { window = melWindow.copyOf() }
            override fun wakeScore(features: FloatArray) = 0f
        }
        FeatureBuffer(models, 1).process(chunk())
        val expected = FloatArray(76 * 32) { if (it >= 71 * 32) 5f else 1f }
        assertArrayEquals(expected, models.window, 0f)
    }

    private companion object {
        const val N = 16
        const val FIRST_FRAME_ID = 100
        const val SCORE = 0.75f
    }
}
