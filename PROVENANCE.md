# Provenance

nx2penguin stands on other people's open-source work. What came from where, for
the parts this project reuses or ports:

- [shirai91/nx2android](https://github.com/shirai91/nx2android) (MIT as
  nxnative): the Android host, the NVN→Vulkan replay backend this project's
  Vulkan code descends from, porter, and the game profiles.
- [nimrc/nx2linux](https://github.com/nimrc/nx2linux): the Linux host this
  project mirrors as its base (dmabuf zero-copy pools in `ahb_vk.c`, guest
  memory below 2³⁹, `CPU_NO_ACCESS` pool copies, FFmpeg movie decode).
- [seedds/nxnative](https://github.com/seedds/nxnative) (MIT): the iOS/Mac
  original; the dirty-page tracking and several host HLE services originate
  there.
- [Sheepheerd/nxasahi](https://github.com/Sheepheerd/nxasahi) (MIT):
  Asahi-Linux work; its library-applet input fix, GPU counter timing, and
  duplicate-colour-slot handling are candidates to carry over.

Game data, keys, and anything extracted from a game never enter this
repository; the tool only accepts dumps the player brings themselves.
