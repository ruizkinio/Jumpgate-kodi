# Jumpgate

Jumpgate is an Android Kodi fork built to be a source-aware external player for
Stremio. It keeps Kodi's playback engine, settings, skins, local history, subtitle
controls, and installed subtitle addons while a private Bridge carries the exact
source context selected in Stremio.

> **Pre-release:** the source-backed overhaul is still completing stable signing,
> physical Android UAT, and coordinated release packaging. Development APKs are not
> public releases.

## What This Fork Adds

- Android external-player lifecycle handling that returns a result to the correct
  Stremio task without changing normal standalone Kodi navigation.
- Paired, isolated profiles and source claims from the actual Stremio provider result,
  without IP, filename, URL, artwork, or hash-based identity guessing.
- Local resume/history for canonical and local-only playback, with optional Trakt sync
  only when an exact canonical claim exists.
- Private delivery of Stremio text, ASS/SSA, and integrity-checked VobSub subtitles
  while preserving Kodi's subtitle picker, delay, styling, skins, and subtitle addons.
- Jumpgate Manager for pairing and profile management, plus a source-aware loading
  overlay and bounded metadata artwork cache.

Jumpgate does not provide media, a catalog, a debrid service, or stream sources. It is
designed to use the Stremio stream and subtitle addons selected by the user, subject to
the provider returning a valid Stremio resource and a transport Kodi can play.

## Install And Support

