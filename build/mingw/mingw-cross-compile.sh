#!/bin/bash

# This script will download and setup a cross compilation environment
# for targetting Win32 from Linux. It can also be used to build on
# Windows under the MSYS/MinGW environment. It will use the GTK
# binaries from Tor Lillqvist.

TOR_URL="http://ftp.gnome.org/pub/gnome/binaries/win32";

TOR_BINARIES=( \
    glib/2.24/glib{-dev,}_2.24.0-2_win32.zip \
    gtk+/2.16/gtk+{-dev,}_2.16.6-2_win32.zip \
    pango/1.28/pango{-dev,}_1.28.0-1_win32.zip \
    atk/1.30/atk{-dev,}_1.30.0-1_win32.zip );

TOR_DEP_URL="http://ftp.gnome.org/pub/gnome/binaries/win32/dependencies";

TOR_DEPS=( \
    cairo{-dev,}_1.8.10-3_win32.zip \
    gettext-runtime-{dev-,}0.17-1.zip \
    fontconfig{-dev,}_2.8.0-2_win32.zip \
    freetype{-dev,}_2.3.12-1_win32.zip \
    expat_2.0.1-1_win32.zip \
    libpng{-dev,}_1.4.0-1_win32.zip \
    zlib{-dev,}_1.2.4-2_win32.zip );

GL_HEADER_URLS=( \
    http://cgit.freedesktop.org/mesa/mesa/plain/include/GL/gl.h \
    http://cgit.freedesktop.org/mesa/mesa/plain/include/GL/mesa_wgl.h \
    http://www.opengl.org/registry/api/glext.h );

GL_HEADERS=( gl.h mesa_wgl.h glext.h );

CLUTTER_GIT="git://git.clutter-project.org"

function download_file ()
{
    local url="$1"; shift;
    local filename="$1"; shift;

    case "$DOWNLOAD_PROG" in
	curl)
	    curl -C - -o "$DOWNLOAD_DIR/$filename" "$url";
	    ;;
	*)
	    $DOWNLOAD_PROG -O "$DOWNLOAD_DIR/$filename" -c "$url";
	    ;;
    esac;

    if [ $? -ne 0 ]; then
	echo "Downloading ${url} failed.";
	exit 1;
    fi;
}

function guess_dir ()
{
    local var="$1"; shift;
    local suffix="$1"; shift;
    local msg="$1"; shift;
    local prompt="$1"; shift;
    local dir="${!var}";

    if [ -z "$dir" ]; then
	echo "Please enter ${msg}.";
	dir="$PWD/$suffix";
	read -r -p "$prompt [$dir] ";
	if [ -n "$REPLY" ]; then
	    dir="$REPLY";
	fi;
    fi;

    eval $var="\"$dir\"";

    if [ ! -d "$dir" ]; then
	if ! mkdir -p "$dir"; then
	    echo "Error making directory $dir";
	    exit 1;
	fi;
    fi;
}

function y_or_n ()
{
    local prompt="$1"; shift;

    while true; do
	read -p "${prompt} [y/n] " -n 1;
	echo;
	case "$REPLY" in
	    y) return 0 ;;
	    n) return 1 ;;
	    *) echo "Please press y or n" ;;
	esac;
    done;
}

function do_unzip ()
{
    do_unzip_d "$ROOT_DIR" "$@";
}

function do_unzip_d ()
{
    local exdir="$1"; shift;
    local zipfile="$1"; shift;

    unzip -o -q -d "$exdir" "$zipfile" "$@";

    if [ "$?" -ne 0 ]; then
	echo "Failed to extract $zipfile";
	exit 1;
    fi;
}

function add_env ()
{
    echo "export $1=\"$2\"" >> $env_file;
}

