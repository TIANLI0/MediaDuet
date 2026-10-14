// MediaDuet website: language switch (zh-CN / en) and direct download links from the latest release.
(function () {
  "use strict";

  var REPO = "TIANLI0/MediaDuet";

  var en = {
    skip: "Skip to content",
    navLabel: "Page navigation",
    navFeatures: "Features",
    navStart: "Get started",
    navDownload: "Download",
    heroTitle: "Phone and PC, singing the same song.",
    heroLine1: "Phone and PC,",
    heroLine2: "one song.",
    heroLead: "MediaDuet lets your Android phone and your Windows PC see and control what the other is playing. Start something on one and the other steps aside. Lyrics light up word by word on the phone, in the app, on the desktop and in the taskbar, all at once.",
    ctaWindows: "Download for Windows",
    ctaAndroid: "Download for Android",
    heroFine: "Free and open source (MIT). Current version",
    altWin: "MediaDuet on Windows: album art and playback controls on the left, word-by-word lyrics with translation on the right",
    altPhoneLyrics: "MediaDuet lyrics on Android: lyrics fill the screen, a row of liquid-glass buttons at the bottom",
    f1Title: "Both sides can see it, both sides can control it",
    f1Body: "See what your PC is playing right on your phone: cover, title, progress. Pause, skip, scrub and change the PC's volume from the phone. And the PC can do the same for the phone.",
    f1Note: "Works with NetEase Cloud Music, QQ Music, Spotify, browsers and any player that uses the system media controls.",
    altPhonePlayer: "Android player: large cover, title, progress bar and three glass buttons",
    altCompact: "Windows mini player: cover, title and the current lyric line",
    f2Title: "Start on one, the other steps aside",
    f2Body: "Same earbuds, both devices: open a video on the PC and the music on your phone pauses by itself. When the video ends, the music picks up again. You decide who goes first and when to resume.",
    handoffLabel: "Automatic pause, step by step",
    hoPhone: "Phone",
    hoPc: "PC",
    ho1: "is playing “Evening Wind Letter”",
    ho2: "opens a video",
    ho3: "pauses automatically",
    ho4: "finishes the video and goes quiet",
    ho5: "continues where it left off",
    f3Title: "Lyrics, wherever you happen to look",
    f3Body: "Desktop lyrics float above every window and there is a line in the taskbar too. In the phone and PC players the lyrics light up word by word as they are sung. With Bluetooth earbuds the audio delay is compensated automatically.",
    altDesktop: "Desktop lyrics: the current line lights up word by word, with its translation below",
    altTaskbar: "Taskbar lyrics: the current and the next line",
    f3Note: "Lyrics come from NetEase Cloud Music, QQ Music and LRCLIB, matched by title, artist and length only. The phone finds lyrics on its own too, no PC needed.",
    altLandscape: "Android lyrics in landscape: cover and controls on the left, lyrics across the right",
    f4Title: "Only one device? That works too",
    f4Body: "On first launch choose “This phone only” or “This PC only” and MediaDuet becomes a playback controller with synced lyrics that listens for no connections. Switch in Settings whenever you want to pair.",
    f4Note: "Once paired, each side shows the other's version and reminds you when they don't match.",
    altWelcome: "Choosing how to use MediaDuet on first launch: with my PC, or this phone only",
    startTitle: "Three steps",
    s1Title: "Install both",
    s1Body: "Install the Windows app on the PC and the Android app on the phone, and allow the phone to read media information.",
    s2Title: "Pair on the PC",
    s2Body: "On the PC open Settings › Devices and pick your phone. It is found on the same Wi-Fi or over Bluetooth.",
    s3Title: "Compare six digits",
    s3Body: "Both sides show the same six digits. Confirm and you're connected; from then on they reconnect by themselves.",
    privTitle: "Only between your two devices",
    priv1: "The connection between phone and PC is end-to-end encrypted; the pairing key lives only on those two devices.",
    priv2: "Notification contents are never read. No accounts, no servers, no analytics.",
    priv3: "Looking up lyrics and covers sends only title, artist, album and length, and can be turned off in Settings.",
    dlTitle: "Download",
    dlWinReq: "Windows 10 / 11, x64",
    dlWinSetup: "Download installer",
    dlWinZip: "or the portable zip",
    dlAndroidReq: "Android 10 or later, 64-bit ARM",
    dlApk: "Download APK",
    dlAll: "All versions and release notes",
    dlNote: "Keep the phone and the PC on the same version.",
    devTitle: "Developer",
    footLicense: "MediaDuet is open source under the MIT License.",
    footLicenseLink: "License",
    footNotices: "Third-party components",
    footIssues: "Report an issue",
    pageTitle: "MediaDuet: phone and PC, one song",
    langButton: "中文",
    langLabel: "切换到中文"
  };

  var zh = {};
  var nodes = document.querySelectorAll("[data-i18n]");
  nodes.forEach(function (n) { zh[n.getAttribute("data-i18n")] = n.textContent; });
  document.querySelectorAll("[data-i18n-alt]").forEach(function (n) { zh[n.getAttribute("data-i18n-alt")] = n.getAttribute("alt"); });
  document.querySelectorAll("[data-i18n-aria]").forEach(function (n) { zh[n.getAttribute("data-i18n-aria")] = n.getAttribute("aria-label"); });
  zh.pageTitle = document.title;
  zh.langButton = "EN";
  zh.langLabel = "Switch to English";

  function stored() {
    try { return localStorage.getItem("md-lang"); } catch (e) { return null; }
  }
  function store(v) {
    try { localStorage.setItem("md-lang", v); } catch (e) { /* private mode: keep it for this visit only */ }
  }

  var query = new URLSearchParams(location.search).get("lang");
  var lang = query === "en" || query === "zh" ? query : stored() || ((navigator.language || "").toLowerCase().indexOf("zh") === 0 ? "zh" : "en");

  function apply(l) {
    var t = l === "en" ? en : zh;
    document.documentElement.lang = l === "en" ? "en" : "zh-CN";
    nodes.forEach(function (n) { var k = n.getAttribute("data-i18n"); if (t[k]) n.textContent = t[k]; });
    document.querySelectorAll("[data-i18n-alt]").forEach(function (n) { var k = n.getAttribute("data-i18n-alt"); if (t[k]) n.setAttribute("alt", t[k]); });
    document.querySelectorAll("[data-i18n-aria]").forEach(function (n) { var k = n.getAttribute("data-i18n-aria"); if (t[k]) n.setAttribute("aria-label", t[k]); });
    document.querySelectorAll("[data-src-zh]").forEach(function (n) { n.src = n.getAttribute(l === "en" ? "data-src-en" : "data-src-zh"); });
    document.title = t.pageTitle;
    var b = document.getElementById("lang");
    b.textContent = t.langButton;
    b.setAttribute("aria-label", t.langLabel);
  }

  apply(lang);
  document.getElementById("lang").addEventListener("click", function () {
    lang = lang === "en" ? "zh" : "en";
    store(lang);
    apply(lang);
  });

  // Direct links to the latest release's files; without the API the links open the release page.
  fetch("https://api.github.com/repos/" + REPO + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (rel) {
      if (!rel || !rel.assets) return;
      var pick = {
        setup: /-x64-setup\.exe$/i,
        zip: /-win-x64\.zip$/i,
        apk: /-android\.apk$/i
      };
      Object.keys(pick).forEach(function (kind) {
        var a = rel.assets.filter(function (x) { return pick[kind].test(x.name); })[0];
        if (!a) return;
        document.querySelectorAll('[data-dl="' + kind + '"]').forEach(function (el) { el.href = a.browser_download_url; });
      });
      var v = (rel.tag_name || "").replace(/^v/, "");
      if (v) document.querySelectorAll(".ver").forEach(function (el) { el.textContent = v; });
    })
    .catch(function () { /* offline or rate limited: the release page links still work */ });
})();
