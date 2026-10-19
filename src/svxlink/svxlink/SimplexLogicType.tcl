###############################################################################
#
# SimplexLogic event handlers
#
# This file is sourced by all simplex logics and contains the TCL code that are
# in common to all simplex logics.
#
###############################################################################

namespace eval ${::logic_name} {

# Mix in ("inherit") generic logic TCL code
sourceTclWithOverrides "Logic.tcl"
mixin Logic

# end of namespace
}

# The embedded interpreter does not run Tcl's after/fileevent queue.
if {[info commands ::AIORS::ensure_hooks] ne ""} {
  ::AIORS::ensure_hooks
}

#
# This file has not been truncated
#
