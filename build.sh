#!/bin/bash
# Build the odt2wp5 milestone test driver.
set -e

CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"

# Three ways to satisfy librevenge:
#  (1) a local libwpd+librevenge build tree at ~/Developer/libwpd-build
#      (the original vendored-sources build — universal binaries, no downloads)
#  (2) fetch librevenge sources into build/vendor/ and compile the handful of
#      core sources we use (same as (1) but needs one download; also universal)
#  (3) the Homebrew librevenge package (brew install librevenge)
#      — dynamic link, arm64 only. Last resort if (2) cannot download.
#
# Modes (1) and (2) produce SELF-CONTAINED binaries: librevenge is compiled
# in-tree, linked statically, and the binaries depend only on macOS system
# libraries (libxml2 + zlib). Boost is needed only at COMPILE time (headers).
LIBWPD_BUILD="$HOME/Developer/libwpd-build"
VEND_REVENGE=0          # 1 = compile librevenge core sources in-tree (static)
if [ -d "$LIBWPD_BUILD" ]; then
	REV_INC="$LIBWPD_BUILD/librevenge/inc"
	REV_LIB="$LIBWPD_BUILD/librevenge/src/lib/.libs"
	REV_SRC="$LIBWPD_BUILD/librevenge/src/lib"
	BOOST_INC="${BOOST_INC:-/opt/homebrew/include}"   # header-only, compile-time only
	ARCHES="-arch arm64 -arch x86_64"                 # universal binaries
	VEND_REVENGE=1
else
	REV_VER="${REV_VERSION:-0.0.6}"
	VEN_DIR="build/vendor/librevenge-$REV_VER"
	VEN_SRC="$VEN_DIR/src/lib"
	VEN_INC="$VEN_DIR/inc"
	if [ ! -f "$VEN_SRC/RVNGString.cpp" ]; then
		echo "Fetching librevenge $REV_VER sources into $VEN_DIR ..." >&2
		mkdir -p build/vendor
		TARBALL="build/vendor/librevenge-$REV_VER.tar.xz"
		URL="https://downloads.sourceforge.net/project/libwpd/librevenge/librevenge-$REV_VER/librevenge-$REV_VER.tar.xz"
		if curl -fsSL --retry 3 -o "$TARBALL" "$URL" \
			&& rm -rf "$VEN_DIR" \
			&& tar -C build/vendor -xf "$TARBALL" \
			&& rm -f "$TARBALL"; then
			:
		else
			echo "WARNING: could not fetch librevenge sources; falling back to" >&2
			echo "Homebrew librevenge (arm64 only, dynamic link)." >&2
		fi
	fi
	if [ -f "$VEN_SRC/RVNGString.cpp" ]; then
		REV_INC="$VEN_INC"; REV_LIB=""; REV_SRC="$VEN_SRC"
		BOOST_INC="${BOOST_INC:-/opt/homebrew/include}"  # header-only, compile-time only
		ARCHES="-arch arm64 -arch x86_64"                # universal binaries
		VEND_REVENGE=1
	else
		REV_PREFIX="${REV_PREFIX:-$(brew --prefix librevenge)}"
		REV_INC="$REV_PREFIX/include/librevenge-0.0"
		REV_LIB="$REV_PREFIX/lib"
		ARCHES=""
	fi
fi

# Regenerate the extended-character reverse tables by inverting libwpd's own
# decoders (WP5 and WP6). This REQUIRES the local libwpd build tree (option 1);
# the produced .inc files are committed, so normally this step is skipped.
if [ "$1" = "regen" ] || [ ! -f WP5CharMap.inc ] || [ ! -f WP6CharMap.inc ]; then
	if [ ! -d "$LIBWPD_BUILD" ]; then
		echo "ERROR: cannot regenerate charmaps without $LIBWPD_BUILD and a libwpd" >&2
		echo "build there." >&2
		echo "Both WP5CharMap.inc and WP6CharMap.inc are committed; restore them or" >&2
		echo "build the libwpd tree and pass 'regen'." >&2
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

SDK="$(xcrun --show-sdk-path)"
INCS="-I$REV_INC -I$SDK/usr/include/libxml2"
if [ "$VEND_REVENGE" = "1" ]; then
	BOOST_INC="${BOOST_INC:-/opt/homebrew/include}"   # header-only, compile-time only
	INCS="-I$REV_INC -I$REV_SRC -I$BOOST_INC -I$SDK/usr/include/libxml2"
fi

OBJDIR="build/obj"
mkdir -p "$OBJDIR"
SHARED_SRCS=(WP5Generator.cpp WP6Generator.cpp ODTReader.cpp)
if [ "$VEND_REVENGE" = "1" ]; then
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
if [ "$VEND_REVENGE" = "0" ]; then
	REVENGE_LINKS=(-L"$REV_LIB" -lrevenge-0.0)
fi

# --- the generator test driver (hand-built event stream) ---
if [ "$VEND_REVENGE" = "1" ]; then
	# Reuse the universal WP5Generator.o and the vendored librevenge core
	# objects; no -lrevenge needed (they define everything).
	DRV_OBJS=("$OBJDIR/WP5Generator.o"
		"$OBJDIR/RVNGString.o" "$OBJDIR/RVNGProperty.o"
		"$OBJDIR/RVNGPropertyList.o" "$OBJDIR/RVNGPropertyListVector.o"
		"$OBJDIR/RVNGBinaryData.o" "$OBJDIR/RVNGMemoryStream.o")
	DRV_LINKS=()
else
	DRV_OBJS=()
	DRV_LINKS=(-L"$REV_LIB" -lrevenge-0.0)
fi
# DRV_OBJS = universal WP5Generator.o followed by vendored librevenge core
# objects: order fine for the linker, no -lrevenge needed.
"$CXX" -std=c++17 -Wall -Wextra -g \
	$INCS \
	${DRV_OBJS[@]+"${DRV_OBJS[@]}"} "${DRV_LINKS[@]}" \
	test_milestone1.cpp -o odt2wp5_test
unset DRV_OBJS DRV_LINKS
echo "Built ./odt2wp5_test"

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
