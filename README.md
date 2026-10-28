# music-player-ffmpeg

FFmpeg (LGPL 2.1 or later) build used by the Android app "Music Player" by Nguyễn Minh Thông.
Bản build FFmpeg (LGPL 2.1 trở lên) dùng trong ứng dụng Android "Trình phát nhạc" của Nguyễn Minh Thông.

The app itself is proprietary. This repository publishes only the parts required by the LGPL:
Ứng dụng là phần mềm độc quyền. Kho này chỉ công bố các phần theo yêu cầu của LGPL:

- `scripts/ffmpeg/build.sh` — exact build script (FFmpeg commit, NDK version, configure options, decoder list).
- `scripts/ffmpeg/ffmpeg_video_jni.cc` — video JNI code linked into `libffmpegJNI.so` (Apache License 2.0, see `LICENSE`).
- `.github/workflows/build-ffmpeg.yml` — reproducible build on GitHub Actions.
- [Releases](https://github.com/minhthongvgt/music-player-ffmpeg/releases) — for every app version: `libffmpegJNI.zip` (the shipped binaries), `BUILD_INFO.txt` (SHA-256), the complete corresponding source of FFmpeg (`ffmpeg-<commit>.tar.gz`, unmodified), the Media3 FFmpeg JNI module with the build modifications (`media3-decoder_ffmpeg-<commit>.tar.gz`, Apache 2.0) and these scripts.

## Licenses / Giấy phép

- FFmpeg: GNU Lesser General Public License 2.1 or later (`COPYING.LGPLv2.1`). Built without `--enable-gpl`, `--enable-version3`, `--enable-nonfree`; the build fails if any of them is enabled. FFmpeg is a trademark of Fabrice Bellard; this project is not affiliated with FFmpeg.
- Media3 FFmpeg JNI (`ffmpeg_jni.cc`, `CMakeLists.txt`, `build_ffmpeg.sh` from androidx/media): Apache License 2.0.
- `ffmpeg_video_jni.cc` and the scripts in this repository: Apache License 2.0 (`LICENSE`).

## Replacing the library / Thay thế thư viện

FFmpeg is statically linked into `libffmpegJNI.so`, which the app loads dynamically through JNI (`System.loadLibrary("ffmpegJNI")`). To use a modified FFmpeg: edit `build.sh` or the FFmpeg source, build with `ANDROID_NDK_HOME=<NDK> OUT_DIR=<dir> bash scripts/ffmpeg/build.sh` (Linux/macOS) or run the workflow, then replace `lib/<abi>/libffmpegJNI.so` in the app package and re-sign it. The JNI interface is defined by the sources above.

FFmpeg được liên kết tĩnh trong `libffmpegJNI.so`, ứng dụng nạp thư viện này động qua JNI. Để dùng FFmpeg đã sửa: sửa `build.sh` hoặc mã FFmpeg, build bằng lệnh trên hoặc chạy workflow, rồi thay `lib/<abi>/libffmpegJNI.so` trong gói ứng dụng và ký lại.

## Legal pages

- Terms of use / Điều khoản sử dụng: https://minhthongvgt.github.io/music-player-ffmpeg/terms.html
- Privacy policy / Chính sách quyền riêng tư: https://minhthongvgt.github.io/music-player-ffmpeg/privacy.html

Contact / Liên hệ: https://github.com/minhthongvgt/music-player-ffmpeg/issues
