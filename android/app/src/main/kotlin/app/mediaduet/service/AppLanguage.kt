package app.mediaduet.service

import android.app.Activity
import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.LocaleList
import androidx.core.content.edit
import java.util.Locale

/**
 * Per-app language without AppCompat: on API 33+ the platform LocaleManager stores the choice
 * (and android:localeConfig exposes it in system settings); below 33 the choice is kept in
 * preferences and applied by wrapping the base context of the application, activity and services.
 */
object AppLanguage {
    enum class Option(val tag: String?) { SYSTEM(null), ZH_CN("zh-CN"), EN("en") }

    private const val PREFS = "language"
    private const val KEY = "tag"

    fun current(context: Context): Option {
        val tag = if (Build.VERSION.SDK_INT >= 33) {
            context.getSystemService(LocaleManager::class.java).applicationLocales.takeIf { !it.isEmpty }?.get(0)?.toLanguageTag()
        } else {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, null)
        }
        return when {
            tag == null -> Option.SYSTEM
            tag.startsWith("zh") -> Option.ZH_CN
            tag.startsWith("en") -> Option.EN
            else -> Option.SYSTEM
        }
    }

    fun set(activity: Activity, option: Option) {
        if (Build.VERSION.SDK_INT >= 33) {
            // The platform persists the choice and recreates activities itself.
            activity.getSystemService(LocaleManager::class.java).applicationLocales =
                if (option.tag == null) LocaleList.getEmptyLocaleList() else LocaleList.forLanguageTags(option.tag)
        } else {
            activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit { putString(KEY, option.tag) }
            activity.recreate()
        }
    }

    /** API < 33: returns a context whose resources use the chosen language. */
    fun wrap(base: Context): Context {
        if (Build.VERSION.SDK_INT >= 33) return base
        val tag = base.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, null) ?: return base
        val locale = Locale.forLanguageTag(tag)
        Locale.setDefault(locale)
        val config = Configuration(base.resources.configuration)
        config.setLocales(LocaleList(locale))
        return base.createConfigurationContext(config)
    }
}
