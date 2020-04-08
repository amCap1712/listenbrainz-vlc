#!/bin/sh

set -e
set -x

info()
{
    local green="\033[1;32m"
    local normal="\033[0m"
    echo "[${green}build${normal}] $1"
}

usage()
{
cat << EOF
usage: $0 [options]

Build vlc in the current directory

OPTIONS:
   -h            Show some help
   -r            Release mode (default is debug)
   -a <arch>     Use the specified arch (default: x86_64, possible i686, aarch64)
   -p            Use a Prebuilt contrib package (speeds up compilation)
   -c            Create a Prebuilt contrib package (rarely used)
   -l            Enable translations (can be slow)
   -i <n|r|u|m>  Create an Installer (n: nightly, r: release, u: unsigned release archive, m: msi only)
   -s            Interactive shell (get correct environment variables for build)
   -b <url>      Enable breakpad support and send crash reports to this URL
   -d            Create PDB files during the build
   -x            Add extra checks when compiling
EOF
}

ARCH="x86_64"
while getopts "hra:pcli:sb:dx" OPTION
do
     case $OPTION in
         h)
             usage
             exit 1
         ;;
         r)
             RELEASE="yes"
             INSTALLER="r"
         ;;
         a)
             ARCH=$OPTARG
         ;;
         p)
             PREBUILT="yes"
         ;;
         c)
             PACKAGE="yes"
         ;;
         l)
             I18N="yes"
         ;;
         i)
             INSTALLER=$OPTARG
         ;;
         s)
             INTERACTIVE="yes"
         ;;
         b)
             BREAKPAD=$OPTARG
         ;;
         d)
             WITH_PDB="yes"
         ;;
         x)
             EXTRA_CHECKS="yes"
         ;;
     esac
done
shift $(($OPTIND - 1))

if [ "x$1" != "x" ]; then
    usage
    exit 1
fi

case $ARCH in
    x86_64)
        SHORTARCH="win64"
        ;;
    i686)
        SHORTARCH="win32"
        ;;
    aarch64)
        SHORTARCH="winarm64"
        ;;
    *)
        usage
        exit 1
esac

#####

SCRIPT_PATH="$( cd "$(dirname "$0")" ; pwd -P )"

: ${JOBS:=$(getconf _NPROCESSORS_ONLN 2>&1)}
TRIPLET=$ARCH-w64-mingw32

# Check if compiling with clang
CC=${CC:-$TRIPLET-gcc}
if ! printf "#ifdef __clang__\n#error CLANG\n#endif" | $CC -E -; then
    COMPILING_WITH_CLANG=1
else
    COMPILING_WITH_CLANG=0
fi

info "Building extra tools"
mkdir -p extras/tools
cd extras/tools
export VLC_TOOLS="$PWD/build"

export PATH="$PWD/build/bin":"$PATH"
# Force patched meson as newer versions don't add -lpthread properly in libplacebo.pc
FORCED_TOOLS="meson"
# Force libtool build when compiling with clang
if [ "$COMPILING_WITH_CLANG" -gt 0 ] && [ ! -d "libtool" ]; then
    FORCED_TOOLS="$FORCED_TOOLS libtool"
fi
# bootstrap only if needed in interactive mode
if [ "$INTERACTIVE" != "yes" ] || [ ! -f ./Makefile ]; then
    NEEDED="$FORCED_TOOLS" ${SCRIPT_PATH}/../../tools/bootstrap
fi
make -j$JOBS

# avoid installing wine on WSL
# wine is needed to build Qt with shaders
if test -z "`command -v wine`"
then
    if test -n "`command -v wsl.exe`"
    then
        echo "Using wsl.exe to replace wine"
        echo "#!/bin/sh" > build/bin/wine
        echo "wsl.exe \"\$@\"" >> build/bin/wine
    fi
fi

cd ../../

export USE_FFMPEG=1
export PATH="$PWD/contrib/$TRIPLET/bin":"$PATH"

if [ "$INTERACTIVE" = "yes" ]; then
if [ "x$SHELL" != "x" ]; then
    exec $SHELL
