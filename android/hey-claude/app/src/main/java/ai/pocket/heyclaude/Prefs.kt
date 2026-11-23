package ai.pocket.heyclaude

import android.content.Context
import android.content.SharedPreferences
import android.util.Log

object Prefs {
    private const val KEY_THRESHOLD = "threshold"
    private const val KEY_CLAUDE_PACKAGE = "claude_package"
    private const val KEY_VOICE_BUTTON_PATTERN = "voice_button_pattern"
    private const val KEY_NEW_CHAT_PATTERN = "new_chat_pattern"
    private const val KEY_RESUME_TIMEOUT = "resume_timeout_s"
    private const val KEY_AUTO_TAP = "auto_tap"
    private const val KEY_SERVICE_ENABLED = "service_enabled"

    const val DEFAULT_THRESHOLD = 0.5f
    const val DEFAULT_VOICE_BUTTON_PATTERN = "(?i)voice|sprach|stimm|sound ?wave|audio"
    const val DEFAULT_NEW_CHAT_PATTERN = "(?i)new chat|neuer chat|start new"
    const val DEFAULT_RESUME_TIMEOUT_S = 600

    private fun sp(ctx: Context): SharedPreferences =
        ctx.applicationContext.getSharedPreferences(Constants.PREFS, Context.MODE_PRIVATE)

    fun threshold(ctx: Context): Float = sp(ctx).getFloat(KEY_THRESHOLD, DEFAULT_THRESHOLD)

    fun setThreshold(ctx: Context, value: Float) {
        sp(ctx).edit().putFloat(KEY_THRESHOLD, value.coerceIn(0.05f, 0.99f)).apply()
    }

    fun claudePackage(ctx: Context): String =
        sp(ctx).getString(KEY_CLAUDE_PACKAGE, null)?.trim()?.takeIf { it.isNotEmpty() }
            ?: Constants.DEFAULT_CLAUDE_PACKAGE

    fun voiceButtonPattern(ctx: Context): Regex =
        regexOrDefault(sp(ctx).getString(KEY_VOICE_BUTTON_PATTERN, null), DEFAULT_VOICE_BUTTON_PATTERN)

    fun newChatPattern(ctx: Context): Regex =
        regexOrDefault(sp(ctx).getString(KEY_NEW_CHAT_PATTERN, null), DEFAULT_NEW_CHAT_PATTERN)

    /** Safety net: listening resumes after this many seconds if no RESUME broadcast arrives. */
    fun resumeTimeoutSec(ctx: Context): Int =
        sp(ctx).getInt(KEY_RESUME_TIMEOUT, DEFAULT_RESUME_TIMEOUT_S).coerceAtLeast(10)

    fun autoTap(ctx: Context): Boolean = sp(ctx).getBoolean(KEY_AUTO_TAP, true)

    /** Whether the user wants the listener running (honoured by BootReceiver). */
    fun serviceEnabled(ctx: Context): Boolean = sp(ctx).getBoolean(KEY_SERVICE_ENABLED, true)

    fun setServiceEnabled(ctx: Context, enabled: Boolean) {
        sp(ctx).edit().putBoolean(KEY_SERVICE_ENABLED, enabled).apply()
    }

    private fun regexOrDefault(stored: String?, default: String): Regex {
        if (!stored.isNullOrBlank()) {
            try {
                return Regex(stored)
            } catch (e: Exception) {
                Log.w(Constants.TAG, "Invalid regex in prefs '$stored', using default", e)
            }
        }
        return Regex(default)
    }
}
