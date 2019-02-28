#!/bin/sh
#
# PROVIDE: dlinktrapd
# REQUIRE: DAEMON
# KEYWORD: shutdown
#
# Add the following line to /etc/rc.conf to enable dlinktrapd:
#
# dlinktrapd_enable="YES"
#

dlinktrapd_enable=${dlinktrapd_enable-"NO"}

. /etc/rc.subr

name=dlinktrapd
rcvar=`set_rcvar`

command=/usr/local/sbin/dlinktrapd
pidfile="/var/run/${name}.pid"

load_rc_config ${name}
run_rc_command "$1"
