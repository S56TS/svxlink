###############################################################################
#
# SimplexLogic event handlers
#
###############################################################################

#
# This is the namespace in which all functions below will exist. The name
# must match the corresponding section "[SimplexLogic]" in the configuration
# file. The name may be changed but it must be changed in both places.
#
namespace eval SimplexLogic {

#
# Checking to see if this is the correct logic core
#
if {$logic_name != [namespace tail [namespace current]]} {
  return;
}

#
# A variable used to time for restart timer for FRN module [sec].
#
variable frn_timer_sec 0;


#
# A variable used to time for WDS watchdog signal [min].
#
variable wds_timer_min 0;

#
# User variables
#
set led_state 0;
set gpio_state(0) 0;
set gpio_state(1) 0;
set gpio_state(2) 0;
set gpio_state(3) 0;
set gpio_state(4) 0;
set gpio_str_bsy "echo: write error: Device or resource busy"

#
# Executed when the SvxLink software is started
#
proc startup {} {
  global logic_name;
  global wds_time_min;
  global frn_time_sec;
  variable frn_timer_sec;
  variable wds_timer_min;
  variable gpio_state;
  variable gpio_str_bsy;
  set res(0) 0;
  set res(1) 0;
  set res(2) 0;
  set res(3) 0;
  set res(4) 0;
  set GPIO_NOINIT 0:
  set GPIO_INITOK 1;
  set GPIO_INITER 2;
  append func $logic_name "::checkPeriodicIdentify";
  Logic::addMinuteTickSubscriber $func;
  Logic::startup;

  # run FRN module
  set wds_timer_min $wds_time_min;
  set frn_timer_sec $frn_time_sec;

  # initilize led's
  set res(0) [catch {
    exec echo 17 > /sys/class/gpio/export
  } res_str(0)]

  set res(1) [catch {
    exec echo 22 > /sys/class/gpio/export
  } res_str(1)]

  set res(2) [catch {
    exec echo 27 > /sys/class/gpio/export
  } res_str(2)]

  set res(3) [catch {
    exec echo 23 > /sys/class/gpio/export
  } res_str(3)]

  set res(4) [catch {
    exec echo 24 > /sys/class/gpio/export
  } res_str(4)]


  for {set i 0} {$i < 5} {incr i} {
    if {$res($i) != 0} {
        if {[string equal $res_str($i) $gpio_str_bsy] == 1} {
            set gpio_state($i) $GPIO_INITOK;
        } else {
            set gpio_state($i) $GPIO_INITER;
        }
    } else {
            set gpio_state($i) $GPIO_INITOK;
    }
  }

  if {$gpio_state(0) == $GPIO_INITOK} {
    if {[catch {exec sudo echo out > /sys/class/gpio/gpio17/direction} result] == 0} {
      if {[catch {exec echo 0 > /sys/class/gpio/gpio17/value} result] != 0} {
        set gpio_state(0) $GPIO_INITER;
        puts "GPIO0 value init error"
      }
    } else {
     set gpio_state(0) $GPIO_INITER;
      puts "GPIO0 direction init error, error: $result"
    }
  } else {
    set gpio_state(0) $GPIO_INITER;
    puts "GPIO0 export init error"
  }

  if {$gpio_state(1) == $GPIO_INITOK} {
    if {[catch {exec echo out > /sys/class/gpio/gpio22/direction} result] == 0} {
      if {[catch {exec echo 0 > /sys/class/gpio/gpio22/value} result] != 0} {
        set gpio_state(1) $GPIO_INITER;
        puts "GPIO1 value init error"
      }
    } else {
     set gpio_state(1) $GPIO_INITER;
      puts "GPIO1 direction init error"
    }
  } else {
    set gpio_state(1) $GPIO_INITER;
    puts "GPIO1 export init error"
  }

  if {$gpio_state(2) == $GPIO_INITOK} {
    if {[catch {exec echo out > /sys/class/gpio/gpio27/direction} result] == 0} {
      if {[catch {exec echo 0 > /sys/class/gpio/gpio27/value} result] != 0} {
        set gpio_state(2) $GPIO_INITER;
        puts "GPIO2 value init error"
      }
    } else {
     set gpio_state(2) $GPIO_INITER;
      puts "GPIO2 direction init error"
    }
  } else {
    set gpio_state(2) $GPIO_INITER;
    puts "GPIO2 export init error"
  }

  if {$gpio_state(3) == $GPIO_INITOK} {
    if {[catch {exec echo out > /sys/class/gpio/gpio23/direction} result] == 0} {
      if {[catch {exec echo 0 > /sys/class/gpio/gpio23/value} result] != 0} {
        set gpio_state(3) $GPIO_INITER;
        puts "GPIO2 value init error"
      }
    } else {
     set gpio_state(3) $GPIO_INITER;
      puts "GPIO2 direction init error"
    }
  } else {
    set gpio_state(3) $GPIO_INITER;
    puts "GPIO2 export init error"
  }

  if {$gpio_state(4) == $GPIO_INITOK} {
    if {[catch {exec echo in > /sys/class/gpio/gpio24/direction} result] == 0} {
    } else {
     set gpio_state(4) $GPIO_INITER;
      puts "GPIO3 direction init error"
    }
  } else {
    set gpio_state(4) $GPIO_INITER;
    puts "GPIO3 export init error"
  }
}


#
# Executed when a specified module could not be found
#
proc no_such_module {module_id} {
  Logic::no_such_module $module_id;
}


#
# Executed when a manual identification is initiated with the * DTMF code
#
proc manual_identification {} {
  Logic::manual_identification;
}


#
# Executed when the squelch just have closed and the RGR_SOUND_DELAY timer has
# expired.
#
proc send_rgr_sound {} {
  Logic::send_rgr_sound;
}


#
# Executed when an empty macro command (i.e. D#) has been entered.
#
proc macro_empty {} {
  Logic::macro_empty;
}


#
# Executed when an entered macro command could not be found
#
proc macro_not_found {} {
  Logic::macro_not_found;
}


#
# Executed when a macro syntax error occurs (configuration error).
#
proc macro_syntax_error {} {
  Logic::macro_syntax_error;
}


#
# Executed when the specified module in a macro command is not found
# (configuration error).
#
proc macro_module_not_found {} {
  Logic::macro_module_not_found;
}


#
# Executed when the activation of the module specified in the macro command
# failed.
#
proc macro_module_activation_failed {} {
  Logic::macro_module_activation_failed;
}


#
# Executed when a macro command is executed that requires a module to
# be activated but another module is already active.
#
proc macro_another_active_module {} {
  Logic::macro_another_active_module;
}


#
# Executed when an unknown DTMF command is entered
#
proc unknown_command {cmd} {
  Logic::unknown_command $cmd;
}


#
# Executed when an entered DTMF command failed
#
proc command_failed {cmd} {
  Logic::command_failed $cmd;
}


#
# Executed when a link to another logic core is activated.
#   name  - The name of the link
#
proc activating_link {name} {
  Logic::activating_link $name;
}


#
# Executed when a link to another logic core is deactivated.
#   name  - The name of the link
#
proc deactivating_link {name} {
  Logic::deactivating_link $name;
}


#
# Executed when trying to deactivate a link to another logic core but the
# link is not currently active.
#   name  - The name of the link
#
proc link_not_active {name} {
  Logic::link_not_active $name;
}


#
# Executed when trying to activate a link to another logic core but the
# link is already active.
#   name  - The name of the link
#
proc link_already_active {name} {
  Logic::link_already_active $name;
}


#
# Executed once every whole minute
#
proc every_minute {} {
  global wds_time_min;
  variable wds_timer_min;

  if {$wds_timer_min > 0} {
    set wds_timer_min [expr $wds_timer_min - 1];
    if {$wds_timer_min == 0} {
      puts "SimplexLogic: WDS signal"
      set wds_timer_min  $wds_time_min;
    }
  }
  Logic::every_minute;
}


#
# Executed once every second
#
proc every_second {} {
  global active_module;
  global frn_time_sec;
  variable frn_timer_sec;
  variable led_state;
  variable gpio_state;
  set GPIO_NOINIT 0:
  set GPIO_INITOK 1;
  set GPIO_INITER 2;

  if {$active_module == ""} {
    if {$frn_timer_sec > 0} {
      set frn_timer_sec [expr $frn_timer_sec - 1];
      if {$frn_timer_sec == 0} {
        puts "SimplexLogic: FRN restart timer elapsed. No active module, activating FRN module."
        set frn_timer_sec $frn_time_sec;
        injectDtmf 2#;
      }
    }
  } else {
    set frn_timer_sec $frn_time_sec;
  }

  if {$gpio_state(1) == $GPIO_INITOK} {
    if {$led_state == 0} {
      set led_state 1;
      exec echo 1 >/sys/class/gpio/gpio22/value;
    } else {
      set led_state 0;
      exec echo 0 >/sys/class/gpio/gpio22/value;
    }
  }
  Logic::every_second;
}


#
# Executed each time the transmitter is turned on or off
#
proc transmit {is_on} {
 variable gpio_state;
 set GPIO_NOINIT 0:
 set GPIO_INITOK 1;
 set GPIO_INITER 2;

 Logic::transmit $is_on;

 if {$is_on == 0 && $gpio_state(0) == $GPIO_INITOK} {exec echo 0 >/sys/class/gpio/gpio17/value}
 if {$is_on == 1 && $gpio_state(0) == $GPIO_INITOK} {exec echo 1 >/sys/class/gpio/gpio17/value}

# if {$is_on == 1} {puts "Tx Led: ON"}
# if {$is_on == 0} {puts "Tx Led: OFF"}
}


#
# Executed each time the squelch is opened or closed
#
proc squelch_open {rx_id is_open} {
 variable gpio_state;
 set GPIO_NOINIT 0:
 set GPIO_INITOK 1;
 set GPIO_INITER 2;

 Logic::squelch_open $rx_id $is_open;

 if {$is_open == 0 && $gpio_state(2) == $GPIO_INITOK} {exec echo 0 >/sys/class/gpio/gpio27/value}
 if {$is_open == 1 && $gpio_state(2) == $GPIO_INITOK} {exec echo 1 >/sys/class/gpio/gpio27/value}

# if {$is_open == 1} {puts "Rx Led: ON"}
# if {$is_open == 0} {puts "Rx Led: OFF"}
}


#
# Executed once every whole minute to check if it's time to identify
#
proc checkPeriodicIdentify {} {
  Logic::checkPeriodicIdentify;
}


#
# Executed when a DTMF digit has been received
#   digit     - The detected DTMF digit
#   duration  - The duration, in milliseconds, of the digit
#
# Return 1 to hide the digit from further processing in SvxLink or
# return 0 to make SvxLink continue processing as normal.
#
proc dtmf_digit_received {digit duration} {
  global frn_time_sec;
  variable frn_timer_sec;
  
  set frn_timer_sec $frn_time_sec;
  return [Logic::dtmf_digit_received $digit $duration];
}


#
# Executed when a DTMF command has been received
#   cmd - The command
#
# Return 1 to hide the command from further processing is SvxLink or
# return 0 to make SvxLink continue processing as normal.
#
proc dtmf_cmd_received {cmd} {
  global frn_time_sec;
  variable frn_timer_sec;
  
  set frn_timer_sec $frn_time_sec;
  return [Logic::dtmf_cmd_received $cmd];
}


#
# Executed when the QSO recorder is being activated
#
proc activating_qso_recorder {} {
  Logic::activating_qso_recorder;
}


#
# Executed when the QSO recorder is being deactivated
#
proc deactivating_qso_recorder {} {
  Logic::deactivating_qso_recorder;
}


#
# Executed when trying to deactivate the QSO recorder even though it's
# not active
#
proc qso_recorder_not_active {} {
  Logic::qso_recorder_not_active;
}


#
# Executed when trying to activate the QSO recorder even though it's
# already active
#
proc qso_recorder_already_active {} {
  Logic::qso_recorder_already_active;
}


#
# Executed when the timeout kicks in to activate the QSO recorder
#
proc qso_recorder_timeout_activate {} {
  Logic::qso_recorder_timeout_activate
}


#
# Executed when the timeout kicks in to deactivate the QSO recorder
#
proc qso_recorder_timeout_deactivate {} {
  Logic::qso_recorder_timeout_deactivate
}


#
# Executed when the user is requesting a language change
#
proc set_language {lang_code} {
  Logic::set_language "$lang_code";
}


#
# Executed when the user requests a list of available languages
#
proc list_languages {} {
  Logic::list_languages
}


#
# Executed when the node is being brought online after being offline
#
proc logic_online {online} {
  Logic::logic_online $online
}


#
# Executed when a configuration variable is updated at runtime in the logic
# core
#
proc config_updated {tag value} {
  Logic::config_updated "$tag" "$value"
}


#
# Executed when a DTMF command is received from another linked logic core
#
#   logic -- The name of the logic core
#   cmd   -- The received command
#
proc remote_cmd_received {logic cmd} {
  Logic::remote_cmd_received "$logic" "$cmd"
}


#
# Executed when a talkgroup is received from another linked logic core
#
#   logic -- The name of the logic core
#   tg    -- The received talkgroup
#
proc remote_received_tg_updated {logic tg} {
  Logic::remote_received_tg_updated "$logic" "$tg"
}


# end of namespace
}


#
# This file has not been truncated
#
