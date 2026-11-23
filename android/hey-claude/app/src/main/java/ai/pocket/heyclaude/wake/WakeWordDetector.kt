package ai.pocket.heyclaude.wake

/**
 * Streaming wake-word detector.
 *
 * Callers feed exactly [frameSamples] mono 16 kHz PCM16 samples per [process] call and get
 * the wake-word score (0..1) for that step back. Implementations are stateful; call [reset]
 * after a detection (or whenever the audio stream is interrupted) and [close] when done.
 */
interface WakeWordDetector {
    /** Samples per [process] call: 1280 = 80 ms @ 16 kHz. */
    val frameSamples: Int

    /** Feeds one frame of exactly [frameSamples] samples; returns the score for this frame. */
    fun process(pcm: ShortArray): Float

    /** Clears all streaming state (audio, mel and embedding history). */
    fun reset()

    /** Releases native resources. The detector must not be used afterwards. */
    fun close()
}
