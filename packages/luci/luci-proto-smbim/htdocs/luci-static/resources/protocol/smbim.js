'use strict';
'require form';
'require network';
'require rpc';

var callFileList = rpc.declare({
	object: 'file',
	method: 'list',
	params: [ 'path' ],
	expect: { entries: [] },
	filter: function(list, params) {
		return list.filter(function(entry) {
			return entry.name.match(/^(cdc-wdm|wwan[0-9]+mbim)/);
		}).map(function(entry) {
			return params.path + entry.name;
		}).sort();
	}
});

network.registerPatternVirtual(/^smbim-.+$/);

return network.registerProtocol('smbim', {
	getI18n: function() {
		return _('sMBIM Cellular');
	},

	getIfname: function() {
		return this._ubus('l3_device') || 'smbim-%s'.format(this.sid);
	},

	getPackageName: function() {
		return 'smbim';
	},

	isFloating: function() {
		return true;
	},

	isVirtual: function() {
		return true;
	},

	getDevices: function() {
		return null;
	},

	containsDevice: function(ifname) {
		return network.getIfnameOf(ifname) == this.getIfname();
	},

	renderFormOptions: function(s) {
		var o;

		o = s.taboption('general', form.Value, '_modem_device', _('Modem device'));
		o.ucioption = 'device';
		o.rmempty = false;
		o.load = function(section_id) {
			return callFileList('/dev/').then(L.bind(function(devices) {
				devices.forEach(L.bind(function(device) { this.value(device); }, this));
				return form.Value.prototype.load.apply(this, [ section_id ]);
			}, this));
		};

		o = s.taboption('general', form.Value, 'apn', _('APN'));
		o.rmempty = false;
		o.validate = function(section_id, value) {
			return /^[a-zA-Z0-9.-]*[a-zA-Z0-9]$/.test(value) ? true : _('Invalid APN provided');
		};

		o = s.taboption('general', form.Value, 'pincode', _('PIN'));
		o.datatype = 'and(uinteger,minlength(4),maxlength(8))';
		o.password = true;

		o = s.taboption('general', form.ListValue, 'auth', _('Authentication type'));
		o.value('none', _('None'));
		o.value('pap', 'PAP');
		o.value('chap', 'CHAP');
		o.default = 'none';

		o = s.taboption('general', form.Value, 'username', _('Authentication username'));
		o.depends('auth', 'pap');
		o.depends('auth', 'chap');

		o = s.taboption('general', form.Value, 'password', _('Authentication password'));
		o.depends('auth', 'pap');
		o.depends('auth', 'chap');
		o.password = true;

		o = s.taboption('general', form.ListValue, 'pdptype', _('PDP type'));
		o.value('ipv4v6', _('IPv4/IPv6'));
		o.value('ipv4', _('IPv4'));
		o.value('ipv6', _('IPv6'));
		o.default = 'ipv4v6';

		o = s.taboption('advanced', form.Flag, 'allow_roaming', _('Allow roaming'));
		o.default = o.disabled;

		o = s.taboption('advanced', form.Flag, 'allow_partner', _('Allow partner networks'));
		o.default = o.disabled;

		o = s.taboption('advanced', form.Flag, 'proxy', _('Use mbim-proxy'));
		o.default = o.enabled;

		o = s.taboption('advanced', form.Value, 'retries', _('MBIM retry count'),
		_('Each MBIM response has a fixed one-second timeout.'));
		o.datatype = 'range(1,20)';
		o.default = '5';

		o = s.taboption('advanced', form.Value, 'poll_interval', _('IP sanity poll interval'));
		o.datatype = 'min(1)';
		o.default = '30';

		o = s.taboption('advanced', form.Value, 'mtu', _('Override MTU'));
		o.datatype = 'range(576,9200)';

		o = s.taboption('advanced', form.Flag, 'defaultroute', _('Use default gateway'));
		o.default = o.enabled;

		o = s.taboption('advanced', form.Value, 'metric', _('Use gateway metric'));
		o.datatype = 'uinteger';
		o.placeholder = '0';
		o.depends('defaultroute', '1');

		o = s.taboption('advanced', form.Flag, 'peerdns', _('Use DNS servers advertised by modem'));
		o.default = o.enabled;

		o = s.taboption('advanced', form.DynamicList, 'dns', _('Use custom DNS servers'));
		o.datatype = 'ipaddr';

		if (L.hasSystemFeature('ipv6')) {
			o = s.taboption('advanced', form.Flag, 'delegate', _('Delegate IPv6 prefixes'));
			o.default = o.enabled;

			o = s.taboption('advanced', form.Flag, 'sourcefilter', _('IPv6 source routing'));
			o.default = o.enabled;
		}
	}
});
