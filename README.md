mpv
===

Overview
--------

**mpv** is a media player based on MPlayer and mplayer2. It supports a wide
variety of video file formats, audio and video codecs, and subtitle types.

If you are wondering what's different from mplayer2 and MPlayer you can read
more about the [changes][changes].

Compilation
-----------

Compiling with full features requires development files for several
external libraries. Below is a list of some important requirements.

The mpv build system uses *waf* but we don't store it in your source tree. The
script './bootstrap.py' will download the latest version of waf that was tested
with the build system.

For a list of the available build options use `./waf configure --help`. If
you think you have support for some feature installed but configure fails to
detect it, the file `build/config.log` may contain information about the
reasons for the failure.

NOTE: To avoid cluttering the output with unreadable spam, `--help` only shows
one of the two switches for each option. If the option is autodetected by
default, the `--disable-***` switch is printed; if the option is disabled by
default, the `--enable-***` switch is printed. Either way, you can use
`--enable-***` or `--disable-**` regardless of what is printed by `--help`.

To build the software you can use `./waf build`: the result of the compilation
will be located in `build/mpv`. You can use `./waf install` to install mpv
to the *prefix* after it is compiled.

NOTE: Using the old build system (with `./old-configure`) should still work,
but will be removed in a future version of mpv.

Essential dependencies (incomplete list):

- gcc or clang
- X development headers (xlib, X extensions, libvdpau, libGL, libXv, ...)
- Audio output development headers (libasound/ALSA, pulseaudio)
- FFmpeg libraries (libavutil libavcodec libavformat libswscale libavfilter
  and either libswresample or libavresample)
  At least FFmpeg 2.1.4 or Libav 10 is required.
- zlib
- iconv (normally provided by the system libc)
- libass
- Lua (optional, required for the OSC pseudo-GUI and youtube-dl integration)
- libjpeg (optional, used for screenshots only)
- Enca (optional, for subtitle charset detection)
- vdpau and vaapi libraries for hardware decoding on Linux (optional)

Libass dependencies:

- gcc or clang, yasm on x86 and x86_64
- fribidi, freetype, fontconfig development headers (for libass)
- harfbuzz (optional, required for correct rendering of combining characters,
  particularly for correct rendering of non-English text on OSX, and
  Arabic/Indic scripts on any platform)

FFmpeg dependencies:

- gcc or clang, yasm on x86 and x86_64
- OpenSSL (has to be explicitly enabled when compiling ffmpeg)
- libx264/libmp3lame/libfdk-aac if you want to use encoding (has to be
  explicitly enabled when compiling ffmpeg)
- Libav also works, but some features will not work. (See section below.)

Most of the above libraries are available in suitable versions on normal
Linux distributions. However FFmpeg is an exception (distro versions may be
too old to work at all or work well). For that reason you may want to use
the separately available build wrapper ([mpv-build][mpv-build]) that first compiles FFmpeg
libraries and libass, and then compiles the player statically linked against
those.

If you are running Mac OSX and using homebrew we provide [homebrew-mpv][homebrew-mpv], an up
to date formula that compiles mpv with sensible dependencies and defaults for
OSX.

If you want to build a Windows binary, you either have to use MSYS2 and MinGW,
or cross-compile from Linux with MinGW. See [Windows compilation][windows_compilation].

FFmpeg vs. Libav
----------------

Generally, mpv should work with the latest release as well as the git version
of both FFmpeg and Libav. But FFmpeg is preferred, and some mpv features work
with FFmpeg only. See the [wiki article][ffmpeg_vs_libav] about the issue.


Release cycle
-------------

Every few months, a new release is cut off of the master branch and is assigned
a 0.X.0 version number.

As part of the maintenance process, minor releases are made, which are assigned
0.X.Y version numbers. Minor releases contain bug fixes only. They never merge
the master branch, and no features are added to it. Only the latest release is
maintained.

The goal of releases is to provide stability and an unchanged base for the sake
of Linux distributions. If you want the newest features, just use the master
branch, which is stable most of the time, except sometimes, when it's not.

Releases other than the latest release are unsupported and unmaintained.

See the [release policy document][release-policy] for more information.

Bug reports
-----------

Please use the [issue tracker][issue-tracker] provided by GitHub to send us bug
reports or feature requests.

Contributing
------------

For small changes you can just send us pull requests through GitHub. For bigger
changes come and talk to us on IRC before you start working on them. It will
make code review easier for both parties later on.

Contact
-------

Most activity happens on the IRC channel and the github issue tracker. The
mailing lists are mostly unused.

 - **Github issue tracker**: [issue tracker][issue-tracker] (report bugs here)
 - **User IRC Channel**: `#mpv-player` on `irc.freenode.net`
 - **Developer IRC Channel**: `#mpv-player-dev` on `irc.freenode.net`
 - **Users Mailing List**: `mpv-users@googlegroups.com` ([Archive / Subscribe][mpv-users]).
 - **Devel Mailing List**: `mpv-devel@googlegroups.com` ([Archive / Subscribe][mpv-devel])

To contact the `mpv` team in private write to `mpv-team@googlegroups.com`. Use
only if discretion is required.

[changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/man/changes.rst
[mpv-build]: https://github.com/mpv-player/mpv-build
[homebrew-mpv]: https://github.com/mpv-player/homebrew-mpv
[issue-tracker]:  https://github.com/mpv-player/mpv/issues
[mpv-users]: https://groups.google.com/forum/?hl=en#!forum/mpv-users
[mpv-devel]: https://groups.google.com/forum/?hl=en#!forum/mpv-devel
[ffmpeg_vs_libav]: https://github.com/mpv-player/mpv/wiki/FFmpeg-versus-Libav
[release-policy]: https://github.com/mpv-player/mpv/blob/master/DOCS/release-policy.md
[windows_compilation]: https://github.com/mpv-player/mpv/blob/master/DOCS/compile-windows.md