function find_compiler ()
{
    local gccbin fullpath;

    if [ -z "$MINGW_TOOL_PREFIX" ]; then
	for gccbin in i{3,4,5,6}86-mingw32{,msvc}-gcc; do
	    fullpath="`which $gccbin 2>/dev/null`";
	    if [ "$?" -eq 0 ]; then
		MINGW_TOOL_PREFIX="${fullpath%%gcc}";
		break;
	    fi;
	done;
	if [ -z "$MINGW_TOOL_PREFIX" ]; then
	    echo;
	    echo "No suitable cross compiler was found.";
	    echo;
	    echo "If you already have a compiler installed,";
	    echo "please set the MINGW_TOOL_PREFIX variable";
	    echo "to point to its location without the";
	    echo "gcc suffix (eg: \"/usr/bin/i386-mingw32-\").";
	    echo;
	    echo "If you are using Ubuntu, you can install a";
	    echo "compiler by typing:";
	    echo;
	    echo " sudo apt-get install mingw32";
	    echo;
	    echo "Otherwise you can try following the instructions here:";
	    echo;
	    echo " http://www.libsdl.org/extras/win32/cross/README.txt";

	    exit 1;
	fi;
    fi;

    CC="${MINGW_TOOL_PREFIX}gcc";
    add_env ADDR2LINE "${MINGW_TOOL_PREFIX}addr2line"
    add_env AS "${MINGW_TOOL_PREFIX}as"
    add_env CC "${CC}"
    add_env CPP "${MINGW_TOOL_PREFIX}cpp"
    add_env CPPFILT "${MINGW_TOOL_PREFIX}c++filt"
    add_env CXX "${MINGW_TOOL_PREFIX}g++"
    add_env DLLTOOL "${MINGW_TOOL_PREFIX}dlltool"
    add_env DLLWRAP "${MINGW_TOOL_PREFIX}dllwrap"
    add_env GCOV "${MINGW_TOOL_PREFIX}gcov"
    add_env LD "${MINGW_TOOL_PREFIX}ld"
    add_env NM "${MINGW_TOOL_PREFIX}nm"
    add_env OBJCOPY "${MINGW_TOOL_PREFIX}objcopy"
    add_env OBJDUMP "${MINGW_TOOL_PREFIX}objdump"
    add_env READELF "${MINGW_TOOL_PREFIX}readelf"
    add_env SIZE "${MINGW_TOOL_PREFIX}size"
    add_env STRINGS "${MINGW_TOOL_PREFIX}strings"
    add_env WINDRES "${MINGW_TOOL_PREFIX}windres"
    add_env AR "${MINGW_TOOL_PREFIX}ar"
    add_env RANLIB "${MINGW_TOOL_PREFIX}ranlib"
    add_env STRIP "${MINGW_TOOL_PREFIX}strip"

    TARGET="${MINGW_TOOL_PREFIX##*/}";
    TARGET="${TARGET%%-}";

    echo "Using compiler $CC and target $TARGET";
}

# If a download directory hasn't been specified then try to guess one
# but ask for confirmation first
guess_dir DOWNLOAD_DIR "downloads" \
    "the directory to download to" "Download directory";

# Try to guess a download program if none has been specified
if [ -z "$DOWNLOAD_PROG" ]; then
    # If no download program has been specified then check if wget or
    # curl exists
    #wget first, because my curl can't download libsdl...
    for x in wget curl; do
	if [ "`type -t $x`" != "" ]; then
	    DOWNLOAD_PROG="$x";
	    break;
	fi;
    done;

    if [ -z "$DOWNLOAD_PROG" ]; then
	echo "No DOWNLOAD_PROG was set and neither wget nor curl is ";
	echo "available.";
	exit 1;
    fi;
fi;

# If a download directory hasn't been specified then try to guess one
# but ask for confirmation first
guess_dir ROOT_DIR "clutter-cross" \
    "the root prefix for the build environment" "Root dir";
SLASH_SCRIPT='s/\//\\\//g';
quoted_root_dir=`echo "$ROOT_DIR" | sed "$SLASH_SCRIPT" `;

##
# Download files
##

for bin in "${TOR_BINARIES[@]}"; do
    bn="${bin##*/}";
    download_file "$TOR_URL/$bin" "$bn"
done;

for dep in "${TOR_DEPS[@]}"; do
    download_file "$TOR_DEP_URL/$dep" "$dep";
done;