Use only APKs attached to a coordinated
[Jumpgate release](https://github.com/ruizkinio/Jumpgate/releases). Each release lists
the APK SHA-256, package, ABI, signer fingerprint, Bridge version, and public UAT
evidence. The package ID is `io.github.ruizkinio.jumpgate`, so a release build can
coexist with official Kodi.

Read the central [setup guide](https://github.com/ruizkinio/Jumpgate#setup),
[support policy](https://github.com/ruizkinio/Jumpgate/blob/main/SUPPORT.md), and
[device UAT protocol](https://github.com/ruizkinio/Jumpgate/blob/main/docs/UAT.md).
Issues are centralized in the
[Jumpgate repository](https://github.com/ruizkinio/Jumpgate/issues).

Never post a configured addon URL, install link, management link, pairing code,
profile/device token, provider URL, credential, or raw private log. These values can
grant access even when they look encrypted or short-lived.

## Upstream Kodi

Jumpgate is independent and is not affiliated with or endorsed by Kodi, Stremio,
Trakt, or TMDB. This fork remains GPL-2.0-or-later and preserves Kodi's notices,
history, build documentation, and attribution. The original Kodi README follows.

---

![Kodi Logo](docs/resources/banner.png)

<p align="center">
  <strong>
    <a href="https://kodi.tv/">website</a>
    •
    <a href="https://kodi.wiki/view/Main_Page">docs</a>
    •
    <a href="https://forum.kodi.tv/">community</a>
    •
    <a href="https://kodi.tv/addons">add-ons</a>
  </strong>
</p>

<p align="center">
  <a href="LICENSE.md"><img alt="License" src="https://img.shields.io/badge/license-GPLv2-blue.svg?style=flat-square"></a>
  <a href="https://docs.kodi.tv/"><img alt="Documentation" src="https://img.shields.io/badge/code-documented-brightgreen.svg?style=flat-square"></a>
  <a href="https://github.com/xbmc/xbmc/pulls"><img alt="PRs Welcome" src="https://img.shields.io/badge/PRs-welcome-brightgreen.svg?style=flat-square"></a>
  <a href="#how-to-contribute"><img alt="Contributions Welcome" src="https://img.shields.io/badge/contributions-welcome-brightgreen.svg?style=flat-square"></a>
  <a href="http://jenkins.kodi.tv/"><img alt="Build" src="https://img.shields.io/badge/CI-jenkins-brightgreen.svg?style=flat-square"></a>
  <a href="https://github.com/xbmc/xbmc/commits/master"><img alt="Commits" src="https://img.shields.io/github/commits-since/xbmc/xbmc/latest.svg?style=flat-square"></a>
</p>

<a href="https://play.google.com/store/apps/details?id=org.xbmc.kodi" target="_blank">
  <img src="https://play.google.com/intl/en_us/badges/images/generic/en-play-badge.png" height="80"/>
</a>

<h1 align="center">
  Welcome to Kodi Home Theater Software!
</h1>

Kodi is an award-winning **free and open source** software media player and entertainment hub for digital media. Available as a native application for **Android, Linux, BSD, macOS, iOS, tvOS and Windows operating systems**, Kodi runs on most common processor architectures.

Created in 2003 by a group of like minded programmers, Kodi is a non-profit project run by the XBMC Foundation and developed by volunteers located around the world. More than 500 software developers have contributed to Kodi to date, and 100-plus translators have worked to expand its reach, making it available in more than 70 languages.

While Kodi functions very well as a standard media player application for your computer, it has been designed to be the perfect companion for your HTPC. With its **beautiful interface and powerful skinning engine**, Kodi feels very natural to use from the couch with a remote control and is the ideal solution for your home theater.

## Give your media the love it deserves
Kodi can be used to play almost all popular audio and video formats around. It was designed for network playback, so you can stream your multimedia from anywhere in the house or directly from the internet using practically any protocol available.

Point Kodi to your media and watch it **scan and automagically create a personalized library** complete with box covers, descriptions, and fanart. There are playlist and slideshow functions, a weather forecast feature and many audio visualizations. Once installed, your computer or HTPC will become a fully functional multimedia jukebox.

<p align="center">
  <img src="docs/resources/kodi.gif" alt="Kodi">
</p>

## Getting Started
Kodi's developers work hard to make it support a large range of devices and operating systems. We provide final as well as development builds. To get started, head over to the **[downloads section](https://kodi.tv/download)** and simply select the platform that you want to install it on. A **[quick start guide](https://kodi.wiki/view/quick_start_guide)** to help you get acquainted with Kodi is available in our wiki.

## How to Contribute
Kodi is created by users for users and **we welcome every contribution**. There are no highly paid developers or poorly paid support personnel on the phones ready to take your call. There are only users who have seen a problem and done their best to fix it. This means Kodi will always need the contributions of users like you. How can you get involved?

* **Coding:** Developers can help Kodi by **[fixing a bug](https://github.com/xbmc/xbmc/issues)**, adding new features, making our technology smaller and faster and making development easier for others. Kodi's codebase consists mainly of C++ with small parts written in a variety of coding languages. Our add-ons mainly consist of python and XML. For more information, please have a look at our **[contributing guide](docs/CONTRIBUTING.md)**.
* **Helping users:** Our support process relies on enthusiastic contributors like you to help others get the most out of Kodi. The #1 priority is always answering questions in our **[support forums](https://forum.kodi.tv/)**. Everyday new people discover Kodi, and everyday they are virtually guaranteed to have questions.
* **Localization:** Translate **[Kodi](https://kodi.weblate.cloud/projects/kodi-core/kodi-main/)**, **[add-ons, skins etc.](https://kodi.weblate.cloud/)** into your native language.
* **Add-ons:** **[Add-ons](https://kodi.tv/addons)** are what make Kodi the most extensible and customizable entertainment hub available. **[Get started building an add-on](https://kodi.tv/create-an-addon)**.
* **Documentation:** Kodi's **[wiki pages](https://kodi.wiki/)** are the hub for information about Kodi and surrounding ecosystem. Help make our documentation better by writing new content or correcting existing material.

**Not enough free time?** No problem! There are other ways to help Kodi.

* **Spread the word:** Share Kodi with the world! Tell your friends and family about how Kodi creates an amazing entertainment experience. Stay up to date on the latest stories about Kodi reading our **[news](https://kodi.tv/blog)** section, follow us on **[Twitter](https://twitter.com/koditv)** and **[Facebook](https://www.facebook.com/XBMC/)**, or **star Kodi's repo** if you want to follow development.
* **Donate:** We are always happy to receive a **[donation](https://kodi.tv/contribute/donate)**. Donations are typically used for travel to attend conferences, any necessary paperwork and legal fees, and the yearly XBMC Foundation Developers Conference, where a great deal of coding and planning for the following year occurs. Donations may also be used to purchase necessary hardware and licenses for developers, along with t-shirts, stickers, and other accessories for conferences.
* **Buy Kodi merchandise:** Purchasing Kodi gear helps just as much as a donation, and you get something in return! Checkout our **[store](https://kodi.tv/store)** for Kodi branded gear. We regularly add new products so check back often.

## Building
Kodi uses CMake as its building system but instructions are highly dependent on your operating system and target platform. Fortunately **[we've got you covered](docs/README.md)**.

## Acknowledgements
Kodi couldn't exist without

* All the **[contributors](https://github.com/xbmc/xbmc/graphs/contributors)**. Big or small a change, it does make a difference.
* All the developers that write the fantastic **software and libraries** that Kodi uses. We stand on the shoulders of giants.
* Our **[fantastic community](https://forum.kodi.tv/)** for the never ending support, inspiration, feedback, and for keeping us on our toes when we screw up!
* **[Our sponsors](https://kodi.tv/sponsors)**. Without them, keeping a huge project like this alive would be next to impossible.

## License
Kodi is **[GPLv2 licensed](LICENSE.md)**. You may use, distribute and copy it under the license terms.

<a href="https://github.com/xbmc/xbmc/graphs/contributors"><img src="https://forthebadge.com/images/badges/built-by-developers.svg" height="25"></a>
<a href="https://github.com/xbmc/xbmc"><img src="https://forthebadge.com/images/badges/certified-cousin-terio.svg" height="25"></a>
<a href="https://github.com/xbmc/xbmc"><img src="https://forthebadge.com/images/badges/approved-by-george-costanza.svg" height="25"></a>
<a href="https://kodi.tv/download"><img src="https://forthebadge.com/images/badges/check-it-out.svg" height="25"></a>
<a href="https://github.com/xbmc/xbmc"><img src="https://forthebadge.com/images/badges/winter-is-coming.svg" height="25"></a>
