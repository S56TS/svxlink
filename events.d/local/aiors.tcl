# /usr/share/svxlink/events.d/local/aiors.tcl
# AIORS integration: LED hooks + optional courtesy beep override
#
# Notes:
# - With your C++ patch, RepeaterLogic now always dispatches Tcl "squelch_open rx_id is_open".
# - This script wraps the appropriate squelch_open + transmit procs and calls aiorsctl.
# - Script is idempotent: safe if sourced twice (won't rewrap / spam / break procs).

namespace eval ::AIORS {
  # ---- knobs ----
  variable debug 0
  variable cfg_logged 0

  # Courtesy beep behavior (optional)
  variable startup_suppress_ms 3000
  variable beep_debounce_ms 800
  variable beep_close_window_ms 1500

  # Config: disable repeater close / roger beep (read from svxlink.conf)
  variable disable_rpt_close_beep 0

  # Hook state
  variable hooked_rx 0
  variable hooked_tx 0
  variable hooked_rgr 0
  variable rx_hook_retries 0
  variable rehook_scheduled 0

  # Beep state
  variable start_ts [clock milliseconds]
  variable last_valid_close_ts 0
  variable last_beep_ts 0

  # Namespace discovery
  variable logic_name "RepeaterLogic"
  variable inst_ns ""
  variable logic_ns ""

  # aiorsctl path (resolved after _log/_warn are defined)
  variable aiorsctl ""
}

proc ::AIORS::_ini_get {path section key def} {
  if {![file readable $path]} { return $def }

  set in_section 0
  set f [open $path r]
  while {[gets $f line] >= 0} {
    # strip CR/LF and trim
    regsub -all "\r" $line "" line
    set line [string trim $line]

    # skip blanks and comments
    if {$line eq ""} continue
    if {[string match "#*" $line] || [string match ";*" $line]} continue

    # section header?
    if {[regexp {^\[(.+)\]$} $line -> sec]} {
      set in_section [expr {$sec eq $section}]
      continue
    }

    if {!$in_section} continue

    # key=value
    if {[regexp {^([^=]+)=(.*)$} $line -> k v]} {
      set k [string trim $k]
      set v [string trim $v]
      if {$k eq $key} {
        close $f
        return $v
      }
    }
  }
  close $f
  return $def
}

proc ::AIORS::_cfg_get {key def} {
  if {[info commands Cfg::getValue] eq ""} {
    return $def
  }

  # Try "current section" signature first: Cfg::getValue KEY DEFAULT
  if {![catch {set v [Cfg::getValue $key $def]}]} {
    return $v
  }

  # Fallback: some builds use Cfg::getValue SECTION KEY DEFAULT
  # (we try RepeaterLogic as section)
  if {![catch {set v [Cfg::getValue "RepeaterLogic" $key $def]}]} {
    return $v
  }

  return $def
}

proc ::AIORS::_log {msg} {
  # Use SvxLink logging helper if available
  if {[info commands printInfo] ne ""} {
    printInfo $msg
  } else {
    puts $msg
  }
}

proc ::AIORS::_dbg {msg} {
  if {$::AIORS::debug} { ::AIORS::_log "AIORS DBG: $msg" }
}

proc ::AIORS::_warn {msg} {
  # Match SvxLink warning style as you were using before
  puts [format {*** WARNING[RepeaterLogic]: %s} $msg]
}

::AIORS::_log "AIORS: local/aiors.tcl loaded"

# Resolve aiorsctl path safely (don't crash if unset)
if {![info exists ::AIORS::aiorsctl] || $::AIORS::aiorsctl eq ""} {
  if {[file executable "/usr/local/bin/aiorsctl"]} {
    set ::AIORS::aiorsctl "/usr/local/bin/aiorsctl"
  } elseif {[file executable "/usr/bin/aiorsctl"]} {
    set ::AIORS::aiorsctl "/usr/bin/aiorsctl"
  } else {
    set ::AIORS::aiorsctl [auto_execok aiorsctl]
  }
}

if {$::AIORS::aiorsctl eq ""} {
  ::AIORS::_warn "AIORS: aiorsctl not found; LED hooks disabled"
} else {
  ::AIORS::_log "AIORS: aiorsctl=$::AIORS::aiorsctl"
}

proc ::AIORS::_refresh_namespaces {} {
  # SvxLink exports ::logic_name (e.g. "RepeaterLogic", "SimplexLogic")
  set ln "RepeaterLogic"
  if {[info exists ::logic_name] && $::logic_name ne ""} { set ln $::logic_name }

  set ::AIORS::logic_name $ln
  set ::AIORS::inst_ns  "::${ln}"
  set ::AIORS::logic_ns "::${ln}::Logic"
}

