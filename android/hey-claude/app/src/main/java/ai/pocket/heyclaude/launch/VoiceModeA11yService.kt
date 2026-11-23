package ai.pocket.heyclaude.launch

import ai.pocket.heyclaude.Constants
import ai.pocket.heyclaude.Prefs
import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Path
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.view.inputmethod.InputMethodManager
import androidx.core.content.ContextCompat

/**
 * Accessibility service with two jobs:
 *  1. After a wake-word launch, find and click the voice-mode button in the Claude app
 *     (there is no official deep link for voice mode).
 *  2. Notice when the Claude app leaves the foreground and send [Constants.ACTION_RESUME]
 *     so WakeWordService starts listening again.
 *
 * Debug: `adb shell am broadcast -a ai.pocket.heyclaude.DUMP_NODES -p ai.pocket.heyclaude`
 * dumps the current window tree to logcat (tag HeyClaude).
 */
class VoiceModeA11yService : AccessibilityService() {

    companion object {
        const val ACTION_DUMP_NODES = "ai.pocket.heyclaude.DUMP_NODES"

        private const val TAG = Constants.TAG
        private const val RETRY_MS = 400L
        private const val MAX_TRIES = 8
        private const val EXTRA_TRIES_AFTER_NEW_CHAT = 4
        private const val LEAVE_DEBOUNCE_MS = 1_500L
        private const val SESSION_SCREEN_MAX_MS = 30 * 60_000L
        private const val MAX_DEPTH = 40
        private const val MAX_NODES = 1_500
        private const val MAX_TEXT_MATCH_LEN = 40
        private const val MAX_CANDIDATES_LOGGED = 60

        @Volatile
        private var instance: VoiceModeA11yService? = null

        val isConnected: Boolean get() = instance != null

        /** Called by [ClaudeLauncher] right after startActivity (any thread). */
        fun onLaunchRequested() {
            val svc = instance ?: return
            svc.handler.post { svc.onLaunch() }
        }
    }

    private val handler = Handler(Looper.getMainLooper())

    // --- auto-tap state (main thread only) ---
    private var tries = 0
    private var maxTries = MAX_TRIES
    private var newChatClicked = false
    private var lastAttemptAt = 0L
    private var tapLoopRunning = false

    // --- foreground tracking (main thread only) ---
    private var foregroundPkg: String? = null
    private var leaveCheckPosted = false
    private var imePackages: Set<String> = emptySet()