for dep in "${OTHER_DEPS[@]}"; do
    bn="${dep##*/}";
    download_file "$dep" "$bn";
done;

for dep in "${GL_HEADER_URLS[@]}"; do
    bn="${dep##*/}";
    download_file "$dep" "$bn";
done;

##
# Extract files
##

for bin in "${TOR_BINARIES[@]}"; do
    echo "Extracting $bin...";
    bn="${bin##*/}";
    do_unzip "$DOWNLOAD_DIR/$bn";
done;

for dep in "${TOR_DEPS[@]}"; do
    echo "Extracting $dep...";
    do_unzip "$DOWNLOAD_DIR/$dep";
done;

echo "Fixing pkgconfig files...";
for x in "$ROOT_DIR/lib/pkgconfig/"*.pc; do
    sed "s/^prefix=.*\$/prefix=${quoted_root_dir}/" \
	< "$x" > "$x.tmp";
    mv "$x.tmp" "$x";
done;

# The Pango FT pc file hardcodes the include path for freetype, so it
# needs to be fixed separately
sed -e 's/^Cflags:.*$/Cflags: -I${includedir}\/pango-1.0 -I${includedir}\/freetype2/' \
    -e 's/^\(Libs:.*\)$/\1 -lfreetype -lfontconfig/' \
    < "$ROOT_DIR/lib/pkgconfig/pangoft2.pc" \
    > "$ROOT_DIR/lib/pkgconfig/pangoft2.pc.tmp";
mv "$ROOT_DIR/lib/pkgconfig/pangoft2.pc"{.tmp,};

echo "Copying GL headers...";
if ! ( test -d "$ROOT_DIR/include/GL" || \
    mkdir "$ROOT_DIR/include/GL" ); then
    echo "Failed to create GL header directory";
    exit 1;
fi;
for header in "${GL_HEADERS[@]}"; do
    if ! cp "$DOWNLOAD_DIR/$header" "$ROOT_DIR/include/GL/"; then
        echo "Failed to copy $header";
        exit 1;
    fi;
done;

##
# Build
##

env_file="$ROOT_DIR/share/env.sh";
echo "Writing build environment script to $env_file";
echo "#!/bin/bash" > "$env_file";

find_compiler;

add_env PKG_CONFIG_PATH "$ROOT_DIR/lib/pkgconfig:\$PKG_CONFIG_PATH";

add_env LDFLAGS "-L$ROOT_DIR/lib -mno-cygwin \$LDFLAGS"
add_env CPPFLAGS "-I$ROOT_DIR/include \$CPPFLAGS"
add_env CFLAGS "-I$ROOT_DIR/include -mno-cygwin -mms-bitfields -march=i686 \${CFLAGS:-"-g"}"
add_env CXXFLAGS "-I$ROOT_DIR/include -mno-cygwin -mms-bitfields -march=i686 \${CFLAGS:-"-g"}"

cat >> "$env_file" <<EOF
ROOT_DIR="$ROOT_DIR";
TARGET="$TARGET";

function do_autogen()
{
  ./autogen.sh --prefix="\$ROOT_DIR" --host="\$TARGET" --target="\$TARGET" \\
    --with-flavour=win32;
}

# If any arguments are given then execute it as a program with the
# environment we set up

if test "\$#" -ge 1; then
    exec "\$@";
fi;

EOF

chmod a+x "$env_file";

if y_or_n "Do you want to checkout and build Clutter?"; then
    source "$env_file";

    guess_dir CLUTTER_BUILD_DIR "clutter" \
	"the build directory for clutter" "Build dir";
    git clone "$CLUTTER_GIT/clutter" $CLUTTER_BUILD_DIR;
    if [ "$?" -ne 0 ]; then
	echo "git failed";
	exit 1;
    fi;
    ( cd "$CLUTTER_BUILD_DIR" && do_autogen );
    if [ "$?" -ne 0 ]; then
	echo "autogen failed";
	exit 1;
    fi;
    ( cd "$CLUTTER_BUILD_DIR" && make all install );
    if [ "$?" -ne 0 ]; then
	echo "make failed";
	exit 1;
    fi;
fi;