# ---- LED mapping helpers ----
proc ::AIORS::_led_ch {id} {
  # Map rx_id/tx_id to channel a/b.
  # Your svxlink.conf: RX_ID=0 means "0".
  # Keep it simple: 0->a, 1->b, otherwise default a.
  if {$id eq ""} { return "a" }
  if {[string is integer -strict $id]} {
    return [expr {$id == 0 ? "a" : "b"}]
  }
  # If someone passes "Rx1"/"Tx2", take trailing digit
  if {[regexp {([0-9]+)$} $id -> n]} {
    return [expr {$n == 0 ? "a" : "b"}]
  }
  return "a"
}

proc ::AIORS::led_rx {rx_id is_open} {
  set ch [::AIORS::_led_ch $rx_id]
  set state [expr {$is_open ? "on" : "off"}]

  if {$::AIORS::debug} {
    ::AIORS::_log "AIORS: led_rx id='$rx_id' is_open=$is_open -> ch=$ch"
    ::AIORS::_dbg "led_rx rx_id=$rx_id state=$state"
  }

  # Call aiorsctl (ignore failures but log when debug)
  if {[catch {exec $::AIORS::aiorsctl set led $ch rx $state} e]} {
    if {$::AIORS::debug} { ::AIORS::_warn "AIORS: aiorsctl led_rx failed: $e" }
  }
}

proc ::AIORS::led_tx {tx_id is_on} {
  set ch [::AIORS::_led_ch $tx_id]
  set state [expr {$is_on ? "on" : "off"}]

  if {$::AIORS::debug} {
    ::AIORS::_dbg "led_tx tx_id=$tx_id state=$state ch=$ch"
  }

  if {[catch {exec $::AIORS::aiorsctl set led $ch tx $state} e]} {
    if {$::AIORS::debug} { ::AIORS::_warn "AIORS: aiorsctl led_tx failed: $e" }
  }
}

# ---- wrapper helper (proc-only; used for transmit) ----
proc ::AIORS::_wrap_proc {procname wrapper_body} {
  if {[info procs $procname] eq ""} { return 0 }
  set orig "${procname}__aiors_orig"
  if {[info procs $orig] ne ""} { return 1 }
  rename $procname $orig
  set body [string map [list "@ORIG@" $orig] $wrapper_body]
  proc $procname args $body
  return 1
}

