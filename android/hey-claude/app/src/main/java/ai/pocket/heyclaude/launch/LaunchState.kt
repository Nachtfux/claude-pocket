package ai.pocket.heyclaude.launch

import android.os.SystemClock

/**
 * Shared state between [ClaudeLauncher] (called from WakeWordService / MainActivity)
 * and [VoiceModeA11yService]. All timestamps use [SystemClock.elapsedRealtime].
 */
internal object LaunchState {
    /** How long after a launch the auto-tap is still attempted. */
    const val PENDING_TAP_WINDOW_MS = 15_000L

    /** Timestamp of the last launch that wants an auto-tap; 0 = none pending. */
    @Volatile
    var pendingTapAt: Long = 0L
        private set

    /** True between a wake-word launch and the RESUME broadcast. */
    @Volatile
    var sessionActive: Boolean = false
        private set

    /** Set once the Claude app has actually been seen in the foreground for this session. */
    @Volatile
    var claudeSeen: Boolean = false

    @Synchronized
    fun onLaunch(wantTap: Boolean) {
        sessionActive = true
        claudeSeen = false
        pendingTapAt = if (wantTap) SystemClock.elapsedRealtime() else 0L
    }

    fun tapPending(): Boolean {
        val t = pendingTapAt
        return t != 0L && SystemClock.elapsedRealtime() - t < PENDING_TAP_WINDOW_MS
    }

    fun clearTap() {
        pendingTapAt = 0L
    }

    @Synchronized
    fun endSession() {
        sessionActive = false
        claudeSeen = false
        pendingTapAt = 0L
    }
}
