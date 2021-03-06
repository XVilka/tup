#! /bin/sh -e

# This is a helper script to generate tup-version.o at the same time as
# linking, so that the version is updated whenever we change anything that
# affects the tup binary. This used to live in the Tupfile, but to support
# Windows local builds we need to make it an explicit shell script.
CC=$1
CFLAGS=$2
LDFLAGS=$3
output=$4
files=$5
version=$6
if [ "$version" = "" ]; then
	# If we don't pass in a version, try to get one from git
	version=`git describe 2>/dev/null || true`
	if [ "$version" = "" ]; then
		# If we aren't using git, try to get one from the pathname (eg:
		# for a tarball release
		version=`echo "$PWD" | sed 's/.*\/tup-//'`
		if [ "$version" = "" ]; then
			# No other version source, use "unknown"
			version="unknown"
		fi
	fi
fi
(echo "#include \"tup/version.h\""; echo "const char tup_version[] = \"$version\";") | $CC -x c -c - -o tup-version.o $CFLAGS
$CC $files tup-version.o -o $output $LDFLAGS
