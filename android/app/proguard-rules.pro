# MediaDuet release rules.
# All JSON is (de)serialised manually through kotlinx.serialization JsonElement trees, so no
# reflection-based rules are required. Services/receivers referenced from the manifest are kept
# by AAPT-generated rules.
-dontwarn org.jetbrains.annotations.**
