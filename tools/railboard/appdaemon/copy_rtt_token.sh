#!/bin/sh
# Copy the Realtime Trains token from Home Assistant's secrets.yaml into
# AppDaemon's, ON the Home Assistant host, without it being shown or leaving
# the host. Run by the owner from the repository:
#
#   ssh nickohome 'sudo sh -s' < tools/railboard/appdaemon/copy_rtt_token.sh
#
# It reads the one rtt_bearer: line, drops the quotes and "Bearer ", and
# appends  rtt_refresh_token: "<token>"  to AppDaemon's secrets.yaml after a
# dated backup of that file. It prints only a length. If the key is already
# there it changes nothing. The value moves through shell variables and a pipe
# into builtins, never through a command line another process could read.
#
# HA_SECRETS and AD_SECRETS override the two paths (the host test uses them).
set -eu
HA=${HA_SECRETS:-/config/secrets.yaml}
AD=${AD_SECRETS:-/addon_configs/a0d7b954_appdaemon/secrets.yaml}
KEY=rtt_refresh_token

[ -r "$HA" ] || { echo "cannot read $HA"; exit 1; }
[ -f "$AD" ] && [ -w "$AD" ] || { echo "cannot write $AD"; exit 1; }
if grep -q "^$KEY:" "$AD"; then
  echo "$KEY is already in $AD - nothing changed"
  exit 0
fi
n=$(grep -c '^rtt_bearer:' "$HA" || true)
[ "$n" = 1 ] || { echo "expected one rtt_bearer: line in $HA, found $n - nothing changed"; exit 1; }

line=$(grep '^rtt_bearer:' "$HA")
v=${line#rtt_bearer:}
v=$(printf '%s' "$v" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//')
case "$v" in
  \"*\") v=${v#\"}; v=${v%\"} ;;
  \'*\') v=${v#\'}; v=${v%\'} ;;
esac
v=${v#Bearer }
case "$v" in
  ""|*[!A-Za-z0-9._~+/=-]*) echo "the value is empty or has characters this script will not copy - do it by hand"; exit 1 ;;
esac

cp -p "$AD" "$AD.bak-$(date +%Y%m%d-%H%M%S)"
printf '%s: "%s"\n' "$KEY" "$v" >> "$AD"
echo "copied $KEY into $AD: ${#v} chars (backup next to it)"
