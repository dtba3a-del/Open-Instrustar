// SPDX-License-Identifier: GPL-2.0-or-later

// set CTPU_VERSION to define a release
// if defined in this file it will tag the commit automatically
// via .git/hooks/post-commit (see below)
// the hook will then renamed from CTPU_VERSION to LAST_CTPU_VERSION
//


// next line shall define either CTPU_VERSION or LAST_CTPU_VERSION
//
#define CTPU_VERSION "0.3.16"


// do not edit below

// VERSION (git describe --tags --dirty) will be shown by OpenHantek
// if VERSION is not defined then use CTPU_VERSION
// if this is also not defined fall back to build date

#ifndef VERSION
#ifdef CTPU_VERSION
#define VERSION CTPU_VERSION
#else
#define VERSION __DATE__
#endif
#endif

/* content of ".git/hooks/post-commit":

#!/bin/bash

# this file is called automatically after a commit
# it tags the commit if a version is defined in the version file
# inspired by: https://coderwall.com/p/mk18zq/automatic-git-version-tagging-for-npm-modules

# this file was updated during development (by script build/MK_NEW_VER)
#
OPENHANTEK="$(git rev-parse --show-toplevel)"
CTPU_VERSION_H="$CTPU_REPO/ctpu/src/CTPU_VERSION.h"

# check if the last commit changed the entry CTPU_VERSION in file ...CTPU_VERSION.h and extract the new version
#
CTPU_VERSION=$(git diff HEAD^..HEAD -- ${CTPU_VERSION_H} | awk '/^\+#define CTPU_VERSION/ { print $3 }' | tr -d '"')

# if commit was marked as CTPU_VERSION then tag it accordingly and change entry to LAST_CTPU_VERSION
#
if [ "$CTPU_VERSION" != "" ]; then
    git tag -a $CTPU_VERSION -m "$(git log -1 --format=%s)"
    sed -i 's|^#define[[:blank:]]*CTPU_VERSION|#define LAST_CTPU_VERSION|g' $CTPU_VERSION_H
    echo "Created a new tag: $CTPU_VERSION"
fi

# update the build system with next build
touch $OPENHANTEK/CMakeLists.txt

*/
