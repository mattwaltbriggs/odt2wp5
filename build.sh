#!/bin/bash
# Build the odt2wp5 milestone test driver.
set -e

CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"

# Two ways to satisfy librevenge:
#  (1) a local libwpd+librevenge build tree at ~/Developer/libwpd-build
#      (the original, vendored-sources build — universal binaries possible)
#  (2) the Homebrew librevenge package (brew install librevenge)
#      — dynamic link, arm64 only.
LIBWPD_BUILD="$HOME/Developer/libwpd-build"
USE_SYSTEM_REVENGE=0
if [ -d "$LIBWPD_BUILD" ]; then
	REV_INC="$LIBWPD_BUILD/librevenge/inc"
	REV_LIB="$LIBWPD_BUILD/librevenge/src/lib/.libs"
	REV_SRC="$LIBWPD_BUILD/librevenge/src/lib"
	BOOST_INC="${BOOST_INC:-/opt/homebrew/include}"   # header-only, compile-time only
	ARCHES="-arch arm64 -arch x86_64"                 # universal binaries
else
	REV_PREFIX="${REV_PREFIX:-$(brew --prefix librevenge)}"
	REV_INC="$REV_PREFIX/include/librevenge-0.0"
	REV_LIB="$REV_PREFIX/lib"
	ARCHES=""
	USE_SYSTEM_REVENGE=1
	echo "Using librevenge from Homebrew ($REV_PREFIX): arm64-only, dynamic link."
	echo "  (For universal binaries, install a local libwpd/librevenge build tree"
	echo "   at ~/Developer/libwpd-build.)"
fi

# Regenerate the extended-character reverse tables by inverting libwpd's own
# decoders (WP5 and WP6). This REQUIRES the local libwpd build tree (option 1);
# the produced .inc files are committed, so normally this step is skipped.
if [ "$1" = "regen" ] || [ ! -f WP5CharMap.inc ] || [ ! -f WP6CharMap.inc ]; then
	if [ "$USE_SYSTEM_REVENGE" = "1" ]; then
		echo "ERROR: cannot regenerate charmaps without $LIBWPD_BUILD and a libwpd build there." >&2
		echo "Both WP5CharMap.inc and WP6CharMap.inc are committed; restore them or" >&2
		echo "clone/librew the libwpd tree and pass 'regen'." >&2
		exit 1
	fi
	DYLD_DEC="$LIBWPD_BUILD/librevenge/src/lib/.libs:$LIBWPD_BUILD/libwpd/src/lib/.libs"
	if [ "$1" = "regen" ] || [ ! -f WP5CharMap.inc ]; then
		echo "Regenerating WP5CharMap.inc ..."
		"$CXX" -std=c++17 -g gen_charmap.cpp \
			-L"$LIBWPD_BUILD/libwpd/src/lib/.libs" -lwpd-0.10 \
			-L"$REV_LIB" -lrevenge-0.0 \
			-o gen_charmap
		DYLD_LIBRARY_PATH="$DYLD_DEC" ./gen_charmap
	fi
	if [ "$1" = "regen" ] || [ ! -f WP6CharMap.inc ]; then
		echo "Regenerating WP6CharMap.inc ..."
		"$CXX" -std=c++17 -g gen_wp6_charmap.cpp \
			-L"$LIBWPD_BUILD/libwpd/src/lib/.libs" -lwpd-0.10 \
			-L"$REV_LIB" -lrevenge-0.0 \
			-o gen_wp6_charmap
		DYLD_LIBRARY_PATH="$DYLD_DEC" ./gen_wp6_charmap
	fi
fi

# --- the generator test driver (hand-built event stream) ---
"$CXX" -std=c++17 -Wall -Wextra -g \
	-I"$REV_INC" \
	WP5Generator.cpp test_milestone1.cpp \
	-L"$REV_LIB" -lrevenge-0.0 \
	-o odt2wp5_test
echo "Built ./odt2wp5_test"

# --- the real CLI tools: odt2wp5 (WP5.1) and odt2wp6 (WP6) ---
# Goals: (1) UNIVERSAL binaries (arm64 + x86_64) when a local librevenge source
# tree is available; (2) link only macOS SYSTEM libraries (libxml2 + zlib),
# no Homebrew runtime dependency (except librevenge in the Homebrew fallback).
#
# With the local tree (option 1) we compile the handful of librevenge core
# sources we use directly with both arches. Those sources need Boost headers
# (spirit/algorithm/base64) — but only at COMPILE time: they are header-only,
# so the resulting binaries link no Boost. libxml2 comes from the SDK
# (-> /usr/lib/libxml2.2.dylib); there is no system libzip, so ODTReader.cpp
# reads the ODT zip with system zlib.
#
# We build TWO separate binaries. The shared, heavy sources (both generators +
# the ODT reader, plus the vendored librevenge core when applicable) are
# compiled to universal object files ONCE, then linked into each tool. Only the
# tiny CLI (odt2wp5.cpp) is compiled twice — once plain (defaults to WP5.1) and
# once with -DODT2WP6_DEFAULT (defaults to WP6) — so each binary's default
# format is baked in and survives renaming.
SDK="$(xcrun --show-sdk-path)"
INCS="-I$REV_INC -I$SDK/usr/include/libxml2"

OBJDIR="build/obj"
mkdir -p "$OBJDIR"
SHARED_SRCS=(WP5Generator.cpp WP6Generator.cpp ODTReader.cpp)
if [ "$USE_SYSTEM_REVENGE" = "0" ]; then
	SHARED_SRCS+=(
		"$REV_SRC/RVNGString.cpp" "$REV_SRC/RVNGProperty.cpp"
		"$REV_SRC/RVNGPropertyList.cpp" "$REV_SRC/RVNGPropertyListVector.cpp"
		"$REV_SRC/RVNGBinaryData.cpp" "$REV_SRC/RVNGMemoryStream.cpp"
	)
fi
OBJS=()
for src in "${SHARED_SRCS[@]}"; do
	obj="$OBJDIR/$(basename "${src%.cpp}").o"
	"$CXX" -std=c++17 -O2 $ARCHES $INCS -c "$src" -o "$obj"
	OBJS+=("$obj")
done

REVENGE_LINKS=()
if [ "$USE_SYSTEM_REVENGE" = "1" ]; then
	REVENGE_LINKS=(-L"$REV_LIB" -lrevenge-0.0)
fi

# Remove any prior outputs (esp. a legacy odt2wp6 symlink, so we don't link
# through it onto odt2wp5).
rm -f odt2wp5 odt2wp6

# odt2wp5: defaults to WP5.1
"$CXX" -std=c++17 -O2 $ARCHES $INCS \
	odt2wp5.cpp "${OBJS[@]}" "${REVENGE_LINKS[@]}" -lxml2 -lz -o odt2wp5
echo "Built ./odt2wp5 ($(lipo -archs odt2wp5)) — defaults to WP5.1"

# odt2wp6: defaults to WP6 (same sources, compile-time default flipped)
"$CXX" -std=c++17 -O2 $ARCHES $INCS -DODT2WP6_DEFAULT \
	odt2wp5.cpp "${OBJS[@]}" "${REVENGE_LINKS[@]}" -lxml2 -lz -o odt2wp6
echo "Built ./odt2wp6 ($(lipo -archs odt2wp6)) — defaults to WP6"