    private var receiverRegistered = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_DUMP_NODES -> dumpAllWindows()
                Intent.ACTION_SCREEN_OFF -> {
                    // Screen went dark while a session was running: the conversation is over.
                    if (LaunchState.sessionActive && LaunchState.claudeSeen) {
                        sendResume("screen off")
                    }
                }
            }
        }
    }

    private val tapRunnable = Runnable { tapTick() }

    private val leaveCheck = Runnable {
        leaveCheckPosted = false
        checkLeft()
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this
        refreshImePackages()
        try {
            val filter = IntentFilter().apply {
                addAction(ACTION_DUMP_NODES)
                addAction(Intent.ACTION_SCREEN_OFF)
            }
            // Exported so that `adb shell am broadcast` (shell uid) can reach it.
            ContextCompat.registerReceiver(this, receiver, filter, ContextCompat.RECEIVER_EXPORTED)
            receiverRegistered = true
        } catch (e: Exception) {
            Log.w(TAG, "A11y: registerReceiver failed", e)
        }
        Log.i(TAG, "A11y: service connected")
        if (LaunchState.tapPending()) startTapSequence()
    }

    override fun onInterrupt() {
        Log.i(TAG, "A11y: interrupted")
        stopTapLoop()
    }

    /**
     * Keep the display on while a voice session runs. Otherwise the screen
     * timeout (post-install.sh sets 120 s) turns it off mid-conversation, the
     * SCREEN_OFF handler resumes our AudioRecord and Android may hand the mic
     * to us instead of Claude. Capped so a stuck session can't burn the panel.
     */
    private var screenLock: PowerManager.WakeLock? = null

    @Suppress("DEPRECATION")
    private fun holdScreen() {
        if (screenLock?.isHeld == true) return
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            screenLock = pm.newWakeLock(PowerManager.SCREEN_DIM_WAKE_LOCK, "HeyClaude:session").apply {
                setReferenceCounted(false)
                acquire(SESSION_SCREEN_MAX_MS)
            }
        } catch (e: Exception) {
            Log.w(TAG, "A11y: screen wake lock failed", e)
        }
    }

    private fun releaseScreen() {
        try {
            screenLock?.takeIf { it.isHeld }?.release()
        } catch (_: Exception) {
        }
        screenLock = null
    }

    override fun onDestroy() {
        releaseScreen()
        instance = null
        stopTapLoop()
        handler.removeCallbacks(leaveCheck)
        if (receiverRegistered) {
            try {
                unregisterReceiver(receiver)
            } catch (_: Exception) {
            }
            receiverRegistered = false
        }
        Log.i(TAG, "A11y: service destroyed")
        super.onDestroy()
    }

    // ------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        event ?: return
        val pkg = event.packageName?.toString() ?: return
        val claude = Prefs.claudePackage(this)

        when (event.eventType) {
            AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED -> {
                if (!isIgnoredPackage(pkg)) onForeground(pkg, claude)
                if (pkg == claude && LaunchState.tapPending()) startTapSequence()
            }
            AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED -> {
                if (pkg == claude && LaunchState.tapPending() && tapLoopRunning) {
                    // Content changed: try sooner than the next scheduled tick (throttled).
                    if (SystemClock.elapsedRealtime() - lastAttemptAt >= RETRY_MS / 2) {
                        handler.removeCallbacks(tapRunnable)
                        handler.post(tapRunnable)
                    }
                }
            }
        }
    }

    private fun onForeground(pkg: String, claude: String) {
        if (pkg == foregroundPkg) return
        foregroundPkg = pkg
        if (!LaunchState.sessionActive) return
        if (pkg == claude) {
            if (!LaunchState.claudeSeen) Log.i(TAG, "A11y: Claude is in foreground")
            LaunchState.claudeSeen = true
            holdScreen()
            handler.removeCallbacks(leaveCheck)
            leaveCheckPosted = false
        } else if (LaunchState.claudeSeen && !leaveCheckPosted) {
            Log.i(TAG, "A11y: foreground changed to $pkg, checking in ${LEAVE_DEBOUNCE_MS}ms")
            leaveCheckPosted = true
            handler.postDelayed(leaveCheck, LEAVE_DEBOUNCE_MS)
        }
    }

    private fun checkLeft() {
        if (!LaunchState.sessionActive || !LaunchState.claudeSeen) return
        val claude = Prefs.claudePackage(this)
        val activePkg = try {
            rootInActiveWindow?.packageName?.toString()
        } catch (_: Exception) {
            null
        }
        val current = activePkg?.takeUnless { isIgnoredPackage(it) } ?: foregroundPkg
        if (current == claude) {
            Log.i(TAG, "A11y: Claude is back in foreground, no resume")
            foregroundPkg = claude
            return
        }
        sendResume("Claude left foreground (now $current)")
    }

    private fun sendResume(reason: String) {
        Log.i(TAG, "A11y: sending RESUME ($reason)")
        releaseScreen()
        LaunchState.endSession()
        stopTapLoop()
        handler.removeCallbacks(leaveCheck)
        leaveCheckPosted = false
        try {
            sendBroadcast(Intent(Constants.ACTION_RESUME).setPackage(packageName))
        } catch (e: Exception) {
            Log.w(TAG, "A11y: RESUME broadcast failed", e)
        }
    }

    private fun isIgnoredPackage(pkg: String): Boolean =
        pkg == packageName ||
            pkg == "com.android.systemui" ||
            pkg == "android" ||
            pkg.endsWith(".permissioncontroller") ||
            pkg.endsWith(".packageinstaller") ||
            pkg in imePackages

    private fun refreshImePackages() {
        imePackages = try {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.inputMethodList.map { it.packageName }.toSet()
        } catch (e: Exception) {
            emptySet()
        }
    }

    // ------------------------------------------------------------------
    // Auto-tap
    // ------------------------------------------------------------------

    /** A new wake-word launch happened: reset per-session state. */
    private fun onLaunch() {
        stopTapLoop()
        handler.removeCallbacks(leaveCheck)
        leaveCheckPosted = false
        foregroundPkg = null
        refreshImePackages()
        // Claude may already be in front, in which case no window-state event follows.
        val active = try {
            rootInActiveWindow?.packageName?.toString()
        } catch (_: Exception) {
            null
        }
        if (active != null && !isIgnoredPackage(active)) onForeground(active, Prefs.claudePackage(this))
        startTapSequence()
    }

    private fun startTapSequence() {
        if (!LaunchState.tapPending()) return
        if (tapLoopRunning) return
        tapLoopRunning = true
        tries = 0
        maxTries = MAX_TRIES
        newChatClicked = false
        lastAttemptAt = 0L
        Log.i(TAG, "A11y: auto-tap sequence started")
        handler.removeCallbacks(tapRunnable)
        handler.postDelayed(tapRunnable, RETRY_MS)
    }

    private fun stopTapLoop() {
        tapLoopRunning = false
        handler.removeCallbacks(tapRunnable)
    }

    private fun tapTick() {
        if (!tapLoopRunning) return
        if (!LaunchState.tapPending()) {
            Log.i(TAG, "A11y: auto-tap window expired after $tries tries")
            LaunchState.clearTap()
            stopTapLoop()
            return
        }
        lastAttemptAt = SystemClock.elapsedRealtime()
        val claude = Prefs.claudePackage(this)
        val root = try {
            rootInActiveWindow
        } catch (_: Exception) {
            null
        }
        if (root == null || root.packageName?.toString() != claude) {
            // Claude not (yet) in front: keep waiting without burning a try.
            Log.d(TAG, "A11y: waiting for Claude window (active=${root?.packageName})")
            scheduleNext()
            return
        }

        tries++
        val voicePattern = Prefs.voiceButtonPattern(this)
        val nodes = collect(root)
        val voice = bestMatch(nodes, voicePattern)
        if (voice != null) {
            Log.i(TAG, "A11y: try $tries/$maxTries: voice button match ${describe(voice)}")
            if (click(voice)) {
                Log.i(TAG, "A11y: voice-mode button tapped")
                LaunchState.clearTap()
                stopTapLoop()
                return
            }
            Log.w(TAG, "A11y: click on voice button failed")
        } else {
            Log.i(TAG, "A11y: try $tries/$maxTries: no node matches $voicePattern")
            if (!newChatClicked) {
                val newChat = bestMatch(nodes, Prefs.newChatPattern(this))
                if (newChat != null) {
                    Log.i(TAG, "A11y: clicking new-chat ${describe(newChat)}")
                    if (click(newChat)) {
                        newChatClicked = true
                        maxTries = tries + EXTRA_TRIES_AFTER_NEW_CHAT
                    }
                }
            }
            if (tries >= maxTries || tries == 1) logCandidates(nodes)
        }

        if (tries >= maxTries) {
            Log.w(TAG, "A11y: giving up auto-tap after $tries tries; set voice_button_pattern to a label above")
            LaunchState.clearTap()
            stopTapLoop()
            return
        }
        scheduleNext()
    }

    private fun scheduleNext() {
        handler.removeCallbacks(tapRunnable)
        handler.postDelayed(tapRunnable, RETRY_MS)
    }

    /** BFS over the tree, bounded by depth and node count. */
    private fun collect(root: AccessibilityNodeInfo): List<AccessibilityNodeInfo> {
        val out = ArrayList<AccessibilityNodeInfo>()
        val queue = ArrayDeque<Pair<AccessibilityNodeInfo, Int>>()
        queue.addLast(root to 0)
        while (queue.isNotEmpty() && out.size < MAX_NODES) {
            val (node, depth) = queue.removeFirst()
            out.add(node)
            if (depth >= MAX_DEPTH) continue
            for (i in 0 until node.childCount) {
                val child = try {
                    node.getChild(i)
                } catch (_: Exception) {
                    null
                } ?: continue
                queue.addLast(child to depth + 1)
            }
        }
        return out
    }

    /**
     * Picks the best node matching [pattern]: contentDescription beats viewId beats short text,
     * and visible/clickable nodes win. Long texts are ignored (they are chat messages).
     */
    private fun bestMatch(nodes: List<AccessibilityNodeInfo>, pattern: Regex): AccessibilityNodeInfo? {
        var best: AccessibilityNodeInfo? = null
        var bestScore = 0
        for (n in nodes) {
            var score = 0
            val desc = n.contentDescription?.toString()
            val id = n.viewIdResourceName
            val text = n.text?.toString()
            if (desc != null && desc.length <= 80 && pattern.containsMatchIn(desc)) score = 30
            else if (id != null && pattern.containsMatchIn(id.substringAfter('/'))) score = 20
            else if (text != null && text.length <= MAX_TEXT_MATCH_LEN && pattern.containsMatchIn(text)) score = 10
            if (score == 0) continue
            if (n.isVisibleToUser) score += 5 else score -= 15
            if (n.isClickable) score += 3
            if (n.isEditable) score -= 20
            if (score > bestScore) {
                bestScore = score
                best = n
            }
        }
        return best
    }

    private fun click(node: AccessibilityNodeInfo): Boolean {
        var n: AccessibilityNodeInfo? = node
        var hops = 0
        while (n != null && hops < 6) {
            if (n.isClickable && n.isEnabled) {
                if (n.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
                break
            }
            n = n.parent
            hops++
        }
        return tapAt(node)
    }

    private fun tapAt(node: AccessibilityNodeInfo): Boolean {
        val r = Rect()
        node.getBoundsInScreen(r)
        if (r.isEmpty) return false
        val path = Path().apply { moveTo(r.exactCenterX(), r.exactCenterY()) }
        val gesture = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(path, 0, 60))
            .build()
        Log.i(TAG, "A11y: gesture tap at ${r.centerX()},${r.centerY()}")
        return try {
            dispatchGesture(gesture, null, null)
        } catch (e: Exception) {
            Log.w(TAG, "A11y: dispatchGesture failed", e)
            false
        }
    }

    // ------------------------------------------------------------------
    // Logging / debugging
    // ------------------------------------------------------------------

    private fun describe(n: AccessibilityNodeInfo): String {
        val r = Rect()
        n.getBoundsInScreen(r)
        val cls = n.className?.toString()?.substringAfterLast('.') ?: "?"
        val sb = StringBuilder(cls)
        n.viewIdResourceName?.let { sb.append(" id=").append(it.substringAfter('/')) }
        n.contentDescription?.let { sb.append(" desc=\"").append(clip(it)).append('"') }
        n.text?.let { sb.append(" text=\"").append(clip(it)).append('"') }
        if (n.isClickable) sb.append(" [click]")
        if (!n.isVisibleToUser) sb.append(" [hidden]")
        sb.append(' ').append(r.toShortString())
        return sb.toString()
    }

    private fun clip(cs: CharSequence): String {
        val s = cs.toString().replace('\n', ' ')
        return if (s.length > 40) s.substring(0, 40) + "…" else s
    }

    /** Compact dump of nodes that could plausibly be the button we are looking for. */
    private fun logCandidates(nodes: List<AccessibilityNodeInfo>) {
        val cands = nodes.filter {
            it.isClickable || !it.contentDescription.isNullOrEmpty() || it.viewIdResourceName != null
        }
        Log.i(TAG, "A11y: ${cands.size} candidate nodes (of ${nodes.size}):")
        cands.take(MAX_CANDIDATES_LOGGED).forEachIndexed { i, n -> Log.i(TAG, "  cand[$i] ${describe(n)}") }
        if (cands.size > MAX_CANDIDATES_LOGGED) Log.i(TAG, "  … ${cands.size - MAX_CANDIDATES_LOGGED} more")
    }

    private fun dumpAllWindows() {
        val roots = try {
            windows.mapNotNull { w -> w.root?.let { "window '${w.title}' type=${w.type}" to it } }
        } catch (_: Exception) {
            emptyList()
        }.ifEmpty {
            listOfNotNull(rootInActiveWindow?.let { "active window" to it })
        }
        Log.i(TAG, "A11y: DUMP_NODES — ${roots.size} window(s), foreground=$foregroundPkg")
        for ((label, root) in roots) {
            Log.i(TAG, "=== $label pkg=${root.packageName} ===")
            var count = 0
            fun walk(n: AccessibilityNodeInfo, depth: Int) {
                if (count >= MAX_NODES) return
                count++
                Log.i(TAG, "  ".repeat(depth.coerceAtMost(20)) + describe(n))
                if (depth >= MAX_DEPTH) return
                for (i in 0 until n.childCount) {
                    val c = try {
                        n.getChild(i)
                    } catch (_: Exception) {
                        null
                    } ?: continue
                    walk(c, depth + 1)
                }
            }
            walk(root, 0)
        }
        Log.i(TAG, "A11y: DUMP_NODES done")
    }
}
