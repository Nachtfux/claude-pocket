package ai.pocket.heyclaude

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.util.Log

/**
 * Starts listening after boot (and after an app update).
 *
 * Android 11 "while-in-use" rules: a microphone foreground service started
 * from the background (e.g. straight from BOOT_COMPLETED) gets silent audio.
 * So when we are allowed to start activities from the background
 * (SYSTEM_ALERT_WINDOW granted by post-install.sh), we bounce through
 * MainActivity, which starts the service while it is in the foreground and
 * then closes itself. Otherwise we start the service directly as a fallback.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) return
        if (!Prefs.serviceEnabled(context)) {
            Log.i(Constants.TAG, "Boot: listener disabled by user, not starting")
            return
        }
        Log.i(Constants.TAG, "Boot: starting wake word listener ($action)")

        if (Settings.canDrawOverlays(context)) {
            try {
                val i = Intent(context, MainActivity::class.java)
                    .putExtra(MainActivity.EXTRA_AUTOSTART, true)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
                context.startActivity(i)
                return
            } catch (e: Exception) {
                Log.w(Constants.TAG, "Boot: activity bounce failed, starting service directly", e)
            }
        }
        WakeWordService.start(context)
    }
}