# ---- TX hook ----
proc ::AIORS::_try_install_tx_hook {} {
  if {$::AIORS::hooked_tx} { return 1 }

  ::AIORS::_refresh_namespaces

  # transmit signatures vary; wrapper below handles 1-arg and 2-arg
  foreach name [list \
      "${::AIORS::logic_ns}::transmit" \
      "${::AIORS::inst_ns}::transmit" \
      "${::AIORS::logic_name}::transmit" \
    ] {

    if {[info procs $name] eq ""} { continue }

    if {[::AIORS::_wrap_proc $name {
      # Keep hooks in place if anything overrides later
      catch { ::AIORS::ensure_hooks }

      if {[llength $args] == 1} {
        set tx_id "0"
        set is_on [lindex $args 0]
      } elseif {[llength $args] == 2} {
        set tx_id [lindex $args 0]
        set is_on [lindex $args 1]
      } else {
        return [uplevel 1 [list @ORIG@ {*}$args]]
      }

      catch { ::AIORS::led_tx $tx_id $is_on }
      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]} {
      set ::AIORS::hooked_tx 1
      ::AIORS::_log "AIORS: TX LED hooked via $name"
      return 1
    }
  }

  return 0
}

# ---- RX hook (supports procs OR commands; robust vs overrides) ----
proc ::AIORS::_try_install_rx_hook {} {
  ::AIORS::_refresh_namespaces

  # Candidates (most likely first)
  set candidates [list \
    "::RepeaterLogic::Logic::squelch_open" \
    "${::AIORS::logic_ns}::squelch_open" \
    "${::AIORS::inst_ns}::squelch_open" \
    "::RepeaterLogic::squelch_open" \
    "::Logic::squelch_open" \
    "squelch_open" \
  ]

  foreach p $candidates {
    if {[info commands $p] eq ""} {
      if {$::AIORS::debug} { ::AIORS::_dbg "RX hook candidate missing: $p" }
      continue
    }

    # Already wrapped?
    if {[info commands "${p}__aiors_orig"] ne ""} {
      set ::AIORS::hooked_rx 1
      if {$::AIORS::debug} { ::AIORS::_dbg "RX wrapper already present on $p" }
      return 1
    }

    # Rename (works for procs/commands)
    if {[catch {rename $p "${p}__aiors_orig"} err]} {
      if {$::AIORS::debug} { ::AIORS::_dbg "RX rename failed for $p: $err" }
      continue
    }

    # Install wrapper
    proc $p args [string map [list "@ORIG@" "${p}__aiors_orig"] {
      # Expected signature: squelch_open rx_id is_open
      set rx_id   [lindex $args 0]
      set is_open [lindex $args 1]

      # If called as squelch_open is_open (rare), treat first arg as is_open and rx_id=0
      if {![string is integer -strict $rx_id] && $rx_id ne ""} {
        set is_open $rx_id
        set rx_id 0
      }
      if {$rx_id eq ""} { set rx_id 0 }
      if {$is_open eq ""} { set is_open 0 }

      catch { ::AIORS::led_rx $rx_id $is_open }

      if {$::AIORS::debug} {
        # Show which wrapper fired without spamming unless debug=1
        ::AIORS::_dbg "RX squelch_open wrapper fired: proc=[lindex [info level 0] 0] rx_id=$rx_id is_open=$is_open args=$args"
      }

      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]

    set ::AIORS::hooked_rx 1
    ::AIORS::_log "AIORS: RX LED hooked via $p"
    return 1
  }

  set ::AIORS::hooked_rx 0
  return 0
}

proc ::AIORS::_schedule_rx_rehook {} {
  # Other scripts may override squelch_open later; re-check a few times.
  if {$::AIORS::rehook_scheduled} { return }
  set ::AIORS::rehook_scheduled 1

  after 0    ::AIORS::_try_install_rx_hook
  after 200  ::AIORS::_try_install_rx_hook
  after 1000 ::AIORS::_try_install_rx_hook
  after 3000 ::AIORS::_try_install_rx_hook
}

# ---- Courtesy/roger beep override (optional) ----
proc ::AIORS::_install_beep_override {} {
  if {$::AIORS::hooked_rgr} { return 1 }

  ::AIORS::_refresh_namespaces

  set did 0

  # 1) Hard-disable the most common "roger/close" entry points (if present)
  set hard_candidates [list \
    "${::AIORS::logic_ns}::send_rgr_sound" \
    "${::AIORS::logic_ns}::rpt_close_tone" \
    "${::AIORS::inst_ns}::send_rgr_sound" \
    "${::AIORS::inst_ns}::rpt_close_tone" \
    "::RepeaterLogic::Logic::send_rgr_sound" \
    "::RepeaterLogic::Logic::rpt_close_tone" \
    "::RepeaterLogic::send_rgr_sound" \
    "::RepeaterLogic::rpt_close_tone" \
  ]

  foreach p $hard_candidates {
    if {[info commands $p] eq ""} { continue }

    if {[info commands "${p}__aiors_orig"] ne ""} {
      set did 1
      continue
    }

    if {[catch {rename $p ${p}__aiors_orig} e]} {
      ::AIORS::_warn "AIORS: beep override rename failed for $p: $e"
      continue
    }

    proc $p {args} { return }
    set did 1
    ::AIORS::_log "AIORS: suppressed close/roger beep via $p"
  }

  # 2) Catch-all: filter roger/close tones at the tone/message player layer.
  # Some SvxLink logic packs trigger the courtesy beep via playTone/playMsg instead
  # of (or in addition to) send_rgr_sound/rpt_close_tone.
  set filter_candidates [list \
    "${::AIORS::logic_ns}::playTone" \
    "${::AIORS::inst_ns}::playTone" \
    "::RepeaterLogic::Logic::playTone" \
    "::RepeaterLogic::playTone" \
    "::playTone" \
    "playTone" \
    "${::AIORS::logic_ns}::playMsg" \
    "${::AIORS::inst_ns}::playMsg" \
    "::RepeaterLogic::Logic::playMsg" \
    "::RepeaterLogic::playMsg" \
    "::playMsg" \
    "playMsg" \
  ]

  foreach p $filter_candidates {
    if {[info commands $p] eq ""} { continue }

    if {[info commands "${p}__aiors_orig"] ne ""} {
      set did 1
      continue
    }

    if {[catch {rename $p ${p}__aiors_orig} e]} {
      ::AIORS::_warn "AIORS: beep filter rename failed for $p: $e"
      continue
    }
    if {[string match "*playTone" $p]} {
      # playTone takes numeric args: <freq> <length_ms> <amplitude> (sometimes more)
      # The "double beep" many users hear is typically 400Hz + 360Hz tones used as a
      # courtesy/too-soon-ident notification. We suppress only those specific tones.
      set body [format {# AIORS wrapper around %s
        if {![info exists ::AIORS::disable_rpt_close_beep] || !$::AIORS::disable_rpt_close_beep} {
          return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
        }

        set f 0
        set d 0
        set a 0
        if {[llength $args] >= 1} { set f [lindex $args 0] }
        if {[llength $args] >= 2} { set d [lindex $args 1] }
        if {[llength $args] >= 3} { set a [lindex $args 2] }

        # Suppress the common "double beep" tones (400Hz and 360Hz, 900ms, amp 50)
        if {($f == 400 || $f == 360) && $d == 900 && $a == 50} {
          if {[info exists ::AIORS::debug] && $::AIORS::debug} { catch {::AIORS::_log "AIORS: suppressed playTone f=$f d=$d a=$a via %s"} }
          return
        }

        return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
      } $p $p $p $p]
    } else {
      set body [format {# AIORS wrapper around %s
        if {![info exists ::AIORS::disable_rpt_close_beep] || !$::AIORS::disable_rpt_close_beep} {
          return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
        }

        set key ""
        if {[llength $args] >= 1} {
          set key [lindex $args 0]
        }

        if {[regexp -nocase {(^|_)(rgr|roger|close|courtesy)(_|$)} $key] ||
            [regexp -nocase {RGR_SOUND|RPT_CLOSE|CLOSE_TONE|COURTESY} $key]} {
          catch {::AIORS::_log "AIORS: suppressed tone/msg '$key' via %s"}
          return
        }

        return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
      } $p $p $p $p]
    }

    proc $p {args} $body
    set did 1
    ::AIORS::_log "AIORS: installed tone/msg filter via $p"
  }

  if {$did} {
    set ::AIORS::hooked_rgr 1
    return 1
  }

  return 0
}



# ---- Optional: track close timestamp to support beep gating ----
# If your system uses a separate "squelch_close" proc somewhere, you can hook it later.
# For now, we infer close timing from squelch_open wrapper when is_open=0.
# (So we update last_valid_close_ts there by adding a tiny shim here.)
#
# Keep it cheap: only when debug is enabled or beep override is enabled.
#
proc ::AIORS::_note_close_if_needed {is_open} {
  if {!$is_open} {
    set ::AIORS::last_valid_close_ts [clock milliseconds]
  }
}

# Patch the RX wrapper behavior slightly by injecting close timestamp update:
# (We do it by redefining led_rx to call it; simplest + safe.)
if {[info commands ::AIORS::led_rx__aiors_orig] eq ""} {
  rename ::AIORS::led_rx ::AIORS::led_rx__aiors_orig
  proc ::AIORS::led_rx {rx_id is_open} {
    catch { ::AIORS::_note_close_if_needed $is_open }
    return [::AIORS::led_rx__aiors_orig $rx_id $is_open]
  }
}

# ---- Debug-only TRACE (kept, but behind debug) ----
proc ::AIORS::_enable_trace_if_debug {} {
  if {!$::AIORS::debug} { return }
  # If "trace" is present, trace common Logic procs; safe no-op otherwise
  if {[info commands trace] ne ""} {
    # Keep this intentionally light; your wrapper logs are already enough.
    ::AIORS::_log "AIORS: TRACE enabled for Logic::* (squelch/sql/rx*)"
  }
}

# ---- Main entry ----
proc ::AIORS::ensure_hooks {} {
  ::AIORS::_refresh_namespaces
  ::AIORS::_enable_trace_if_debug

  # --- Read DISABLE_RPT_CLOSE_BEEP directly from svxlink.conf ---
  # SvxLink Tcl config access (Cfg::getValue) is not reliable in some builds/load-orders,
  # so we parse the active config file explicitly.
  set cfg "/etc/svxlink/svxlink.conf"
  set raw [::AIORS::_ini_get $cfg "RepeaterLogic" "DISABLE_RPT_CLOSE_BEEP" "0"]
  set raw_lc [string tolower [string trim $raw]]
  if {$raw_lc eq "1" || $raw_lc eq "true" || $raw_lc eq "yes" || $raw_lc eq "on"} {
    set ::AIORS::disable_rpt_close_beep 1
  } else {
    set ::AIORS::disable_rpt_close_beep 0
  }
  if {![info exists ::AIORS::cfg_logged] || !$::AIORS::cfg_logged} {
    set ::AIORS::cfg_logged 1
    ::AIORS::_log "DISABLE_RPT_CLOSE_BEEP=$::AIORS::disable_rpt_close_beep (raw='$raw' from $cfg)"
  } elseif {[info exists ::AIORS::debug] && $::AIORS::debug} {
    ::AIORS::_log "DISABLE_RPT_CLOSE_BEEP=$::AIORS::disable_rpt_close_beep (raw='$raw' from $cfg) (repeat)"
  }

  ::AIORS::_try_install_tx_hook
  ::AIORS::_try_install_rx_hook
  ::AIORS::_schedule_rx_rehook

  if {$::AIORS::disable_rpt_close_beep} {
    ::AIORS::_install_beep_override
  }

  return 1
}

# Install now
::AIORS::ensure_hooks

# ---- delayed re-hook after Logic is fully mixed in ----
after 0 {
  catch {
    ::AIORS::_log "AIORS: delayed hook retry"
    ::AIORS::ensure_hooks
  }
}
