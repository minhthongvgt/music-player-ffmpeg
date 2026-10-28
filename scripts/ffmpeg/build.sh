#!/usr/bin/env bash
set -euo pipefail

MEDIA3_VERSION="1.11.1"
FFMPEG_BRANCH="release/6.0"
FFMPEG_COMMIT="573f2b81ebd97da81def2ba762e81b0328547004"
NDK_VERSION="30.0.16248370"
ANDROID_API=26
DECODERS=(mp1 mp2 mp3 ac3 truehd mlp dca vorbis opus flac alac pcm_mulaw pcm_alaw
  mpeg1video mpeg2video mpeg4 h263 vp8 vp9 mjpeg)
ABIS=(armeabi-v7a arm64-v8a x86 x86_64)

NDK="${ANDROID_NDK_HOME:?Hãy đặt ANDROID_NDK_HOME trỏ tới Android NDK $NDK_VERSION}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT_DIR:-$ROOT/ffmpeg-decoder/src/main/jniLibs}"
WORK="${WORK_DIR:-$(mktemp -d)}"
case "$(uname -s)" in
  Linux) HOST=linux-x86_64 ;;
  Darwin) HOST=darwin-x86_64 ;;
  *) echo "Chỉ hỗ trợ Linux/macOS (trên Windows hãy dùng WSL hoặc GitHub Actions)"; exit 1 ;;
esac

NDK_FOUND="$(grep -m1 Pkg.Revision "$NDK/source.properties" | cut -d= -f2 | tr -d ' ')"
if [ "$NDK_FOUND" != "$NDK_VERSION" ]; then
  echo "LỖI: cần Android NDK $NDK_VERSION, đang dùng $NDK_FOUND ($NDK)"; exit 1
fi

echo "==> Thư mục làm việc: $WORK"

git clone --quiet --depth 1 --branch "$MEDIA3_VERSION" --filter=blob:none --sparse \
  https://github.com/androidx/media.git "$WORK/media"
git -C "$WORK/media" sparse-checkout set libraries/decoder_ffmpeg
MEDIA3_COMMIT="$(git -C "$WORK/media" rev-parse HEAD)"
MODULE="$WORK/media/libraries/decoder_ffmpeg/src/main"
JNI="$MODULE/jni"

cp "$ROOT/scripts/ffmpeg/ffmpeg_video_jni.cc" "$JNI/"
sed -i.bak \
  -e 's/ffmpeg_jni\.cc)/ffmpeg_jni.cc ffmpeg_video_jni.cc)/' \
  -e 's/PRIVATE android$/PRIVATE android EGL GLESv2/' \
  "$JNI/CMakeLists.txt"
grep -q "ffmpeg_video_jni.cc" "$JNI/CMakeLists.txt" || { echo "LỖI: không thêm được ffmpeg_video_jni.cc vào CMakeLists.txt"; exit 1; }
grep -q "PRIVATE android EGL GLESv2" "$JNI/CMakeLists.txt" || { echo "LỖI: không thêm được EGL/GLESv2 vào CMakeLists.txt"; exit 1; }

git init --quiet "$JNI/ffmpeg"
git -C "$JNI/ffmpeg" remote add origin https://github.com/FFmpeg/FFmpeg.git
git -C "$JNI/ffmpeg" fetch --quiet --depth 1 origin "$FFMPEG_COMMIT"
git -C "$JNI/ffmpeg" checkout --quiet --detach FETCH_HEAD
[ "$(git -C "$JNI/ffmpeg" rev-parse HEAD)" = "$FFMPEG_COMMIT" ] || { echo "LỖI: commit FFmpeg không khớp $FFMPEG_COMMIT"; exit 1; }

(cd "$JNI" && ./build_ffmpeg.sh "$MODULE" "$NDK" "$HOST" "$ANDROID_API" "${DECODERS[@]}")

for flag in CONFIG_GPL CONFIG_VERSION3 CONFIG_NONFREE; do
  if ! grep -q "#define $flag 0" "$JNI/ffmpeg/config.h"; then
    echo "LỖI: $flag đang bật, thư viện sẽ không còn là LGPL"; exit 1
  fi
done
LICENSE_LINE="$(cd "$JNI/ffmpeg" && grep -m1 'FFMPEG_LICENSE' config.h || true)"

rm -rf "$OUT"
for abi in "${ABIS[@]}"; do
  cmake -S "$JNI" -B "$WORK/build-$abi" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build "$WORK/build-$abi" > /dev/null
  mkdir -p "$OUT/$abi"
  "$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip" --strip-unneeded \
    -o "$OUT/$abi/libffmpegJNI.so" "$WORK/build-$abi/libffmpegJNI.so"
done

{
  echo "libffmpegJNI.so - build info"
  echo "Built:           $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Media3 (JNI):    https://github.com/androidx/media tag $MEDIA3_VERSION commit $MEDIA3_COMMIT (libraries/decoder_ffmpeg, Apache-2.0)"
  echo "Video JNI:       scripts/ffmpeg/ffmpeg_video_jni.cc (Apache-2.0)"
  echo "FFmpeg:          https://github.com/FFmpeg/FFmpeg branch $FFMPEG_BRANCH commit $FFMPEG_COMMIT (unmodified)"
  echo "FFmpeg source:   https://github.com/FFmpeg/FFmpeg/archive/$FFMPEG_COMMIT.tar.gz"
  echo "FFmpeg license:  ${LICENSE_LINE:-LGPL version 2.1 or later}"
  echo "NDK:             $NDK_FOUND"
  echo "Android API:     $ANDROID_API"
  echo "Decoders:        ${DECODERS[*]}"
  echo "Script:          scripts/ffmpeg/build.sh"
  echo
  for abi in "${ABIS[@]}"; do
    (cd "$OUT" && { sha256sum "$abi/libffmpegJNI.so" 2> /dev/null || shasum -a 256 "$abi/libffmpegJNI.so"; })
  done
} > "$OUT/BUILD_INFO.txt"

cat "$OUT/BUILD_INFO.txt"
