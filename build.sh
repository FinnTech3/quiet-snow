#!/bin/sh
# Builds build/QuietSnow.app (universal, ad-hoc signed) and build/QuietSnow.zip. Needs Xcode or the command line tools.
set -eu
cd "$(dirname "$0")"
swift build -c release --arch arm64 --arch x86_64
app=build/QuietSnow.app
rm -rf build
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp .build/apple/Products/Release/QuietSnow "$app/Contents/MacOS/QuietSnow"
cp Info.plist "$app/Contents/Info.plist"
if [ -f assets/icon-1024.png ]; then
    set=build/AppIcon.iconset
    mkdir -p "$set"
    for s in 16 32 128 256 512; do
        sips -z $s $s assets/icon-1024.png --out "$set/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) assets/icon-1024.png --out "$set/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$app/Contents/Resources/AppIcon.icns"
    rm -rf "$set"
fi
codesign --force --sign - "$app"
ditto -c -k --keepParent "$app" build/QuietSnow.zip
echo "built $app and build/QuietSnow.zip"