else
    exec /bin/sh
fi
fi

info "Building contribs"
echo $PATH

mkdir -p contrib/contrib-$SHORTARCH && cd contrib/contrib-$SHORTARCH
if [ ! -z "$WITH_PDB" ]; then
    CONTRIBFLAGS="$CONTRIBFLAGS --enable-pdb"
fi
if [ ! -z "$BREAKPAD" ]; then
     CONTRIBFLAGS="$CONTRIBFLAGS --enable-breakpad"
fi
if [ "$RELEASE" != "yes" ]; then
     CONTRIBFLAGS="$CONTRIBFLAGS --disable-optim"
fi
${SCRIPT_PATH}/../../../contrib/bootstrap --host=$TRIPLET $CONTRIBFLAGS

# Rebuild the contribs or use the prebuilt ones
if [ "$PREBUILT" != "yes" ]; then
    make list
    make -j$JOBS fetch
    make -j$JOBS -k || make -j1
    if [ "$PACKAGE" = "yes" ]; then
        make package
    fi
elif [ -n "$VLC_PREBUILT_CONTRIBS_URL" ]; then
    make prebuilt PREBUILT_URL="$VLC_PREBUILT_CONTRIBS_URL"
    make .luac
else
    make prebuilt
    make .luac
fi
cd ../..

info "Bootstrapping"

if ! [ -e ${SCRIPT_PATH}/../../../configure ]; then
    echo "Bootstraping vlc"
    ${SCRIPT_PATH}/../../../bootstrap
fi

info "Configuring VLC"
if [ -z "$PKG_CONFIG" ]; then
    if [ `unset PKG_CONFIG_LIBDIR; $TRIPLET-pkg-config --version 1>/dev/null 2>/dev/null || echo FAIL` = "FAIL" ]; then
        # $TRIPLET-pkg-config DOESNT WORK
        # on Debian it pretends it works to autoconf
        export PKG_CONFIG="pkg-config"
        if [ -z "$PKG_CONFIG_LIBDIR" ]; then
            export PKG_CONFIG_LIBDIR="/usr/$TRIPLET/lib/pkgconfig:/usr/lib/$TRIPLET/pkgconfig"
        else
            export PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR:/usr/$TRIPLET/lib/pkgconfig:/usr/lib/$TRIPLET/pkgconfig"
        fi
    else
        # $TRIPLET-pkg-config WORKs
        export PKG_CONFIG="pkg-config"
    fi
fi

mkdir -p $SHORTARCH
cd $SHORTARCH

CONFIGFLAGS=""
if [ "$RELEASE" != "yes" ]; then
     CONFIGFLAGS="$CONFIGFLAGS --enable-debug"
else
     CONFIGFLAGS="$CONFIGFLAGS --disable-debug"
fi
if [ "$I18N" != "yes" ]; then
     CONFIGFLAGS="$CONFIGFLAGS --disable-nls"
fi
if [ ! -z "$BREAKPAD" ]; then
     CONFIGFLAGS="$CONFIGFLAGS --with-breakpad=$BREAKPAD"
fi
if [ ! -z "$WITH_PDB" ]; then
    CONFIGFLAGS="$CONFIGFLAGS --enable-pdb"
fi
if [ ! -z "$EXTRA_CHECKS" ]; then
    CFLAGS="$CFLAGS -Werror=incompatible-pointer-types -Werror=missing-field-initializers"
    CXXFLAGS="$CXXFLAGS -Werror=missing-field-initializers"
fi

${SCRIPT_PATH}/configure.sh --host=$TRIPLET --with-contrib=../contrib/$TRIPLET $CONFIGFLAGS

info "Compiling"
make -j$JOBS

if [ "$INSTALLER" = "n" ]; then
make package-win32-debug package-win32 package-msi
elif [ "$INSTALLER" = "r" ]; then
make package-win32
elif [ "$INSTALLER" = "u" ]; then
make package-win32-release
sha512sum vlc-*-release.7z
elif [ "$INSTALLER" = "m" ]; then
make package-msi
fi
