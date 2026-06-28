# Copyright 2006 Milan Digital Audio LLC
# Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
# License GPL-2.0 or later
# (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

include(BundleUtilities)

function(verify_app)
endfunction()

set(APPLE  )
set(UNIX   )
set(CYGWIN )
set(WIN32  )
include("${statusfile}")
set(BU_CHMOD_BUNDLE_ITEMS ON)

# IGNORE_ITEM libdb-5.3.dylib: a stale, now-unresolvable reference to the
# deprecated "berkeley-db@5" Homebrew formula, baked into the load commands
# of some other linked library on the macos-15-intel CI runner (not
# reproducible on macos-14/arm64, where the cached Homebrew state differs).
# This is not a dependency GrandOrgue itself uses - safe to skip bundling
# it rather than fail the whole build trying to resolve it.
fixup_bundle("${bundledtarget}"  ""  "${searchdirs}" IGNORE_ITEM "libdb-5.3.dylib")
