'use strict';
'require network';

// FM160 ECM 数据链路协议（模组自拨号 + 模组侧 DHCP/RA）。
// 拨号参数（v4 / v6 / 自动拨号）由「服务 → FM160 → 拨号」页统一管理，
// 这里只负责让接口在 LuCI 正常显示/编辑。
return network.registerProtocol('ecm_fm160', {
	getI18n: function() {
		return _('FM160 ECM');
	},

	getIfname: function() {
		return this._ubus('l3_device') || 'usb0';
	},

	getPackageName: function() {
		return 'luci-proto-ecm-fm160';
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
		return (network.getIfnameOf(ifname) == this.getIfname());
	},

	renderFormOptions: function(s) {
		// v4 / v6 / 自动拨号在 FM160 拨号页配置
	}
});
