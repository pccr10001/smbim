#!/bin/sh

[ -n "$INCLUDE_ONLY" ] || {
	. /lib/functions.sh
	. ../netifd-proto.sh
	init_proto "$@"
}

proto_smbim_init_config() {
	available=1
	no_device=1
	proto_config_add_string "device:device"
	proto_config_add_string apn
	proto_config_add_string pincode
	proto_config_add_string auth
	proto_config_add_string username
	proto_config_add_string password
	proto_config_add_string pdptype
	proto_config_add_boolean allow_roaming
	proto_config_add_boolean allow_partner
	proto_config_add_boolean proxy
	proto_config_add_int retries
	proto_config_add_int poll_interval
	proto_config_add_boolean delegate
	proto_config_add_boolean sourcefilter
	proto_config_add_int mtu
	proto_config_add_array "dns:list(string)"
	proto_config_add_defaults
}

proto_smbim_setup() {
	local interface="$1"
	local device apn pincode auth username password pdptype
	local allow_roaming allow_partner proxy retries poll_interval
	local delegate sourcefilter mtu defaultroute peerdns metric dns
	local devname devpath ifname args

	json_get_vars device apn pincode auth username password pdptype
	json_get_vars allow_roaming allow_partner proxy retries poll_interval
	json_get_vars delegate sourcefilter mtu defaultroute peerdns metric
	json_get_values dns dns

	[ -n "$device" ] || {
		proto_notify_error "$interface" NO_DEVICE
		return 1
	}
	[ -n "$apn" ] || {
		proto_notify_error "$interface" NO_APN
		return 1
	}

	devname="${device##*/}"
	devpath="$(readlink -f "/sys/class/usbmisc/$devname/device" 2>/dev/null || readlink -f "/sys/class/wwan/$devname/device" 2>/dev/null)"
	ifname="$(ls "$devpath/net" 2>/dev/null | head -n 1)"

	[ -n "$auth" ] || auth=none
	[ -n "$pdptype" ] || pdptype=ipv4v6
	[ -n "$proxy" ] || proxy=1
	[ -n "$retries" ] || retries=5
	[ -n "$poll_interval" ] || poll_interval=30
	[ -n "$defaultroute" ] || defaultroute=1
	[ -n "$peerdns" ] || peerdns=1
	[ -n "$delegate" ] || delegate=1
	[ -n "$sourcefilter" ] || sourcefilter=1

	set -- /usr/bin/smbimd \
		--interface "$interface" --device "$device" --ifname "$ifname" \
		--apn "$apn" --auth "$auth" \
		--ip-type "$pdptype" --retries "$retries" --poll-interval "$poll_interval" \
		--metric "${metric:-0}" --dns "$dns" --mtu "${mtu:-0}"
	proto_export "SMBIM_PIN=$pincode"
	proto_export "SMBIM_USERNAME=$username"
	proto_export "SMBIM_PASSWORD=$password"
	[ "$proxy" = 1 ] || set -- "$@" --no-proxy
	[ "$defaultroute" = 1 ] || set -- "$@" --no-default-route
	[ "$peerdns" = 1 ] || set -- "$@" --no-peer-dns
	[ "$delegate" = 1 ] || set -- "$@" --no-delegate
	[ "$sourcefilter" = 1 ] || set -- "$@" --no-source-filter
	[ "$allow_roaming" = 1 ] && set -- "$@" --allow-roaming
	[ "$allow_partner" = 1 ] && set -- "$@" --allow-partner

	proto_run_command "$interface" "$@"
}

proto_smbim_teardown() {
	proto_kill_command "$1"
}

[ -n "$INCLUDE_ONLY" ] || add_protocol smbim
